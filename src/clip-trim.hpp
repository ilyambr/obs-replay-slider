#pragma once

#include <string>

// Trims the video file at `path` down to just its last `seconds` of content
// (stream copy, no re-encoding), in place. If the file is already shorter
// than `seconds`, it's left untouched.
//
// If `destDir` is non-empty, the result (trimmed or not) is then moved there
// (keeping its original filename) -- meant for when `path` lives on a RAM
// disk and the caller wants the final clip to end up somewhere persistent
// instead of vanishing on reboot/power loss.
//
// Returns the clip's final path on success, or an empty string on failure
// (in which case the original file at `path` is left untouched).
std::string TrimReplayToLastSeconds(const std::string &path, int seconds, const std::string &destDir = std::string());
