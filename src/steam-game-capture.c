// SPDX-License-Identifier: Unlicense
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>
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
#define S_ONLY_WHEN_SHOWING "only_when_showing"
#define S_CAPTURE_AUDIO "capture_audio"
#define S_AUDIO_MATCH "audio_match"
#define S_DISPLAY "display"

#define T_(text) obs_module_text(text)

#define MAX_GAMES 32
#define SCAN_INTERVAL_NS 2000000000ULL
#define RECONNECT_INTERVAL_NS 3000000000ULL
#define SCAN_WAIT_MS 100
/* Imported swapchain buffers kept around; games typically use 2-4. */
#define IMPORT_CACHE_SIZE 8
/* Consecutive DMA-BUF failures before using the CPU path for a window. */
#define MAX_DMABUF_FAILURES 30

enum capture_status {
	STATUS_STARTING,
	STATUS_NO_DISPLAY,
	STATUS_WAITING,
	STATUS_CAPTURING,
};

enum capture_path {
	PATH_NONE,
	PATH_GPU,
	PATH_CPU,
};

struct imported_buffer {
	uint64_t id;
	gs_texture_t *texture;
	uint64_t last_used;
};

struct steam_game_capture {
	obs_source_t *source;

	/* Scan thread: finds the game window and keeps audio targets current. */
	pthread_t thread;
	bool thread_created;
	os_event_t *stop_event;

	/* Everything up to the graphics thread state is protected by mutex. */
	pthread_mutex_t mutex;

	uint32_t app_id;
	bool only_when_showing;
	char *display;
	char *audio_match;
	bool settings_dirty;
	bool rescan;

	bool audio_wanted;
	struct sgc_pw_audio *audio;
	uint32_t game_pids[2];

	enum capture_status status;
	enum capture_path path;
	struct sgc_window_info game;

	/* Graphics thread (video_tick/video_render) state. */
	struct sgc_x11 *gx;
	char *gx_display;
	uint64_t gx_next_connect;
	uint32_t attached_window;
	enum capture_path gx_path;
	bool force_cpu; /* SGC_DISABLE_DMABUF=1, for troubleshooting */
	bool dmabuf_disabled; /* for the attached window */
	uint32_t dmabuf_failures;

	gs_texture_t *texture; /* what video_render draws */
	enum gs_color_format texture_format;
	bool texture_dynamic;
	uint32_t width;
	uint32_t height;

	struct imported_buffer imports[IMPORT_CACHE_SIZE];
	uint32_t import_width;
	uint32_t import_height;
	uint32_t import_drm_format;
	uint64_t frame_counter;
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

/* mutex must be held */
static void set_game_locked(struct steam_game_capture *s, const struct sgc_window_info *game,
			    enum capture_status status)
{
	if (s->game.window && (!game || game->window != s->game.window))
		blog(LOG_INFO, "[steam-game-capture] Stopped capturing '%s' (AppID %u)", s->game.title,
		     s->game.app_id);

