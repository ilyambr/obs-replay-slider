#pragma once

#include <string>

// Trims the video file at `path` down to just its last `seconds` of content
// (stream copy, no re-encoding) and replaces the original file in place.
// If the file is already shorter than `seconds`, it's left untouched (still
// returns true). Returns false on any failure, leaving the original file
// untouched.
bool TrimReplayToLastSeconds(const std::string &path, int seconds);
