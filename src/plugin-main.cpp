#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMainWindow>

#include "replay-buffer-dock.hpp"
#include "version.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("replay-slider", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Slide the replay buffer duration (30s-15min) for the built-in replay buffer and every "
	       "Source Record filter, with live status and hotkey display.";
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Replay Slider] loaded version %s", PROJECT_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return;

	auto *dock = new ReplayBufferDock(mainWindow);
	obs_frontend_add_dock(dock);
}

void obs_module_unload(void)
{
	// obs_frontend_add_dock() hands ownership to the main window's Qt object tree,
	// so it will be destroyed along with everything else on shutdown; nothing to do here.
	blog(LOG_INFO, "[Replay Slider] unloaded");
}
