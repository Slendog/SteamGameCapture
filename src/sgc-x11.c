// SPDX-License-Identifier: Unlicense
#include "sgc-x11.h"
#include "sgc-log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <xcb/composite.h>
#include <xcb/dri3.h>
#include <xcb/res.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <xcb/xcb.h>

/* The Steam client itself carries STEAM_GAME=769 on its windows. */
#define STEAM_CLIENT_APP_ID 769u
#define MAX_CLIENT_WINDOWS 1024

enum {
	ATOM_NET_CLIENT_LIST,
	ATOM_STEAM_GAME,
	ATOM_NET_WM_PID,
	ATOM_NET_WM_NAME,
	ATOM_UTF8_STRING,
	ATOM_WM_STATE,
	ATOM_COUNT,
};

static const char *const atom_names[ATOM_COUNT] = {
	"_NET_CLIENT_LIST", "STEAM_GAME", "_NET_WM_PID", "_NET_WM_NAME", "UTF8_STRING", "WM_STATE",
};

struct sgc_x11 {
	xcb_connection_t *conn;
	xcb_window_t root;
	xcb_atom_t atoms[ATOM_COUNT];

	bool has_shm;
	bool has_composite;
	bool has_res;
	bool has_dri3;
	bool has_dri3_modifiers; /* DRI3 1.2: BuffersFromPixmap */
	bool has_xfixes;
	bool is_xwayland;

	xcb_window_t target;
	xcb_window_t target_toplevel; /* child of root containing target (WM frame) */
	bool redirected;
	bool warned_depth;
	uint32_t target_width; /* from the last grab */
	uint32_t target_height;

	xcb_xfixes_get_cursor_image_reply_t *cursor_reply;

	xcb_shm_seg_t shm_seg;
	void *shm_addr;
	size_t shm_size;

	/* Holds the pixels when MIT-SHM is unavailable. */
	xcb_get_image_reply_t *image_reply;
};

static bool has_extension(xcb_connection_t *conn, const char *name)
{
	xcb_query_extension_reply_t *reply =
		xcb_query_extension_reply(conn, xcb_query_extension(conn, strlen(name), name), NULL);
	bool present = reply && reply->present;

	free(reply);
	return present;
}

static void detect_extensions(struct sgc_x11 *x)
{
	xcb_connection_t *conn = x->conn;

	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_shm_id);
	if (ext && ext->present) {
		xcb_shm_query_version_reply_t *r = xcb_shm_query_version_reply(conn, xcb_shm_query_version(conn), NULL);
		x->has_shm = r != NULL;
		free(r);
	}

	ext = xcb_get_extension_data(conn, &xcb_composite_id);
	if (ext && ext->present) {
		xcb_composite_query_version_reply_t *r =
			xcb_composite_query_version_reply(conn, xcb_composite_query_version(conn, 0, 2), NULL);
		x->has_composite = r && (r->major_version > 0 || r->minor_version >= 2);
		free(r);
	}

	ext = xcb_get_extension_data(conn, &xcb_res_id);
	if (ext && ext->present) {
		xcb_res_query_version_reply_t *r =
			xcb_res_query_version_reply(conn, xcb_res_query_version(conn, 1, 2), NULL);
		/* QueryClientIds needs X-Resource 1.2 */
		x->has_res = r && (r->server_major > 1 || (r->server_major == 1 && r->server_minor >= 2));
		free(r);
	}

	ext = xcb_get_extension_data(conn, &xcb_dri3_id);
	if (ext && ext->present) {
		xcb_dri3_query_version_reply_t *r =
			xcb_dri3_query_version_reply(conn, xcb_dri3_query_version(conn, 1, 2), NULL);
		x->has_dri3 = r != NULL;
		x->has_dri3_modifiers = r && (r->major_version > 1 || r->minor_version >= 2);
		free(r);
	}

	ext = xcb_get_extension_data(conn, &xcb_xfixes_id);
	if (ext && ext->present) {
		/* Version negotiation is mandatory before any other XFixes request. */
		xcb_xfixes_query_version_reply_t *r =
			xcb_xfixes_query_version_reply(conn, xcb_xfixes_query_version(conn, 4, 0), NULL);
		x->has_xfixes = r && r->major_version >= 1;
		free(r);
	}

	x->is_xwayland = has_extension(conn, "XWAYLAND");
}

