#include "clip-trim.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <cstdio>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring result(static_cast<size_t>(len), 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), result.data(), len);
	return result;
}

// CRT rename() refuses to overwrite an existing destination on Windows, unlike
// POSIX. MoveFileExW with MOVEFILE_REPLACE_EXISTING does the atomic replace
// directly and needs no separate remove() step (which can itself transiently
// fail with a sharing violation right after another process just wrote the
// file, e.g. antivirus/indexer scans, OBS's own muxer closing its handle).
bool ReplaceFile(const std::string &tempPath, const std::string &path)
{
	const std::wstring wTemp = Utf8ToWide(tempPath);
	const std::wstring wPath = Utf8ToWide(path);
	for (int attempt = 0; attempt < 10; attempt++) {
		if (MoveFileExW(wTemp.c_str(), wPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		Sleep(200);
	}
	return false;
}

// Same idea, but MOVEFILE_COPY_ALLOWED is required whenever source and
// destination can be on different volumes (e.g. a RAM disk -> a real SSD) --
// plain MoveFileExW fails across volumes without it.
bool MoveFileAcrossVolumes(const std::string &srcPath, const std::string &destPath)
{
	const std::wstring wSrc = Utf8ToWide(srcPath);
	const std::wstring wDest = Utf8ToWide(destPath);
	for (int attempt = 0; attempt < 10; attempt++) {
		if (MoveFileExW(wSrc.c_str(), wDest.c_str(),
				 MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
			return true;
		Sleep(200);
	}
	return false;
}
#else
bool ReplaceFile(const std::string &tempPath, const std::string &path)
{
	return rename(tempPath.c_str(), path.c_str()) == 0;
}

bool MoveFileAcrossVolumes(const std::string &srcPath, const std::string &destPath)
{
	if (rename(srcPath.c_str(), destPath.c_str()) == 0)
		return true;
	// rename() fails across filesystems on POSIX too; fall back to copy+remove.
	FILE *in = fopen(srcPath.c_str(), "rb");
	if (!in)
		return false;
	FILE *out = fopen(destPath.c_str(), "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	char buf[1 << 16];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	}
	fclose(in);
	fclose(out);
	if (ok)
		remove(srcPath.c_str());
	else
		remove(destPath.c_str());
	return ok;
}
#endif

std::string BaseName(const std::string &path)
{
	const size_t pos = path.find_last_of("/\\");
	return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Joins a directory and filename, tolerating a trailing slash on the directory.
std::string JoinPath(const std::string &dir, const std::string &name)
{
	if (dir.empty())
		return name;
	const char last = dir.back();
	if (last == '/' || last == '\\')
		return dir + name;
#ifdef _WIN32
	return dir + "\\" + name;
#else
	return dir + "/" + name;
#endif
}

struct InputGuard {
	AVFormatContext *ctx = nullptr;
	~InputGuard()
	{
		if (ctx)
			avformat_close_input(&ctx);
	}
};

struct OutputGuard {
	AVFormatContext *ctx = nullptr;
	bool openedFile = false;
	~OutputGuard()
	{
		if (ctx) {
			if (openedFile && ctx->pb)
				avio_closep(&ctx->pb);
			avformat_free_context(ctx);
		}
	}
};

// Moves `path` to destDir (if non-empty) and returns the final path, or
// `path` unchanged if destDir is empty. Returns an empty string only if a
// move was requested and actually failed.
std::string FinishAt(const std::string &path, const std::string &destDir)
{
	if (destDir.empty())
		return path;
	const std::string destPath = JoinPath(destDir, BaseName(path));
	if (destPath == path)
		return path;
	if (!MoveFileAcrossVolumes(path, destPath))
		return std::string();
	return destPath;
}

} // namespace

std::string TrimReplayToLastSeconds(const std::string &path, int seconds, const std::string &destDir)
{
	if (seconds <= 0 || path.empty())
		return std::string();

	InputGuard in;
	if (avformat_open_input(&in.ctx, path.c_str(), nullptr, nullptr) < 0)
		return std::string();
	if (avformat_find_stream_info(in.ctx, nullptr) < 0)
		return std::string();

	const int64_t totalDuration = in.ctx->duration; // AV_TIME_BASE units (microseconds)
	if (totalDuration <= 0)
		return std::string();

	const int64_t wantedDuration = static_cast<int64_t>(seconds) * AV_TIME_BASE;
	if (wantedDuration >= totalDuration) {
		// Already shorter than the requested clip length -- nothing to trim,
		// but still honor a requested move off e.g. a RAM disk.
		avformat_close_input(&in.ctx);
		return FinishAt(path, destDir);
	}

	const int64_t seekTarget = totalDuration - wantedDuration;
	if (av_seek_frame(in.ctx, -1, seekTarget, AVSEEK_FLAG_BACKWARD) < 0)
		return std::string();

	const std::string tempPath = path + ".trimtmp";

	OutputGuard out;
	// Guess the container from the real path's extension -- tempPath ends in
	// ".trimtmp", which libavformat can't map to a format on its own.
	avformat_alloc_output_context2(&out.ctx, nullptr, nullptr, path.c_str());
	if (!out.ctx)
		return std::string();

	std::vector<int> streamMapping(in.ctx->nb_streams, -1);
	int nextOutIndex = 0;
	for (unsigned i = 0; i < in.ctx->nb_streams; i++) {
		AVStream *inStream = in.ctx->streams[i];
		const enum AVMediaType type = inStream->codecpar->codec_type;
		if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
			continue;

		AVStream *outStream = avformat_new_stream(out.ctx, nullptr);
		if (!outStream)
			return std::string();
		if (avcodec_parameters_copy(outStream->codecpar, inStream->codecpar) < 0)
			return std::string();
		outStream->codecpar->codec_tag = 0;
		outStream->time_base = inStream->time_base;
		streamMapping[i] = nextOutIndex++;
	}
	if (nextOutIndex == 0)
		return std::string();

	if (!(out.ctx->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&out.ctx->pb, tempPath.c_str(), AVIO_FLAG_WRITE) < 0)
			return std::string();
		out.openedFile = true;
	}

	if (avformat_write_header(out.ctx, nullptr) < 0) {
		// avio_open above already created tempPath on disk (out.openedFile is
		// true) -- OutputGuard's destructor closes the AVIOContext but was
		// never told to remove() the file it created, unlike the packet-loop
		// failure path below. Close the handle first (an open handle from
		// this same process can make even our own later remove() fail on
		// Windows, same reasoning as ReplaceFile's own comment).
		if (out.openedFile && out.ctx->pb) {
			avio_closep(&out.ctx->pb);
			out.openedFile = false;
		}
		remove(tempPath.c_str());
		return std::string();
	}

	// Every stream shares ONE reference point -- the real seek target (in
	// AV_TIME_BASE units), converted into each stream's own time_base --
	// rather than each stream picking its own first packet observed after
	// the seek as its zero point. av_seek_frame only lands at/near the
	// target, and keyframe-driven seeking means the video and audio
	// streams' actual first post-seek packets essentially never share the
	// same real timestamp, so zeroing each stream independently baked a
	// constant, stream-dependent audio/video sync offset into every
	// trimmed clip. pts and dts still get shifted by the SAME (per-stream)
	// offset -- using different offsets for pts vs dts breaks their
	// ordering on B-frame streams (dts stays monotonic and precedes pts)
	// and libavformat rejects the resulting packets with "pts < dts in
	// stream N" -- this only changes what that per-stream offset is
	// derived from.
	std::vector<int64_t> offset(in.ctx->nb_streams, 0);
	for (unsigned i = 0; i < in.ctx->nb_streams; i++) {
		if (streamMapping[i] < 0)
			continue;
		offset[i] = av_rescale_q(seekTarget, AV_TIME_BASE_Q, in.ctx->streams[i]->time_base);
	}

	AVPacket pkt;
	bool ok = true;
	while (av_read_frame(in.ctx, &pkt) >= 0) {
		const int inIdx = pkt.stream_index;
		if (inIdx < 0 || static_cast<unsigned>(inIdx) >= in.ctx->nb_streams || streamMapping[inIdx] < 0) {
			av_packet_unref(&pkt);
			continue;
		}

		AVStream *inStream = in.ctx->streams[inIdx];
		AVStream *outStream = out.ctx->streams[streamMapping[inIdx]];

		// The NOPTS guard has to cover the rescale too, not just the offset
		// subtraction -- av_rescale_q_rnd doesn't know AV_NOPTS_VALUE
		// (INT64_MIN) is a sentinel, not a real timestamp, and will happily
		// "rescale" it into some large-magnitude garbage value, silently
		// destroying the "no timestamp" information a legitimately
		// NOPTS packet (dts-less audio, some passthrough containers) relies
		// on downstream muxers/readers to respect.
		if (pkt.pts != AV_NOPTS_VALUE) {
			pkt.pts = std::max<int64_t>(0, pkt.pts - offset[inIdx]);
			pkt.pts = av_rescale_q_rnd(pkt.pts, inStream->time_base, outStream->time_base,
						  static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		}
		if (pkt.dts != AV_NOPTS_VALUE) {
			pkt.dts = std::max<int64_t>(0, pkt.dts - offset[inIdx]);
			pkt.dts = av_rescale_q_rnd(pkt.dts, inStream->time_base, outStream->time_base,
						  static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		}
		pkt.duration = av_rescale_q(pkt.duration, inStream->time_base, outStream->time_base);
		pkt.pos = -1;
		pkt.stream_index = streamMapping[inIdx];

		if (av_interleaved_write_frame(out.ctx, &pkt) < 0) {
			ok = false;
			av_packet_unref(&pkt);
			break;
		}
		av_packet_unref(&pkt);
	}

	if (ok)
		ok = av_write_trailer(out.ctx) >= 0;

	if (out.openedFile && out.ctx->pb) {
		avio_closep(&out.ctx->pb);
		out.openedFile = false;
	}

	if (!ok) {
		remove(tempPath.c_str());
		return std::string();
	}

	// Release our own read handle on the source file before replacing it --
	// on Windows a still-open handle from this same process can make the
	// replace below fail with a sharing violation.
	avformat_close_input(&in.ctx);

	if (!ReplaceFile(tempPath, path)) {
		// Best effort: the trimmed file exists at tempPath even if the replace failed.
		return std::string();
	}
	return FinishAt(path, destDir);
}
