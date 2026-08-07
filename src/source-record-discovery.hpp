#pragma once

#include <obs.h>

#include <QString>

#include <vector>

// Shared "find every source_record_filter instance in the scene collection"
// helper, used by every dock that lists rows per filter (ReplayBufferDock,
// ControlPanelDock, and whatever gets added next). Originally lived only in
// replay-buffer-dock.cpp; pulled out here once a second dock needed the same
// discovery logic rather than copy-pasting it.

// Returns one owned weak ref per distinct "source_record_filter" instance
// found. Caller takes ownership of each returned weak ref.
std::vector<obs_weak_source_t *> DiscoverSourceRecordFilters();

// "<parent source name> - <filter name>", or just the filter name if for some
// reason it has no parent.
QString FilterRowLabel(obs_source_t *filterSource);
