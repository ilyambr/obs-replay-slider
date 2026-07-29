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
#else
bool ReplaceFile(const std::string &tempPath, const std::string &path)
{
	return rename(tempPath.c_str(), path.c_str()) == 0;
}
#endif

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

} // namespace

bool TrimReplayToLastSeconds(const std::string &path, int seconds)
{
	if (seconds <= 0 || path.empty())
		return false;

	InputGuard in;
	if (avformat_open_input(&in.ctx, path.c_str(), nullptr, nullptr) < 0)
		return false;
	if (avformat_find_stream_info(in.ctx, nullptr) < 0)
		return false;

	const int64_t totalDuration = in.ctx->duration; // AV_TIME_BASE units (microseconds)
	if (totalDuration <= 0)
		return false;

	const int64_t wantedDuration = static_cast<int64_t>(seconds) * AV_TIME_BASE;
	if (wantedDuration >= totalDuration)
		return true; // Already shorter than the requested clip length; nothing to do.

	const int64_t seekTarget = totalDuration - wantedDuration;
	if (av_seek_frame(in.ctx, -1, seekTarget, AVSEEK_FLAG_BACKWARD) < 0)
		return false;

	const std::string tempPath = path + ".trimtmp";

	OutputGuard out;
	// Guess the container from the real path's extension -- tempPath ends in
	// ".trimtmp", which libavformat can't map to a format on its own.
	avformat_alloc_output_context2(&out.ctx, nullptr, nullptr, path.c_str());
	if (!out.ctx)
		return false;

	std::vector<int> streamMapping(in.ctx->nb_streams, -1);
	int nextOutIndex = 0;
	for (unsigned i = 0; i < in.ctx->nb_streams; i++) {
		AVStream *inStream = in.ctx->streams[i];
		const enum AVMediaType type = inStream->codecpar->codec_type;
		if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
			continue;

		AVStream *outStream = avformat_new_stream(out.ctx, nullptr);
		if (!outStream)
			return false;
		if (avcodec_parameters_copy(outStream->codecpar, inStream->codecpar) < 0)
			return false;
		outStream->codecpar->codec_tag = 0;
		outStream->time_base = inStream->time_base;
		streamMapping[i] = nextOutIndex++;
	}
	if (nextOutIndex == 0)
		return false;

	if (!(out.ctx->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&out.ctx->pb, tempPath.c_str(), AVIO_FLAG_WRITE) < 0)
			return false;
		out.openedFile = true;
	}

	if (avformat_write_header(out.ctx, nullptr) < 0)
		return false;

	// Each stream's first packet after the seek becomes its new zero point.
	// pts and dts must be shifted by the SAME offset per stream -- using
	// independent offsets breaks the pts/dts ordering on B-frame streams
	// (dts stays monotonic and precedes pts) and libavformat rejects the
	// resulting packets with "pts < dts in stream N".
	std::vector<int64_t> offset(in.ctx->nb_streams, AV_NOPTS_VALUE);

	AVPacket pkt;
	bool ok = true;
	while (av_read_frame(in.ctx, &pkt) >= 0) {
		const int inIdx = pkt.stream_index;
		if (inIdx < 0 || static_cast<unsigned>(inIdx) >= in.ctx->nb_streams || streamMapping[inIdx] < 0) {
			av_packet_unref(&pkt);
			continue;
		}

		if (offset[inIdx] == AV_NOPTS_VALUE) {
			if (pkt.dts != AV_NOPTS_VALUE)
				offset[inIdx] = pkt.dts;
			else if (pkt.pts != AV_NOPTS_VALUE)
				offset[inIdx] = pkt.pts;
			else
				offset[inIdx] = 0;
		}

		AVStream *inStream = in.ctx->streams[inIdx];
		AVStream *outStream = out.ctx->streams[streamMapping[inIdx]];

		if (pkt.pts != AV_NOPTS_VALUE)
			pkt.pts = std::max<int64_t>(0, pkt.pts - offset[inIdx]);
		if (pkt.dts != AV_NOPTS_VALUE)
			pkt.dts = std::max<int64_t>(0, pkt.dts - offset[inIdx]);

		pkt.pts = av_rescale_q_rnd(pkt.pts, inStream->time_base, outStream->time_base,
					  static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		pkt.dts = av_rescale_q_rnd(pkt.dts, inStream->time_base, outStream->time_base,
					  static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
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
		return false;
	}

	// Release our own read handle on the source file before replacing it --
	// on Windows a still-open handle from this same process can make the
	// replace below fail with a sharing violation.
	avformat_close_input(&in.ctx);

	if (!ReplaceFile(tempPath, path)) {
		// Best effort: the trimmed file exists at tempPath even if the replace failed.
		return false;
	}
	return true;
}
