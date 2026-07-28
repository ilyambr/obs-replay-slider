#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>

#include "replay-buffer-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return;

	auto *dock = new ReplayBufferDock(mainWindow);
	obs_frontend_add_dock_by_id("ReplayBufferDock", obs_module_text("ReplayBufferDock"), dock);
}

void obs_module_unload(void)
{
	obs_frontend_remove_dock("ReplayBufferDock");
	obs_log(LOG_INFO, "plugin unloaded");
}
