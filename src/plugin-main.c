// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>

#include <obs-module.h>
#include <pipewire/pipewire.h>

#include "sgc-log.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("steam-game-capture", "en-US")

extern struct obs_source_info steam_game_capture_info;

static void log_to_obs(int level, const char *fmt, va_list args)
{
	char message[1024];

	vsnprintf(message, sizeof(message), fmt, args);
	blog(level, "[steam-game-capture] %s", message);
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Captures video and audio of Steam games running under XWayland/X11 with PipeWire audio.";
}

bool obs_module_load(void)
{
	sgc_set_log_handler(log_to_obs);
	pw_init(NULL, NULL);
	obs_register_source(&steam_game_capture_info);
	return true;
}

void obs_module_unload(void)
{
	pw_deinit();
}
