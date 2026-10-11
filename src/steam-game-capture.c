// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <pthread.h>
#include <string.h>

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "sgc-log.h"
#include "sgc-pipewire.h"
#include "sgc-x11.h"

#define S_STATUS "status"
#define S_APP_ID "app_id"
#define S_REFRESH "refresh"
#define S_FPS "fps"
#define S_ONLY_WHEN_SHOWING "only_when_showing"
#define S_CAPTURE_AUDIO "capture_audio"
#define S_AUDIO_MATCH "audio_match"
#define S_DISPLAY "display"

#define T_(text) obs_module_text(text)

#define MAX_GAMES 32
#define SCAN_INTERVAL_NS 2000000000ULL
#define RECONNECT_INTERVAL_NS 3000000000ULL
#define IDLE_WAIT_MS 100

enum capture_status {
	STATUS_STARTING,
	STATUS_NO_DISPLAY,
	STATUS_WAITING,
	STATUS_CAPTURING,
};

struct steam_game_capture {
	obs_source_t *source;

	pthread_t thread;
	bool thread_created;
	os_event_t *stop_event;

	/* Everything below is protected by mutex. */
	pthread_mutex_t mutex;

	uint32_t app_id;
	uint32_t fps;
	bool only_when_showing;
	char *display;
	char *audio_match;
	bool settings_dirty;

	bool audio_wanted;
	struct sgc_pw_audio *audio;
	uint32_t game_pids[2];

	enum capture_status status;
	struct sgc_window_info game;
};

static const char *sgc_get_name(void *type_data)
{
	(void)type_data;
	return T_("SteamGameCapture");
}

static void on_audio(void *param, const float *samples, uint32_t frames, uint32_t rate, uint64_t timestamp_ns)
{
	struct steam_game_capture *s = param;
	struct obs_source_audio audio = {
		.data[0] = (const uint8_t *)samples,
		.frames = frames,
		.speakers = SPEAKERS_STEREO,
		.format = AUDIO_FORMAT_FLOAT,
		.samples_per_sec = rate,
		.timestamp = timestamp_ns,
	};

	obs_source_output_audio(s->source, &audio);
}

/* mutex must be held */
static void apply_audio_targets(struct steam_game_capture *s)
{
	if (s->audio)
		sgc_pw_audio_set_targets(s->audio, s->game_pids, 2, s->audio_match);
}

static void set_game(struct steam_game_capture *s, const struct sgc_window_info *game, enum capture_status status)
{
	pthread_mutex_lock(&s->mutex);
	if (game) {
		s->game = *game;
	} else {
		memset(&s->game, 0, sizeof(s->game));
	}
	s->game_pids[0] = s->game.pid;
	s->game_pids[1] = s->game.xres_pid;
	s->status = status;
	apply_audio_targets(s);
	pthread_mutex_unlock(&s->mutex);
}

static void set_status(struct steam_game_capture *s, enum capture_status status)
{
	pthread_mutex_lock(&s->mutex);
	s->status = status;
	pthread_mutex_unlock(&s->mutex);
}

static bool should_switch(const struct sgc_window_info *current, const struct sgc_window_info *games, size_t count,
			  int best, uint32_t app_id)
{
	if (!current->window)
		return true;
	if (games[best].window == current->window)
		return false;
	if (app_id && current->app_id != app_id)
		return true;

	for (size_t i = 0; i < count; i++) {
		if (games[i].window != current->window)
			continue;

		/* Prefer a visible, bigger window (e.g. game replacing its launcher). */
		const struct sgc_window_info *b = &games[best];
		uint64_t cur_area = (uint64_t)games[i].width * games[i].height;
		uint64_t best_area = (uint64_t)b->width * b->height;
		if (!games[i].viewable)
			return b->viewable;
		return b->viewable && best_area > cur_area;
	}

	/* Current window no longer looks like a Steam game. */
	return true;
}

static void output_frame(struct steam_game_capture *s, const struct sgc_x11_frame *f)
{
	struct obs_source_frame frame = {
		.data[0] = (uint8_t *)f->data,
		.linesize[0] = f->linesize,
		.width = f->width,
		.height = f->height,
		.timestamp = os_gettime_ns(),
		.format = VIDEO_FORMAT_BGRX,
		.full_range = true,
	};

	obs_source_output_video(s->source, &frame);
}