struct sgc_x11 *sgc_x11_create(const char *display)
{
	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(display && *display ? display : NULL, &screen_num);

	if (xcb_connection_has_error(conn)) {
		xcb_disconnect(conn);
		return NULL;
	}

	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	for (int i = 0; i < screen_num && it.rem; i++)
		xcb_screen_next(&it);
	if (!it.rem) {
		xcb_disconnect(conn);
		return NULL;
	}

	struct sgc_x11 *x = calloc(1, sizeof(*x));
	x->conn = conn;
	x->root = it.data->root;

	xcb_intern_atom_cookie_t cookies[ATOM_COUNT];
	for (int i = 0; i < ATOM_COUNT; i++)
		cookies[i] = xcb_intern_atom(conn, 0, strlen(atom_names[i]), atom_names[i]);
	for (int i = 0; i < ATOM_COUNT; i++) {
		xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, cookies[i], NULL);
		x->atoms[i] = r ? r->atom : XCB_ATOM_NONE;
		free(r);
	}

	detect_extensions(x);

	sgc_log(SGC_LOG_DEBUG, "Connected to X display %s (xwayland=%d shm=%d composite=%d xres=%d dri3=%d)",
		display && *display ? display : "$DISPLAY", x->is_xwayland, x->has_shm, x->has_composite,
		x->has_res, x->has_dri3);
	return x;
}

bool sgc_x11_is_xwayland(const struct sgc_x11 *x)
{
	return x->is_xwayland;
}

/* Errors from unchecked requests end up in the event queue; drop them. */
static void drain_events(struct sgc_x11 *x)
{
	xcb_generic_event_t *ev;

	while ((ev = xcb_poll_for_event(x->conn)))
		free(ev);
}

static xcb_get_property_reply_t *get_property(struct sgc_x11 *x, xcb_window_t window, xcb_atom_t property,
					      xcb_atom_t type, uint32_t long_length)
{
	if (property == XCB_ATOM_NONE)
		return NULL;

	xcb_get_property_reply_t *reply = xcb_get_property_reply(
		x->conn, xcb_get_property(x->conn, 0, window, property, type, 0, long_length), NULL);
	if (reply && xcb_get_property_value_length(reply) <= 0) {
		free(reply);
		return NULL;
	}
	return reply;
}

static bool get_cardinal(struct sgc_x11 *x, xcb_window_t window, xcb_atom_t property, uint32_t *value)
{
	xcb_get_property_reply_t *reply = get_property(x, window, property, XCB_ATOM_CARDINAL, 1);
	bool ok = reply && reply->format == 32;

	if (ok)
		*value = *(uint32_t *)xcb_get_property_value(reply);
	free(reply);
	return ok;
}

static void get_string(struct sgc_x11 *x, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, char *out,
		       size_t out_size)
{
	xcb_get_property_reply_t *reply = get_property(x, window, property, type, 256);
	if (!reply)
		return;

	size_t len = (size_t)xcb_get_property_value_length(reply);
	if (len >= out_size)
		len = out_size - 1;
	memcpy(out, xcb_get_property_value(reply), len);
	out[len] = '\0';
	free(reply);
}

static bool has_wm_state(struct sgc_x11 *x, xcb_window_t window)
{
	xcb_get_property_reply_t *reply = get_property(x, window, x->atoms[ATOM_WM_STATE], XCB_GET_PROPERTY_TYPE_ANY, 2);
	bool found = reply != NULL;

	free(reply);
	return found;
}

