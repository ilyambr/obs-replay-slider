#pragma once

#include <obs.h>
#include <QString>

// Finds the (single) hotkey the "replay_buffer" output type registers for itself
// and returns its bound key combination as a display string (e.g. "F8"), or an
// empty string if the output has no hotkey or it is unbound.
//
// Only safe to call with an output pointer you currently hold a real reference
// to (e.g. from obs_frontend_get_replay_buffer_output()) -- this walks global
// hotkey state synchronously and does not itself manage the output's lifetime.
QString FindOutputHotkeyString(obs_output_t *output);