static void lose_game(struct steam_game_capture *s, struct sgc_x11 *x, struct sgc_window_info *current,
		      enum capture_status status)
{
	if (current->window)
		blog(LOG_INFO, "[steam-game-capture] Stopped capturing '%s' (AppID %u)", current->title,
		     current->app_id);

	if (x)
		sgc_x11_detach(x);
	memset(current, 0, sizeof(*current));
	set_game(s, NULL, status);
	obs_source_output_video(s->source, NULL);
}

static void scan_for_game(struct steam_game_capture *s, struct sgc_x11 *x, struct sgc_window_info *current,
			  uint32_t app_id)
{
	struct sgc_window_info games[MAX_GAMES];
	size_t count = sgc_x11_find_games(x, games, MAX_GAMES);
	int best = sgc_x11_pick_game(games, count, app_id);

	if (best < 0) {
		if (current->window)
			lose_game(s, x, current, STATUS_WAITING);
		else
			set_status(s, STATUS_WAITING);
		return;
	}

	if (!should_switch(current, games, count, best, app_id))
		return;

	*current = games[best];
	sgc_x11_attach(x, current->window);
	set_game(s, current, STATUS_CAPTURING);
	blog(LOG_INFO, "[steam-game-capture] Capturing '%s' (AppID %u, window 0x%x, pid %u)", current->title,
	     current->app_id, current->window, current->pid ? current->pid : current->xres_pid);
}

static void *capture_thread(void *data)
{
	struct steam_game_capture *s = data;
	struct sgc_x11 *x = NULL;
	struct sgc_window_info current = {0};
	char *display = NULL;
	bool warned_display = false;
	uint64_t next_connect = 0;
	uint64_t next_scan = 0;
	uint64_t next_frame = os_gettime_ns();

	os_set_thread_name("steam-game-capture");

	while (os_event_try(s->stop_event) == EAGAIN) {
		pthread_mutex_lock(&s->mutex);
		bool dirty = s->settings_dirty;
		bool display_changed = dirty && strcmp(display ? display : "", s->display) != 0;
		uint32_t app_id = s->app_id;
		uint32_t fps = s->fps;
		bool only_when_showing = s->only_when_showing;
		s->settings_dirty = false;
		if (display_changed) {
			bfree(display);
			display = bstrdup(s->display);
		}
		pthread_mutex_unlock(&s->mutex);

		uint64_t now = os_gettime_ns();

		if (display_changed && x) {
			lose_game(s, x, &current, STATUS_STARTING);
			sgc_x11_destroy(x);
			x = NULL;
			next_connect = 0;
		}
		if (dirty)
			next_scan = 0;

		if (!x && now >= next_connect) {
			x = sgc_x11_create(display);
			if (x) {
				warned_display = false;
				next_scan = 0;
			} else {
				if (!warned_display)
					blog(LOG_WARNING, "[steam-game-capture] Cannot open X11 display '%s'",
					     display && *display ? display : "$DISPLAY");
				warned_display = true;
				set_status(s, STATUS_NO_DISPLAY);
				next_connect = now + RECONNECT_INTERVAL_NS;
			}
		}

		if (x && now >= next_scan) {
			next_scan = now + SCAN_INTERVAL_NS;
			scan_for_game(s, x, &current, app_id);
		}

		if (!x || !current.window) {
			os_event_timedwait(s->stop_event, IDLE_WAIT_MS);
			next_frame = os_gettime_ns();
			continue;
		}

		if (!only_when_showing || obs_source_showing(s->source)) {
			struct sgc_x11_frame frame;

			switch (sgc_x11_grab(x, &frame)) {
			case SGC_GRAB_OK:
				output_frame(s, &frame);
				break;
			case SGC_GRAB_UNAVAILABLE:
				break;
			case SGC_GRAB_GONE:
				lose_game(s, x, &current, STATUS_WAITING);
				next_scan = 0;
				break;
			case SGC_GRAB_ERROR:
				blog(LOG_WARNING, "[steam-game-capture] Lost connection to X server");
				lose_game(s, x, &current, STATUS_NO_DISPLAY);
				sgc_x11_destroy(x);
				x = NULL;
				next_connect = now + RECONNECT_INTERVAL_NS;
				break;
			}
		}

		next_frame += 1000000000ULL / fps;
		now = os_gettime_ns();
		if (next_frame <= now)
			next_frame = now; /* fell behind, don't try to catch up */
		else
			os_sleepto_ns(next_frame);
	}

	if (x)
		lose_game(s, x, &current, STATUS_STARTING);
	sgc_x11_destroy(x);
	bfree(display);
	return NULL;
}