static size_t list_client_windows(struct sgc_x11 *x, xcb_window_t *out, size_t max)
{
	size_t count = 0;

	xcb_get_property_reply_t *reply =
		get_property(x, x->root, x->atoms[ATOM_NET_CLIENT_LIST], XCB_ATOM_WINDOW, (uint32_t)max);
	if (reply) {
		count = (size_t)xcb_get_property_value_length(reply) / sizeof(xcb_window_t);
		if (count > max)
			count = max;
		memcpy(out, xcb_get_property_value(reply), count * sizeof(xcb_window_t));
		free(reply);
		return count;
	}

	/* No EWMH window manager: walk the tree, one level deep for reparenting WMs. */
	xcb_query_tree_reply_t *tree = xcb_query_tree_reply(x->conn, xcb_query_tree(x->conn, x->root), NULL);
	if (!tree)
		return 0;

	xcb_window_t *children = xcb_query_tree_children(tree);
	int n = xcb_query_tree_children_length(tree);
	for (int i = 0; i < n && count < max; i++) {
		if (has_wm_state(x, children[i])) {
			out[count++] = children[i];
			continue;
		}

		xcb_query_tree_reply_t *sub =
			xcb_query_tree_reply(x->conn, xcb_query_tree(x->conn, children[i]), NULL);
		if (!sub)
			continue;
		xcb_window_t *grandchildren = xcb_query_tree_children(sub);
		int m = xcb_query_tree_children_length(sub);
		for (int j = 0; j < m && count < max; j++) {
			if (has_wm_state(x, grandchildren[j])) {
				out[count++] = grandchildren[j];
				break;
			}
		}
		free(sub);
	}
	free(tree);
	return count;
}

static uint32_t query_xres_pid(struct sgc_x11 *x, xcb_window_t window)
{
	if (!x->has_res)
		return 0;

	xcb_res_client_id_spec_t spec = {
		.client = window,
		.mask = XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID,
	};
	xcb_res_query_client_ids_reply_t *reply =
		xcb_res_query_client_ids_reply(x->conn, xcb_res_query_client_ids(x->conn, 1, &spec), NULL);
	if (!reply)
		return 0;

	uint32_t pid = 0;
	xcb_res_client_id_value_iterator_t it = xcb_res_query_client_ids_ids_iterator(reply);
	for (; it.rem; xcb_res_client_id_value_next(&it)) {
		if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
		    xcb_res_client_id_value_value_length(it.data) >= 1) {
			pid = *xcb_res_client_id_value_value(it.data);
			break;
		}
	}
	free(reply);
	return pid;
}

static uint32_t parse_steam_app_class(const char *name)
{
	unsigned int app_id = 0;

	if (sscanf(name, "steam_app_%u", &app_id) == 1)
		return app_id;
	return 0;
}

static bool is_steam_client_class(const char *name)
{
	return strcasecmp(name, "steam") == 0 || strcasecmp(name, "steamwebhelper") == 0;
}

static void trim_trailing_space(char *s)
{
	size_t len = strlen(s);

	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		s[--len] = '\0';
}

