// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * X11 (XWayland or Xorg) side of the capture: finds Steam game windows and
 * exports their contents as DMA-BUFs (DRI3) for zero-copy GPU import, or
 * reads them through MIT-SHM / plain GetImage as a CPU fallback.
 *
 * A struct sgc_x11 owns one X connection and must only be used from one
 * thread at a time.
 */

struct sgc_x11;

struct sgc_window_info {
	uint32_t window;
	uint32_t app_id;    /* Steam AppID, never 0 for a detected game */
	uint32_t pid;       /* _NET_WM_PID, as seen by the game itself */
	uint32_t xres_pid;  /* PID reported by the X server (X-Resource) */
	uint32_t width;
	uint32_t height;
	bool viewable;
	char title[256];
	char wm_class[128];
};

struct sgc_x11_frame {
	const uint8_t *data; /* BGRX, valid until the next grab/detach */
	uint32_t width;
	uint32_t height;
	uint32_t linesize;
};

#define SGC_MAX_PLANES 4

struct sgc_dmabuf {
	int fds[SGC_MAX_PLANES]; /* owned by the caller, see sgc_dmabuf_close() */
	uint32_t strides[SGC_MAX_PLANES];
	uint32_t offsets[SGC_MAX_PLANES];
	uint32_t n_planes;
	uint64_t modifier; /* DRM_FORMAT_MOD_INVALID when the server didn't say */
	uint32_t drm_format;
	uint32_t width;
	uint32_t height;
	uint64_t buffer_id; /* same value for the same underlying buffer */
};

enum sgc_grab_result {
	SGC_GRAB_OK,
	SGC_GRAB_UNAVAILABLE, /* minimized, resizing, unsupported depth... */
	SGC_GRAB_UNSUPPORTED, /* DMA-BUF export impossible, use sgc_x11_grab() */
	SGC_GRAB_GONE,        /* window was destroyed */
	SGC_GRAB_ERROR,       /* X connection is broken */
};

/* display may be NULL or "" to use $DISPLAY. Returns NULL on failure. */
struct sgc_x11 *sgc_x11_create(const char *display);
void sgc_x11_destroy(struct sgc_x11 *x);

bool sgc_x11_is_xwayland(const struct sgc_x11 *x);

/* Lists top-level windows that belong to Steam games (Steam client excluded). */
size_t sgc_x11_find_games(struct sgc_x11 *x, struct sgc_window_info *out, size_t max);

/*
 * Picks the best candidate: matching app_id (0 = any), viewable first, then
 * largest. Returns -1 if nothing matches.
 */
int sgc_x11_pick_game(const struct sgc_window_info *games, size_t count, uint32_t app_id);

void sgc_x11_attach(struct sgc_x11 *x, uint32_t window);
void sgc_x11_detach(struct sgc_x11 *x);

/* CPU path: copies the window contents into memory. */
enum sgc_grab_result sgc_x11_grab(struct sgc_x11 *x, struct sgc_x11_frame *frame);

/* GPU path: exports the window's current backing buffer, no pixel copy. */
enum sgc_grab_result sgc_x11_grab_dmabuf(struct sgc_x11 *x, struct sgc_dmabuf *buf);
void sgc_dmabuf_close(struct sgc_dmabuf *buf);
