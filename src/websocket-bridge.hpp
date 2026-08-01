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
//                  status (0 grey / 1 green / 2 red) }, ... ], "success": true }
//   save_row  { "key": <row key> } -> { "success": bool, "error"?: string }
//   set_row_length { "key": <row key>, "seconds": int } -> { "success": bool, "error"?: string }
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