static bool read_game_info(struct sgc_x11 *x, xcb_window_t window, struct sgc_window_info *info)
{
	memset(info, 0, sizeof(*info));
	info->window = window;

	/* WM_CLASS is "instance\0class\0" */
	char wm_class[sizeof(info->wm_class)] = {0};
	get_string(x, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, wm_class, sizeof(wm_class));
	const char *instance = wm_class;
	const char *klass = wm_class + strnlen(wm_class, sizeof(wm_class) - 1) + 1;
	if (klass >= wm_class + sizeof(wm_class))
		klass = "";

	if (is_steam_client_class(instance) || is_steam_client_class(klass))
		return false;

	uint32_t app_id = 0;
	get_cardinal(x, window, x->atoms[ATOM_STEAM_GAME], &app_id);
	if (!app_id || app_id == STEAM_CLIENT_APP_ID)
		app_id = parse_steam_app_class(instance);
	if (!app_id)
		app_id = parse_steam_app_class(klass);
	if (!app_id || app_id == STEAM_CLIENT_APP_ID)
		return false;

	info->app_id = app_id;
	snprintf(info->wm_class, sizeof(info->wm_class), "%s", *klass ? klass : instance);

	get_cardinal(x, window, x->atoms[ATOM_NET_WM_PID], &info->pid);
	info->xres_pid = query_xres_pid(x, window);

	get_string(x, window, x->atoms[ATOM_NET_WM_NAME], x->atoms[ATOM_UTF8_STRING], info->title,
		   sizeof(info->title));
	if (!info->title[0])
		get_string(x, window, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY, info->title, sizeof(info->title));
	trim_trailing_space(info->title);
	if (!info->title[0])
		snprintf(info->title, sizeof(info->title), "%s", info->wm_class);

	xcb_get_geometry_cookie_t gc = xcb_get_geometry(x->conn, window);
	xcb_get_window_attributes_cookie_t ac = xcb_get_window_attributes(x->conn, window);
	xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(x->conn, gc, NULL);
	xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(x->conn, ac, NULL);
	if (geo) {
		info->width = geo->width;
		info->height = geo->height;
	}
	info->viewable = attr && attr->map_state == XCB_MAP_STATE_VIEWABLE;
	free(geo);
	free(attr);
	return true;
}

size_t sgc_x11_find_games(struct sgc_x11 *x, struct sgc_window_info *out, size_t max)
{
	xcb_window_t windows[MAX_CLIENT_WINDOWS];
	size_t count = 0;

	drain_events(x);

	size_t n = list_client_windows(x, windows, MAX_CLIENT_WINDOWS);
	for (size_t i = 0; i < n && count < max; i++) {
		if (read_game_info(x, windows[i], &out[count]))
			count++;
	}
	return count;
}

int sgc_x11_pick_game(const struct sgc_window_info *games, size_t count, uint32_t app_id)
{
	int best = -1;

	for (size_t i = 0; i < count; i++) {
		const struct sgc_window_info *g = &games[i];
		if (app_id && g->app_id != app_id)
			continue;
		if (best < 0) {
			best = (int)i;
			continue;
		}

		const struct sgc_window_info *b = &games[best];
		uint64_t area = (uint64_t)g->width * g->height;
		uint64_t best_area = (uint64_t)b->width * b->height;
		if ((g->viewable && !b->viewable) || (g->viewable == b->viewable && area > best_area))
			best = (int)i;
	}
	return best;
}

static void free_shm(struct sgc_x11 *x)
{
	if (!x->shm_addr)
		return;

	xcb_shm_detach(x->conn, x->shm_seg);
	xcb_flush(x->conn);
	shmdt(x->shm_addr);
	x->shm_addr = NULL;
	x->shm_size = 0;
}

static bool ensure_shm(struct sgc_x11 *x, size_t size)
{
	if (x->shm_addr && x->shm_size >= size)
		return true;

	free_shm(x);

	int id = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
	if (id < 0)
		goto fail;

	void *addr = shmat(id, NULL, 0);
	if (addr == (void *)-1) {
		shmctl(id, IPC_RMID, NULL);
		goto fail;
	}

	xcb_shm_seg_t seg = xcb_generate_id(x->conn);
	xcb_generic_error_t *err = xcb_request_check(x->conn, xcb_shm_attach_checked(x->conn, seg, id, 0));
	/* Segment stays alive until both sides detach. */
	shmctl(id, IPC_RMID, NULL);
	if (err) {
		free(err);
		shmdt(addr);
		goto fail;
	}

	x->shm_seg = seg;
	x->shm_addr = addr;
	x->shm_size = size;
	return true;

fail:
	sgc_log(SGC_LOG_WARNING, "MIT-SHM unusable, falling back to slower GetImage");
	x->has_shm = false;
	return false;
}

