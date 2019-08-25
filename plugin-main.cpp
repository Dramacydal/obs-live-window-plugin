#include <windows.h>
#include <obs-module.h>
#include <util/windows/win-version.h>
#include <util/platform.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("live-window-capture", "en-US")

MODULE_EXPORT const char* obs_module_description(void)
{
	return "Windows live-window capture";
}

extern struct obs_source_info live_window_capture_info;

bool obs_module_load(void)
{
	obs_register_source(&live_window_capture_info);

	return true;
}

void obs_module_unload(void)
{
}
