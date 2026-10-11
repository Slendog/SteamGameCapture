// SPDX-License-Identifier: GPL-2.0-or-later
#include "sgc-x11.h"
#include "sgc-log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <xcb/composite.h>
#include <xcb/res.h>
#include <xcb/shm.h>
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
	bool is_xwayland;

	xcb_window_t target;
	bool redirected;
	bool warned_depth;

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

	sgc_log(SGC_LOG_DEBUG, "Connected to X display %s (xwayland=%d shm=%d composite=%d xres=%d)",
		display && *display ? display : "$DISPLAY", x->is_xwayland, x->has_shm, x->has_composite,
		x->has_res);
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

void sgc_x11_attach(struct sgc_x11 *x, uint32_t window)
{
	sgc_x11_detach(x);
	x->target = window;

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
	x->redirected = false;
	x->warned_depth = false;
	free(x->image_reply);
	x->image_reply = NULL;
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

enum sgc_grab_result sgc_x11_grab(struct sgc_x11 *x, struct sgc_x11_frame *frame)
{
	xcb_connection_t *conn = x->conn;
	xcb_generic_error_t *err = NULL;

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

	uint32_t width = geo->width;
	uint32_t height = geo->height;
	uint8_t depth = geo->depth;
	bool viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
	free(geo);
	free(attr);

	if (!viewable || !width || !height)
		return SGC_GRAB_UNAVAILABLE;

	if (depth != 24 && depth != 32) {
		if (!x->warned_depth)
			sgc_log(SGC_LOG_WARNING, "Window 0x%x has unsupported depth %u", x->target, depth);
		x->warned_depth = true;
		return SGC_GRAB_UNAVAILABLE;
	}

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