/* Walks up to the root's child: the window manager frame, or the window itself. */
static xcb_window_t find_toplevel(struct sgc_x11 *x, xcb_window_t window)
{
	for (int depth = 0; depth < 16; depth++) {
		xcb_query_tree_reply_t *tree = xcb_query_tree_reply(x->conn, xcb_query_tree(x->conn, window), NULL);
		if (!tree)
			break;
		xcb_window_t parent = tree->parent;
		free(tree);
		if (parent == x->root || parent == XCB_WINDOW_NONE)
			return window;
		window = parent;
	}
	return window;
}

void sgc_x11_attach(struct sgc_x11 *x, uint32_t window)
{
	sgc_x11_detach(x);
	x->target = window;
	x->target_toplevel = find_toplevel(x, window);

	/*
	 * Make sure obscured parts are rendered. XWayland already redirects
	 * every top-level, and an extra redirect could stop it from flipping
	 * the game's buffers directly, so leave it alone there.
	 */
	if (x->has_composite && !x->is_xwayland) {
		xcb_generic_error_t *err = xcb_request_check(
			x->conn, xcb_composite_redirect_window_checked(x->conn, window,
								       XCB_COMPOSITE_REDIRECT_AUTOMATIC));
		x->redirected = err == NULL;
		free(err);
	}
}

void sgc_x11_detach(struct sgc_x11 *x)
{
	if (x->target && x->redirected)
		xcb_composite_unredirect_window(x->conn, x->target, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
	xcb_flush(x->conn);

	x->target = XCB_WINDOW_NONE;
	x->target_toplevel = XCB_WINDOW_NONE;
	x->redirected = false;
	x->warned_depth = false;
	x->target_width = 0;
	x->target_height = 0;
	free(x->image_reply);
	x->image_reply = NULL;
	free(x->cursor_reply);
	x->cursor_reply = NULL;
}

void sgc_x11_destroy(struct sgc_x11 *x)
{
	if (!x)
		return;

	sgc_x11_detach(x);
	free_shm(x);
	xcb_disconnect(x->conn);
	free(x);
}

static enum sgc_grab_result request_failed(struct sgc_x11 *x, xcb_generic_error_t *err)
{
	free(err);
	return xcb_connection_has_error(x->conn) ? SGC_GRAB_ERROR : SGC_GRAB_UNAVAILABLE;
}

/* Common checks before reading a frame; fills size and depth of the window. */
static enum sgc_grab_result check_window(struct sgc_x11 *x, uint32_t *width, uint32_t *height, uint8_t *depth)
{
	xcb_connection_t *conn = x->conn;

	if (xcb_connection_has_error(conn))
		return SGC_GRAB_ERROR;
	if (!x->target)
		return SGC_GRAB_GONE;

	drain_events(x);

	xcb_get_geometry_cookie_t gc = xcb_get_geometry(conn, x->target);
	xcb_get_window_attributes_cookie_t ac = xcb_get_window_attributes(conn, x->target);
	xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, gc, NULL);
	xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(conn, ac, NULL);

	if (!geo || !attr) {
		free(geo);
		free(attr);
		return xcb_connection_has_error(conn) ? SGC_GRAB_ERROR : SGC_GRAB_GONE;
	}

	*width = x->target_width = geo->width;
	*height = x->target_height = geo->height;
	*depth = geo->depth;
	bool viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
	free(geo);
	free(attr);

	if (!viewable || !*width || !*height)
		return SGC_GRAB_UNAVAILABLE;

	if (*depth != 24 && *depth != 32 && *depth != 30) {
		if (!x->warned_depth)
			sgc_log(SGC_LOG_WARNING, "Window 0x%x has unsupported depth %u", x->target, *depth);
		x->warned_depth = true;
		return SGC_GRAB_UNAVAILABLE;
	}
	return SGC_GRAB_OK;
}

enum sgc_grab_result sgc_x11_grab(struct sgc_x11 *x, struct sgc_x11_frame *frame)
{
	xcb_connection_t *conn = x->conn;
	xcb_generic_error_t *err = NULL;
	uint32_t width, height;
	uint8_t depth;