	if (game) {
		s->game = *game;
	} else {
		memset(&s->game, 0, sizeof(s->game));
	}
	s->game_pids[0] = s->game.pid;
	s->game_pids[1] = s->game.xres_pid;
	s->status = status;
	apply_audio_targets(s);
}

static void set_game(struct steam_game_capture *s, const struct sgc_window_info *game, enum capture_status status)
{
	pthread_mutex_lock(&s->mutex);
	set_game_locked(s, game, status);
	pthread_mutex_unlock(&s->mutex);
}

static void set_status(struct steam_game_capture *s, enum capture_status status)
{
	pthread_mutex_lock(&s->mutex);
	s->status = status;
	pthread_mutex_unlock(&s->mutex);
}

/* ------------------------------------------------------------------------ */
/* Scan thread                                                              */

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

static void scan_for_game(struct steam_game_capture *s, struct sgc_x11 *x, uint32_t app_id)
{
	struct sgc_window_info games[MAX_GAMES];
	size_t count = sgc_x11_find_games(x, games, MAX_GAMES);
	int best = sgc_x11_pick_game(games, count, app_id);

	pthread_mutex_lock(&s->mutex);
	if (best < 0) {
		set_game_locked(s, NULL, STATUS_WAITING);
	} else if (should_switch(&s->game, games, count, best, app_id)) {
		const struct sgc_window_info *g = &games[best];
		set_game_locked(s, g, STATUS_CAPTURING);
		blog(LOG_INFO, "[steam-game-capture] Capturing '%s' (AppID %u, window 0x%x, pid %u)", g->title,
		     g->app_id, g->window, g->pid ? g->pid : g->xres_pid);
	}
	pthread_mutex_unlock(&s->mutex);
}

static void *scan_thread(void *data)
{
	struct steam_game_capture *s = data;
	struct sgc_x11 *x = NULL;
	char *display = NULL;
	bool warned_display = false;
	uint64_t next_connect = 0;
	uint64_t next_scan = 0;

	os_set_thread_name("steam-game-capture-scan");

	do {
		pthread_mutex_lock(&s->mutex);
		bool rescan = s->settings_dirty || s->rescan;
		bool display_changed = s->settings_dirty && strcmp(display ? display : "", s->display) != 0;
		uint32_t app_id = s->app_id;
		s->settings_dirty = false;
		s->rescan = false;
		if (display_changed) {
			bfree(display);
			display = bstrdup(s->display);
		}
		pthread_mutex_unlock(&s->mutex);

		uint64_t now = os_gettime_ns();

		if (display_changed && x) {
			set_game(s, NULL, STATUS_STARTING);
			sgc_x11_destroy(x);
			x = NULL;
			next_connect = 0;
		}

		if (!x && now >= next_connect) {
			x = sgc_x11_create(display);
			if (x) {
				warned_display = false;
				rescan = true;
			} else {
				if (!warned_display)
					blog(LOG_WARNING, "[steam-game-capture] Cannot open X11 display '%s'",
					     display && *display ? display : "$DISPLAY");
				warned_display = true;
				set_status(s, STATUS_NO_DISPLAY);
				next_connect = now + RECONNECT_INTERVAL_NS;
			}
		}

		if (x && (rescan || now >= next_scan)) {
			next_scan = now + SCAN_INTERVAL_NS;
			scan_for_game(s, x, app_id);
		}
	} while (os_event_timedwait(s->stop_event, SCAN_WAIT_MS) == ETIMEDOUT);

	sgc_x11_destroy(x);
	bfree(display);
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Graphics thread                                                          */

/* Graphics context must be active. */
static void clear_imports(struct steam_game_capture *s)
{
	for (size_t i = 0; i < IMPORT_CACHE_SIZE; i++) {
		gs_texture_destroy(s->imports[i].texture);
		s->imports[i] = (struct imported_buffer){0};
	}
}

static void release_capture(struct steam_game_capture *s)
{
	obs_enter_graphics();
	clear_imports(s);
	gs_texture_destroy(s->texture);
	s->texture = NULL;
	obs_leave_graphics();

	s->width = 0;
	s->height = 0;
	if (s->gx && s->attached_window)
		sgc_x11_detach(s->gx);
	s->attached_window = 0;
}

static void set_path(struct steam_game_capture *s, enum capture_path path)
{
	if (s->gx_path == path)
		return;

	s->gx_path = path;
	pthread_mutex_lock(&s->mutex);
	s->path = path;
	pthread_mutex_unlock(&s->mutex);
}

/* Graphics context must be active. */
static bool ensure_texture(struct steam_game_capture *s, uint32_t width, uint32_t height,
			   enum gs_color_format format, bool dynamic)
{
	if (s->texture && s->width == width && s->height == height && s->texture_format == format &&
	    s->texture_dynamic == dynamic)
		return true;

	gs_texture_destroy(s->texture);
	s->texture = gs_texture_create(width, height, format, 1, NULL, dynamic ? GS_DYNAMIC : 0);
	s->texture_format = format;
	s->texture_dynamic = dynamic;
	s->width = s->texture ? width : 0;
	s->height = s->texture ? height : 0;
	return s->texture != NULL;
}

/* Graphics context must be active. */
static gs_texture_t *import_dmabuf(struct steam_game_capture *s, const struct sgc_dmabuf *buf,
				   enum gs_color_format format)
{
	struct imported_buffer *slot = &s->imports[0];

	/* A resize or format change invalidates every cached buffer. */
	if (buf->width != s->import_width || buf->height != s->import_height ||
	    buf->drm_format != s->import_drm_format) {
		clear_imports(s);
		s->import_width = buf->width;
		s->import_height = buf->height;
		s->import_drm_format = buf->drm_format;
	}

	for (size_t i = 0; i < IMPORT_CACHE_SIZE; i++) {
		struct imported_buffer *entry = &s->imports[i];
		if (entry->texture && entry->id == buf->buffer_id) {
			entry->last_used = s->frame_counter;
			return entry->texture;
		}
	}

	/* Not imported yet: take a free slot, or evict the least recently used. */
	for (size_t i = 0; i < IMPORT_CACHE_SIZE && slot->texture; i++) {
		struct imported_buffer *entry = &s->imports[i];
		if (!entry->texture || entry->last_used < slot->last_used)
			slot = entry;
	}

	uint64_t modifiers[SGC_MAX_PLANES];
	for (size_t i = 0; i < SGC_MAX_PLANES; i++)
		modifiers[i] = buf->modifier;

	gs_texture_t *texture = gs_texture_create_from_dmabuf(
		buf->width, buf->height, buf->drm_format, format, buf->n_planes, buf->fds, buf->strides, buf->offsets,
		buf->modifier != DRM_FORMAT_MOD_INVALID ? modifiers : NULL);
	if (!texture)
		return NULL;

	gs_texture_destroy(slot->texture);
	slot->id = buf->buffer_id;
	slot->texture = texture;
	slot->last_used = s->frame_counter;
	return texture;
}

static enum sgc_grab_result capture_gpu(struct steam_game_capture *s)
{
	struct sgc_dmabuf buf;
	enum sgc_grab_result res = sgc_x11_grab_dmabuf(s->gx, &buf);

	if (res != SGC_GRAB_OK)
		return res;

	enum gs_color_format format = buf.drm_format == DRM_FORMAT_XRGB2101010 ? GS_R10G10B10A2 : GS_BGRX;

	obs_enter_graphics();
	gs_texture_t *imported = import_dmabuf(s, &buf, format);
	/*
	 * Snapshot the buffer right away so the game can reuse it for the next
	 * frame without tearing what we render.
	 */
	if (imported && ensure_texture(s, buf.width, buf.height, format, false))
		gs_copy_texture(s->texture, imported);
	obs_leave_graphics();

	sgc_dmabuf_close(&buf);
	return imported ? SGC_GRAB_OK : SGC_GRAB_UNSUPPORTED;
}

static enum sgc_grab_result capture_cpu(struct steam_game_capture *s)
{
	struct sgc_x11_frame frame;
	enum sgc_grab_result res = sgc_x11_grab(s->gx, &frame);

	if (res != SGC_GRAB_OK)
		return res;

	obs_enter_graphics();
	if (ensure_texture(s, frame.width, frame.height, GS_BGRX, true))
		gs_texture_set_image(s->texture, frame.data, frame.linesize, false);
	obs_leave_graphics();
	return SGC_GRAB_OK;
}

static void window_gone(struct steam_game_capture *s, uint32_t window, enum capture_status status)
{
	release_capture(s);

	pthread_mutex_lock(&s->mutex);
	if (s->game.window == window)
		set_game_locked(s, NULL, status);
	s->rescan = true;
	pthread_mutex_unlock(&s->mutex);
}

static void sgc_video_tick(void *data, float seconds)
{
	struct steam_game_capture *s = data;
	char *new_display = NULL;

	(void)seconds;

	pthread_mutex_lock(&s->mutex);
	uint32_t window = s->game.window;
	bool only_when_showing = s->only_when_showing;
	if (!s->gx_display || strcmp(s->gx_display, s->display) != 0)
		new_display = bstrdup(s->display);
	pthread_mutex_unlock(&s->mutex);

	if (new_display) {
		release_capture(s);
		sgc_x11_destroy(s->gx);
		s->gx = NULL;
		s->gx_next_connect = 0;
		bfree(s->gx_display);
		s->gx_display = new_display;
	}

	if (!window) {
		if (s->attached_window || s->texture)
			release_capture(s);
		set_path(s, PATH_NONE);
		return;
	}

	if (only_when_showing && !obs_source_showing(s->source))
		return;

	if (!s->gx) {
		uint64_t now = os_gettime_ns();
		if (now < s->gx_next_connect)
			return;
		s->gx = sgc_x11_create(s->gx_display);
		if (!s->gx) {
			s->gx_next_connect = now + RECONNECT_INTERVAL_NS;
			return;
		}
	}

	if (window != s->attached_window) {
		release_capture(s);
		sgc_x11_attach(s->gx, window);
		s->attached_window = window;
		s->dmabuf_disabled = s->force_cpu;
		s->dmabuf_failures = 0;
	}

	s->frame_counter++;

	enum sgc_grab_result res = SGC_GRAB_UNSUPPORTED;
	if (!s->dmabuf_disabled) {
		res = capture_gpu(s);
		if (res == SGC_GRAB_OK) {
			s->dmabuf_failures = 0;
			set_path(s, PATH_GPU);
		} else if (res == SGC_GRAB_UNSUPPORTED && ++s->dmabuf_failures >= MAX_DMABUF_FAILURES) {
			blog(LOG_WARNING, "[steam-game-capture] Zero-copy DMA-BUF capture unavailable for this "
					  "window, falling back to CPU copy");
			s->dmabuf_disabled = true;
		}
	}
	/* Also covers single failed exports, e.g. mid-resize. */
	if (res == SGC_GRAB_UNSUPPORTED) {
		res = capture_cpu(s);
		if (res == SGC_GRAB_OK)
			set_path(s, PATH_CPU);
	}

	switch (res) {
	case SGC_GRAB_OK:
	case SGC_GRAB_UNAVAILABLE:
	case SGC_GRAB_UNSUPPORTED:
		break;
	case SGC_GRAB_GONE:
		window_gone(s, window, STATUS_WAITING);
		break;
	case SGC_GRAB_ERROR:
		blog(LOG_WARNING, "[steam-game-capture] Lost connection to X server");
		window_gone(s, window, STATUS_NO_DISPLAY);
		sgc_x11_destroy(s->gx);
		s->gx = NULL;
		s->gx_next_connect = os_gettime_ns() + RECONNECT_INTERVAL_NS;
		break;
	}
}

static void sgc_video_render(void *data, gs_effect_t *unused)
{
	struct steam_game_capture *s = data;

	(void)unused;
	if (!s->texture)
		return;

	/* Game windows have no meaningful alpha. */
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	const bool linear_srgb = gs_get_linear_srgb();
	const bool previous = gs_framebuffer_srgb_enabled();

	gs_enable_framebuffer_srgb(linear_srgb);
	if (linear_srgb)
		gs_effect_set_texture_srgb(image, s->texture);
	else
		gs_effect_set_texture(image, s->texture);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(s->texture, 0, 0, 0);

	gs_enable_framebuffer_srgb(previous);
}

static uint32_t sgc_get_width(void *data)
{
	struct steam_game_capture *s = data;
	return s->width;
}

static uint32_t sgc_get_height(void *data)
{
	struct steam_game_capture *s = data;
	return s->height;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle and properties                                                 */

static void sgc_update(void *data, obs_data_t *settings)
{
	struct steam_game_capture *s = data;
	bool want_audio = obs_data_get_bool(settings, S_CAPTURE_AUDIO);

	pthread_mutex_lock(&s->mutex);

	s->app_id = (uint32_t)obs_data_get_int(settings, S_APP_ID);
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
	const char *disable_dmabuf = getenv("SGC_DISABLE_DMABUF");
	s->force_cpu = disable_dmabuf && strcmp(disable_dmabuf, "0") != 0;
	pthread_mutex_init(&s->mutex, NULL);
	if (os_event_init(&s->stop_event, OS_EVENT_TYPE_MANUAL) != 0) {
		pthread_mutex_destroy(&s->mutex);
		bfree(s);
		return NULL;
	}

	sgc_update(s, settings);

	s->thread_created = pthread_create(&s->thread, NULL, scan_thread, s) == 0;
	if (!s->thread_created)
		blog(LOG_ERROR, "[steam-game-capture] Failed to start scan thread");
	return s;
}

static void sgc_destroy(void *data)
{
	struct steam_game_capture *s = data;

	if (s->thread_created) {
		os_event_signal(s->stop_event);
		pthread_join(s->thread, NULL);
	}

	release_capture(s);
	sgc_x11_destroy(s->gx);
	sgc_pw_audio_destroy(s->audio);
	bfree(s->gx_display);
	bfree(s->display);
	bfree(s->audio_match);
	os_event_destroy(s->stop_event);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static void sgc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_APP_ID, 0);
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
		if (s->path == PATH_GPU)
			dstr_catf(out, " — %s", T_("Status.GPU"));
		else if (s->path == PATH_CPU)
			dstr_catf(out, " — %s", T_("Status.CPU"));
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
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
	.get_name = sgc_get_name,
	.create = sgc_create,
	.destroy = sgc_destroy,
	.update = sgc_update,
	.get_defaults = sgc_get_defaults,
	.get_properties = sgc_properties,
	.video_tick = sgc_video_tick,
	.video_render = sgc_video_render,
	.get_width = sgc_get_width,
	.get_height = sgc_get_height,
};