static void sgc_update(void *data, obs_data_t *settings)
{
	struct steam_game_capture *s = data;
	bool want_audio = obs_data_get_bool(settings, S_CAPTURE_AUDIO);
	long long fps = obs_data_get_int(settings, S_FPS);

	pthread_mutex_lock(&s->mutex);

	s->app_id = (uint32_t)obs_data_get_int(settings, S_APP_ID);
	s->fps = (uint32_t)(fps < 1 ? 1 : fps > 240 ? 240 : fps);
	s->only_when_showing = obs_data_get_bool(settings, S_ONLY_WHEN_SHOWING);
	bfree(s->display);
	s->display = bstrdup(obs_data_get_string(settings, S_DISPLAY));
	bfree(s->audio_match);
	s->audio_match = bstrdup(obs_data_get_string(settings, S_AUDIO_MATCH));
	s->settings_dirty = true;
	s->audio_wanted = want_audio;

	if (want_audio && !s->audio) {
		struct obs_audio_info oai;
		uint32_t rate = obs_get_audio_info(&oai) ? oai.samples_per_sec : 48000;
		s->audio = sgc_pw_audio_create(rate, on_audio, s);
	} else if (!want_audio && s->audio) {
		sgc_pw_audio_destroy(s->audio);
		s->audio = NULL;
	}
	apply_audio_targets(s);

	pthread_mutex_unlock(&s->mutex);
}

static void *sgc_create(obs_data_t *settings, obs_source_t *source)
{
	struct steam_game_capture *s = bzalloc(sizeof(*s));

	s->source = source;
	pthread_mutex_init(&s->mutex, NULL);
	if (os_event_init(&s->stop_event, OS_EVENT_TYPE_MANUAL) != 0) {
		pthread_mutex_destroy(&s->mutex);
		bfree(s);
		return NULL;
	}

	sgc_update(s, settings);

	s->thread_created = pthread_create(&s->thread, NULL, capture_thread, s) == 0;
	if (!s->thread_created)
		blog(LOG_ERROR, "[steam-game-capture] Failed to start capture thread");
	return s;
}

