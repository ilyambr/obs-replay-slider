#pragma once

#include <QString>

class ReplayBufferDock;

// Bridges this plugin's rows (the main OBS replay buffer, plus every
// discovered Source Record filter) to obs-websocket as a "replay-buffer-slider"
// vendor, so an external tool can ask what buffers exist and trigger a save on
// one specific row -- exactly what the dock's own Save buttons already do,
// just reachable over the wire instead of only from the dock's UI.
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
// Vendor event:
//   row_saved { "key": <row key>, "path": <string> } -- emitted once a row's
//   buffer actually finishes saving (main or filter), same moment the dock's
//   own status bar message / trim kicks in.
//
// Safe no-op if obs-websocket isn't installed or hasn't loaded yet.
namespace WebsocketBridge {

void Register(ReplayBufferDock *dock);
void Unregister();

void EmitRowSaved(const QString &rowKey, const QString &path);

} // namespace WebsocketBridge
