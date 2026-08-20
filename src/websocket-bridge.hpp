#pragma once

#include <QString>

class ReplayBufferDock;
class ControlPanelDock;

// Bridges this plugin's rows (the main OBS replay buffer, plus every
// discovered Source Record filter) to obs-websocket as a "replay-buffer-slider"
// vendor, so an external tool can ask what buffers exist and trigger a save on
// one specific row -- exactly what the dock's own Save buttons already do,
// just reachable over the wire instead of only from the dock's UI. Also
// bridges ControlPanelDock's per-filter Record rows under the same vendor.
//
// Vendor requests:
//   list_rows -> { "rows": [ { key, label, is_main, length_seconds, hotkey,
//                  status (0 grey / 1 green / 2 red), dest_dir }, ... ], "success": true }
//   save_row  { "key": <row key> } -> { "success": bool, "error"?: string }
//   set_row_length { "key": <row key>, "seconds": int } -> { "success": bool, "error"?: string }
//   set_dest_dir { "path": <string> } -> { "success": bool } -- "" clears it;
//   applies to every row that doesn't have its own set_row_dest_dir override
//   set_row_dest_dir { "key": <row key>, "path": <string> } -> { "success": bool,
//   "error"?: string } -- per-row override of set_dest_dir above, e.g. a
//   distinct subfolder for just this buffer's clips. "" clears back to the
//   shared set_dest_dir value. Persisted by this row's label, so it re-applies
//   to a same-named filter discovered again later (a new OBS session gives
//   filters a new, unrelated key).
//   set_buffer_duration { "seconds": int } -> { "success": bool } -- pushes a
//   Source Record filter's own replay_duration (how much it buffers and
//   flushes to disk on save), applied to every tracked filter and remembered
//   for any discovered later. Does not touch the main OBS replay buffer.
//
//   list_record_rows -> { "rows": [ { key, label, status, source, filter }, ... ],
//   "success": true } -- one row per discovered Source Record filter (see
//   ControlPanelDock), independent of list_rows above: no "main" row here,
//   since ControlPanelDock itself doesn't track the main OBS recording, only
//   per-filter ones. status is 0 Inactive (currently unused -- see
//   control-panel-dock.cpp's UpdateRow for why the "source has nothing to
//   capture" check this was meant for got reverted), 1 Stopped (record_mode
//   off), 2 Recording, or 3 Error (record_mode was on and the output stopped
//   with a failure). source/filter are the parent source's and the filter's
//   own names -- use these (not label) to look up this filter's settings
//   (e.g. its configured output path) via the plain obs-websocket
//   GetSourceFilterList/SetSourceFilterSettings requests.
//   start_record_row { "key": <row key> } -> { "success": bool, "error"?: string }
//   stop_record_row  { "key": <row key> } -> { "success": bool, "error"?: string }
//   -- same as clicking that row's Record button in ControlPanelDock itself.
//
// Vendor events:
//   row_saving { "key": <row key> } -- emitted the instant OBS itself reports
//   a row's raw (untrimmed) replay buffer file has landed on disk, right
//   before TrimAndReplace's own background trim thread starts -- fires
//   regardless of what triggered the save (this dock's own Save button, a
//   hotkey bound directly in OBS, or a save_row request), since all three
//   paths funnel through the exact same OBS signal handler. There is no
//   equivalent "about to start" moment before THIS -- the raw file genuinely
//   doesn't exist yet before it, so there's nothing earlier to announce.
//   row_saved { "key": <row key>, "path": <string> } -- emitted once a row's
//   buffer actually finishes saving (main or filter) -- i.e. once the trim
//   above completes -- same moment the dock's own status bar message kicks
//   in. Every row_saving is followed by exactly one row_saved for that same
//   key, whether or not the trim itself succeeded (see TrimAndReplace's own
//   comment on the trim-failure fallback).
//
// Safe no-op if obs-websocket isn't installed or hasn't loaded yet.
namespace WebsocketBridge {

void Register(ReplayBufferDock *dock, ControlPanelDock *controlDock);
void Unregister();

void EmitRowSaving(const QString &rowKey);
void EmitRowSaved(const QString &rowKey, const QString &path);

} // namespace WebsocketBridge