static void sgc_destroy(void *data)
{
	struct steam_game_capture *s = data;

	if (s->thread_created) {
		os_event_signal(s->stop_event);
		pthread_join(s->thread, NULL);
	}

	sgc_pw_audio_destroy(s->audio);
	bfree(s->display);
	bfree(s->audio_match);
	os_event_destroy(s->stop_event);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static void sgc_get_defaults(obs_data_t *settings)
{
	struct obs_video_info ovi;
	long long fps = 60;

	if (obs_get_video_info(&ovi) && ovi.fps_den)
		fps = ovi.fps_num / ovi.fps_den;

	obs_data_set_default_int(settings, S_APP_ID, 0);
	obs_data_set_default_int(settings, S_FPS, fps);
	obs_data_set_default_bool(settings, S_ONLY_WHEN_SHOWING, true);
	obs_data_set_default_bool(settings, S_CAPTURE_AUDIO, true);
	obs_data_set_default_string(settings, S_AUDIO_MATCH, "");
	obs_data_set_default_string(settings, S_DISPLAY, "");
}

static void describe_status(struct steam_game_capture *s, struct dstr *out)
{
	uint32_t streams = 0, links = 0;

	pthread_mutex_lock(&s->mutex);
	switch (s->status) {
	case STATUS_STARTING:
		dstr_copy(out, T_("Status.Starting"));
		break;
	case STATUS_NO_DISPLAY:
		dstr_copy(out, T_("Status.NoDisplay"));
		break;
	case STATUS_WAITING:
		dstr_copy(out, T_("Status.Waiting"));
		break;
	case STATUS_CAPTURING:
		dstr_printf(out, "%s: %s (AppID %u)", T_("Status.Capturing"), s->game.title, s->game.app_id);
		break;
	}

	if (s->audio) {
		sgc_pw_audio_get_status(s->audio, &streams, &links);
		dstr_catf(out, " — %s: %u", T_("Status.AudioStreams"), streams);
	} else if (s->audio_wanted) {
		dstr_catf(out, " — %s", T_("Status.NoPipeWire"));
	}
	pthread_mutex_unlock(&s->mutex);
}

static void fill_game_list(struct steam_game_capture *s, obs_property_t *list)
{
	obs_data_t *settings = obs_source_get_settings(s->source);
	uint32_t selected = (uint32_t)obs_data_get_int(settings, S_APP_ID);
	bool selected_listed = selected == 0;
	struct sgc_window_info games[MAX_GAMES];
	size_t count = 0;
	struct dstr name = {0};

	obs_property_list_add_int(list, T_("Game.Auto"), 0);

	struct sgc_x11 *x = sgc_x11_create(obs_data_get_string(settings, S_DISPLAY));
	if (x) {
		count = sgc_x11_find_games(x, games, MAX_GAMES);
		sgc_x11_destroy(x);
	}

	for (size_t i = 0; i < count; i++) {
		bool duplicate = false;
		for (size_t j = 0; j < i; j++)
			duplicate |= games[j].app_id == games[i].app_id;
		if (duplicate)
			continue;

		dstr_printf(&name, "%s (AppID %u)", games[i].title, games[i].app_id);
		obs_property_list_add_int(list, name.array, games[i].app_id);
		selected_listed |= games[i].app_id == selected;
	}

	/* Keep a saved selection visible even when that game is not running. */
	if (!selected_listed) {
		dstr_printf(&name, "AppID %u (%s)", selected, T_("Game.NotRunning"));
		obs_property_list_add_int(list, name.array, selected);
	}

	dstr_free(&name);
	obs_data_release(settings);
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct steam_game_capture *s = data;
	struct dstr status = {0};

	(void)property;

	obs_property_t *list = obs_properties_get(props, S_APP_ID);
	obs_property_list_clear(list);
	fill_game_list(s, list);

	describe_status(s, &status);
	obs_property_set_description(obs_properties_get(props, S_STATUS), status.array);
	dstr_free(&status);
	return true;
}

static obs_properties_t *sgc_properties(void *data)
{
	struct steam_game_capture *s = data;
	obs_properties_t *props = obs_properties_create();
	struct dstr status = {0};
	obs_property_t *p;

	describe_status(s, &status);
	obs_properties_add_text(props, S_STATUS, status.array, OBS_TEXT_INFO);
	dstr_free(&status);

	p = obs_properties_add_list(props, S_APP_ID, T_("Game"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	fill_game_list(s, p);
	obs_properties_add_button2(props, S_REFRESH, T_("Refresh"), refresh_clicked, s);

	obs_properties_add_int_slider(props, S_FPS, T_("FPS"), 1, 240, 1);
	obs_properties_add_bool(props, S_ONLY_WHEN_SHOWING, T_("OnlyWhenShowing"));

	obs_properties_add_bool(props, S_CAPTURE_AUDIO, T_("CaptureAudio"));
	p = obs_properties_add_text(props, S_AUDIO_MATCH, T_("AudioMatch"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, T_("AudioMatch.Description"));

	p = obs_properties_add_text(props, S_DISPLAY, T_("Display"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, T_("Display.Description"));

	return props;
}

struct obs_source_info steam_game_capture_info = {
	.id = "steam_game_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
	.get_name = sgc_get_name,
	.create = sgc_create,
	.destroy = sgc_destroy,
	.update = sgc_update,
	.get_defaults = sgc_get_defaults,
	.get_properties = sgc_properties,
};