	enum sgc_grab_result res = check_window(x, &width, &height, &depth);
	if (res != SGC_GRAB_OK)
		return res;

	/* BGRX output only; 30-bit windows need the DMA-BUF path. */
	if (depth == 30)
		return SGC_GRAB_UNAVAILABLE;

	/* Depth 24 and 32 both use 32 bits per pixel in ZPixmap format. */
	size_t size = (size_t)width * height * 4;

	if (x->has_shm && ensure_shm(x, size)) {
		xcb_shm_get_image_reply_t *reply = xcb_shm_get_image_reply(
			conn,
			xcb_shm_get_image(conn, x->target, 0, 0, width, height, ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP,
					  x->shm_seg, 0),
			&err);
		if (!reply)
			return request_failed(x, err);
		free(reply);
		frame->data = x->shm_addr;
	} else {
		free(x->image_reply);
		x->image_reply = xcb_get_image_reply(
			conn, xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, x->target, 0, 0, width, height, ~0u),
			&err);
		if (!x->image_reply)
			return request_failed(x, err);
		if ((size_t)xcb_get_image_data_length(x->image_reply) < size)
			return SGC_GRAB_UNAVAILABLE;
		frame->data = xcb_get_image_data(x->image_reply);
	}

	frame->width = width;
	frame->height = height;
	frame->linesize = width * 4;
	return SGC_GRAB_OK;
}

void sgc_dmabuf_close(struct sgc_dmabuf *buf)
{
	for (uint32_t i = 0; i < buf->n_planes; i++) {
		if (buf->fds[i] >= 0)
			close(buf->fds[i]);
		buf->fds[i] = -1;
	}
	buf->n_planes = 0;
}

static uint32_t drm_format_for(uint8_t depth, uint8_t bpp)
{
	if (bpp != 32)
		return 0;
	/* Alpha of game windows is meaningless, always treat it as padding. */
	switch (depth) {
	case 24:
	case 32:
		return DRM_FORMAT_XRGB8888;
	case 30:
		return DRM_FORMAT_XRGB2101010;
	default:
		return 0;
	}
}

/*
 * Fetches the window pixmap as DMA-BUF. The pixmap is named again on every
 * call because XWayland swaps in the game's buffer on each Present flip.
 */
enum sgc_grab_result sgc_x11_grab_dmabuf(struct sgc_x11 *x, struct sgc_dmabuf *buf)
{
	xcb_connection_t *conn = x->conn;
	xcb_generic_error_t *err = NULL;
	uint32_t width, height;
	uint8_t depth, bpp;

	memset(buf, 0, sizeof(*buf));
	for (int i = 0; i < SGC_MAX_PLANES; i++)
		buf->fds[i] = -1;

	if (!x->has_dri3 || !x->has_composite)
		return SGC_GRAB_UNSUPPORTED;

	enum sgc_grab_result res = check_window(x, &width, &height, &depth);
	if (res != SGC_GRAB_OK)
		return res;

	xcb_pixmap_t pixmap = xcb_generate_id(conn);
	err = xcb_request_check(conn, xcb_composite_name_window_pixmap_checked(conn, x->target, pixmap));
	if (err)
		return request_failed(x, err);

	if (x->has_dri3_modifiers) {
		xcb_dri3_buffers_from_pixmap_reply_t *reply =
			xcb_dri3_buffers_from_pixmap_reply(conn, xcb_dri3_buffers_from_pixmap(conn, pixmap), &err);
		xcb_free_pixmap(conn, pixmap);
		if (!reply)
			goto export_failed;

		int *fds = xcb_dri3_buffers_from_pixmap_reply_fds(conn, reply);
		uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(reply);
		uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(reply);
		uint32_t n = reply->nfd;

		for (uint32_t i = 0; i < n; i++) {
			if (i < SGC_MAX_PLANES) {
				buf->fds[i] = fds[i];
				buf->strides[i] = strides[i];
				buf->offsets[i] = offsets[i];
			} else {
				close(fds[i]);
			}
		}
		buf->n_planes = n < SGC_MAX_PLANES ? n : SGC_MAX_PLANES;
		buf->modifier = reply->modifier;
		buf->width = reply->width;
		buf->height = reply->height;
		depth = reply->depth;
		bpp = reply->bpp;
		free(reply);
	} else {
		xcb_dri3_buffer_from_pixmap_reply_t *reply =
			xcb_dri3_buffer_from_pixmap_reply(conn, xcb_dri3_buffer_from_pixmap(conn, pixmap), &err);
		xcb_free_pixmap(conn, pixmap);
		if (!reply)
			goto export_failed;

		buf->fds[0] = xcb_dri3_buffer_from_pixmap_reply_fds(conn, reply)[0];
		buf->strides[0] = reply->stride;
		buf->n_planes = 1;
		buf->modifier = DRM_FORMAT_MOD_INVALID;
		buf->width = reply->width;
		buf->height = reply->height;
		depth = reply->depth;
		bpp = reply->bpp;
		free(reply);
	}

	buf->drm_format = drm_format_for(depth, bpp);
	if (!buf->n_planes || !buf->drm_format) {
		sgc_dmabuf_close(buf);
		return SGC_GRAB_UNSUPPORTED;
	}

	/* A DMA-BUF's inode identifies the buffer, whichever fd refers to it. */
	struct stat st;
	if (fstat(buf->fds[0], &st) == 0)
		buf->buffer_id = (uint64_t)st.st_ino;

	return SGC_GRAB_OK;

export_failed:
	free(err);
	xcb_flush(conn);
	return xcb_connection_has_error(conn) ? SGC_GRAB_ERROR : SGC_GRAB_UNSUPPORTED;
}

bool sgc_x11_get_cursor(struct sgc_x11 *x, struct sgc_cursor *cursor)
{
	xcb_connection_t *conn = x->conn;

	memset(cursor, 0, sizeof(*cursor));
	if (!x->has_xfixes || !x->target || xcb_connection_has_error(conn))
		return false;

	xcb_xfixes_get_cursor_image_cookie_t cc = xcb_xfixes_get_cursor_image(conn);
	xcb_translate_coordinates_cookie_t tc = xcb_translate_coordinates(conn, x->target, x->root, 0, 0);
	xcb_query_pointer_cookie_t pc = xcb_query_pointer(conn, x->root);

	free(x->cursor_reply);
	x->cursor_reply = xcb_xfixes_get_cursor_image_reply(conn, cc, NULL);
	xcb_translate_coordinates_reply_t *origin = xcb_translate_coordinates_reply(conn, tc, NULL);
	xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(conn, pc, NULL);

	const xcb_xfixes_get_cursor_image_reply_t *image = x->cursor_reply;
	if (!image || !origin || !pointer) {
		free(origin);
		free(pointer);
		return false;
	}

	/*
	 * The top-level window under the pointer must be the game. This hides
	 * the cursor when another window covers the game, and when the pointer
	 * is over a Wayland window: XWayland then leaves the position parked at
	 * the game's edge but reports the root window as the one under it.
	 */
	bool over_game = pointer->same_screen && pointer->child == x->target_toplevel;

	int32_t px = image->x - origin->dst_x;
	int32_t py = image->y - origin->dst_y;
	bool inside = px >= 0 && py >= 0 && (uint32_t)px < x->target_width && (uint32_t)py < x->target_height;

	cursor->visible = over_game && inside && image->width && image->height;
	cursor->x = px - image->xhot;
	cursor->y = py - image->yhot;
	cursor->width = image->width;
	cursor->height = image->height;
	cursor->serial = image->cursor_serial;
	cursor->pixels = xcb_xfixes_get_cursor_image_cursor_image(image);

	free(origin);
	free(pointer);
	return true;
}
