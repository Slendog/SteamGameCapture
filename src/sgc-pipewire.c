// SPDX-License-Identifier: GPL-2.0-or-later
#include "sgc-pipewire.h"
#include "sgc-log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#define MAX_TARGET_PIDS 4

struct client_obj {
	struct spa_list link;
	struct sgc_pw_audio *pa;
	uint32_t id;
	struct pw_proxy *proxy;
	struct spa_hook object_listener;
	uint32_t pid;
	char *app_name;
	char *binary;
};

/* Only Stream/Output/Audio nodes are tracked. */
struct node_obj {
	struct spa_list link;
	struct sgc_pw_audio *pa;
	uint32_t id;
	struct pw_proxy *proxy;
	struct spa_hook object_listener;
	uint32_t client_id;
	char *node_name;
	char *app_name;
	bool linked;
};

struct port_obj {
	struct spa_list link;
	uint32_t id;
	uint32_t node_id;
	enum pw_direction direction;
	uint32_t index;
	char channel[16];
};

struct link_obj {
	struct spa_list link;
	struct sgc_pw_audio *pa;
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	uint32_t out_port;
	uint32_t in_port;
	bool dead; /* removed by the server, freed on the next update */
	bool keep;
};

struct sgc_pw_audio {
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	uint32_t stream_node_id;

	uint32_t rate;
	sgc_audio_cb cb;
	void *cb_param;

	uint32_t target_pids[MAX_TARGET_PIDS];
	size_t n_target_pids;
	char *match;

	struct spa_list clients;
	struct spa_list nodes;
	struct spa_list ports;
	struct spa_list links;

	bool link_error_logged;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static void replace_str(char **dst, const char *src)
{
	free(*dst);
	*dst = src ? strdup(src) : NULL;
}

static bool contains_ci(const char *haystack, const char *needle)
{
	return haystack && strcasestr(haystack, needle) != NULL;
}

static struct client_obj *find_client(struct sgc_pw_audio *pa, uint32_t id)
{
	struct client_obj *c;

	spa_list_for_each(c, &pa->clients, link)
	{
		if (c->id == id)
			return c;
	}
	return NULL;
}

static bool node_matches(struct sgc_pw_audio *pa, struct node_obj *n)
{
	struct client_obj *c = find_client(pa, n->client_id);

	if (c && c->pid) {
		for (size_t i = 0; i < pa->n_target_pids; i++) {
			if (c->pid == pa->target_pids[i])
				return true;
		}
	}

	if (pa->match && *pa->match) {
		if (contains_ci(n->node_name, pa->match) || contains_ci(n->app_name, pa->match))
			return true;
		if (c && (contains_ci(c->app_name, pa->match) || contains_ci(c->binary, pa->match)))
			return true;
	}
	return false;
}

enum { SIDE_NONE = 0, SIDE_LEFT = 1, SIDE_RIGHT = 2, SIDE_BOTH = 3, SIDE_UNKNOWN = -1 };

static bool in_list(const char *s, const char *const *list)
{
	for (; *list; list++) {
		if (strcmp(s, *list) == 0)
			return true;
	}
	return false;
}

/* Which of our stereo inputs a playback channel is folded into. */
static int channel_side(const char *channel)
{
	static const char *const left[] = {"FL", "SL", "RL", "FLC", "RLC", "TFL", "TRL",
					   "FLW", "FLH", "TFLC", "TSL", "BLC", NULL};
	static const char *const right[] = {"FR", "SR", "RR", "FRC", "RRC", "TFR", "TRR",
					    "FRW", "FRH", "TFRC", "TSR", "BRC", NULL};
	static const char *const center[] = {"MONO", "FC", "RC", "TC", "TFC", "TRC", "FCH", "BC", NULL};

	if (in_list(channel, left))
		return SIDE_LEFT;
	if (in_list(channel, right))
		return SIDE_RIGHT;
	if (in_list(channel, center))
		return SIDE_BOTH;
	if (strstr(channel, "LFE"))
		return SIDE_NONE;
	return SIDE_UNKNOWN;
}

static bool should_route(const struct port_obj *out, const struct port_obj *in)
{
	int side = channel_side(out->channel);
	int in_side = strcmp(in->channel, "FR") == 0 ? SIDE_RIGHT : SIDE_LEFT;

	if (side == SIDE_UNKNOWN)
		return out->index % SGC_PW_CHANNELS == in->index % SGC_PW_CHANNELS;
	return (side & in_side) != 0;
}

static void link_free(struct link_obj *l)
{
	spa_hook_remove(&l->proxy_listener);
	pw_proxy_destroy(l->proxy);
	spa_list_remove(&l->link);
	free(l);
}

static void link_removed(void *data)
{
	struct link_obj *l = data;

	l->dead = true;
}

static void link_error(void *data, int seq, int res, const char *message)
{
	struct link_obj *l = data;

	(void)seq;
	if (!l->pa->link_error_logged)
		sgc_log(SGC_LOG_WARNING, "Failed to link game audio: %s (%s)", message, spa_strerror(res));
	l->pa->link_error_logged = true;
	l->dead = true;
}

static const struct pw_proxy_events link_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = link_removed,
	.error = link_error,
};

static void ensure_link(struct sgc_pw_audio *pa, uint32_t out_node, uint32_t out_port, uint32_t in_port)
{
	struct link_obj *l;

	spa_list_for_each(l, &pa->links, link)
	{
		if (l->out_port == out_port && l->in_port == in_port) {
			l->keep = true;
			return;
		}
	}

	struct pw_properties *props = pw_properties_new(PW_KEY_OBJECT_LINGER, "false", NULL);
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", out_node);
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out_port);
	pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", pa->stream_node_id);
	pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", in_port);

	struct pw_proxy *proxy =
		pw_core_create_object(pa->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
	pw_properties_free(props);
	if (!proxy)
		return;

	l = calloc(1, sizeof(*l));
	l->pa = pa;
	l->proxy = proxy;
	l->out_port = out_port;
	l->in_port = in_port;
	l->keep = true;
	pw_proxy_add_listener(proxy, &l->proxy_listener, &link_proxy_events, l);
	spa_list_append(&pa->links, &l->link);
}

/* Reconciles our links with the current set of matching game streams. */
static void update_links(struct sgc_pw_audio *pa)
{
	struct link_obj *l, *lt;
	struct node_obj *n;

	spa_list_for_each_safe(l, lt, &pa->links, link)
	{
		if (l->dead)
			link_free(l);
		else
			l->keep = false;
	}

	spa_list_for_each(n, &pa->nodes, link)
	{
		bool matched = pa->stream_node_id != SPA_ID_INVALID && node_matches(pa, n);

		if (matched) {
			struct port_obj *out, *in;
			spa_list_for_each(out, &pa->ports, link)
			{
				if (out->node_id != n->id || out->direction != PW_DIRECTION_OUTPUT)
					continue;
				spa_list_for_each(in, &pa->ports, link)
				{
					if (in->node_id == pa->stream_node_id && in->direction == PW_DIRECTION_INPUT &&
					    should_route(out, in))
						ensure_link(pa, n->id, out->id, in->id);
				}
			}
		}

		if (matched && !n->linked)
			sgc_log(SGC_LOG_INFO, "Capturing audio stream %u (%s)", n->id,
				n->app_name ? n->app_name : n->node_name ? n->node_name : "unknown");
		n->linked = matched;
	}

	spa_list_for_each_safe(l, lt, &pa->links, link)
	{
		if (!l->keep)
			link_free(l);
	}
}

static void client_info(void *data, const struct pw_client_info *info)
{
	struct client_obj *c = data;

	if (!(info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) || !info->props)
		return;

	const char *pid = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID);
	c->pid = pid ? (uint32_t)strtoul(pid, NULL, 10) : 0;
	replace_str(&c->app_name, spa_dict_lookup(info->props, PW_KEY_APP_NAME));
	replace_str(&c->binary, spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY));
	update_links(c->pa);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_info,
};

static void node_info(void *data, const struct pw_node_info *info)
{
	struct node_obj *n = data;

	if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props)
		return;

	const char *client_id = spa_dict_lookup(info->props, PW_KEY_CLIENT_ID);
	if (client_id)
		n->client_id = (uint32_t)strtoul(client_id, NULL, 10);
	replace_str(&n->node_name, spa_dict_lookup(info->props, PW_KEY_NODE_NAME));
	replace_str(&n->app_name, spa_dict_lookup(info->props, PW_KEY_APP_NAME));
	update_links(n->pa);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
};

static void client_free(struct client_obj *c)
{
	spa_hook_remove(&c->object_listener);
	pw_proxy_destroy(c->proxy);
	spa_list_remove(&c->link);
	free(c->app_name);
	free(c->binary);
	free(c);
}

static void node_free(struct node_obj *n)
{
	spa_hook_remove(&n->object_listener);
	pw_proxy_destroy(n->proxy);
	spa_list_remove(&n->link);
	free(n->node_name);
	free(n->app_name);
	free(n);
}

static void port_free(struct port_obj *p)
{
	spa_list_remove(&p->link);
	free(p);
}

static void add_port(struct sgc_pw_audio *pa, uint32_t id, const struct spa_dict *props)
{
	const char *node_id = spa_dict_lookup(props, PW_KEY_NODE_ID);
	const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
	const char *channel = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
	const char *port_id = spa_dict_lookup(props, PW_KEY_PORT_ID);
	const char *port_name = spa_dict_lookup(props, PW_KEY_PORT_NAME);

	if (!node_id || !direction)
		return;

	struct port_obj *p = calloc(1, sizeof(*p));
	p->id = id;
	p->node_id = (uint32_t)strtoul(node_id, NULL, 10);
	p->direction = spa_streq(direction, "in") ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT;
	p->index = port_id ? (uint32_t)strtoul(port_id, NULL, 10) : 0;

	/* Fall back to the suffix of names like "output_FL". */
	if (!channel && port_name) {
		channel = strrchr(port_name, '_');
		channel = channel ? channel + 1 : NULL;
	}
	if (channel)
		snprintf(p->channel, sizeof(p->channel), "%s", channel);

	spa_list_append(&pa->ports, &p->link);
}

static void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
			    const struct spa_dict *props)
{
	struct sgc_pw_audio *pa = data;

	(void)permissions;
	(void)version;

	if (spa_streq(type, PW_TYPE_INTERFACE_Client)) {
		struct pw_proxy *proxy = pw_registry_bind(pa->registry, id, type, PW_VERSION_CLIENT, 0);
		if (!proxy)
			return;

		struct client_obj *c = calloc(1, sizeof(*c));
		c->pa = pa;
		c->id = id;
		c->proxy = proxy;
		pw_client_add_listener((struct pw_client *)proxy, &c->object_listener, &client_events, c);
		spa_list_append(&pa->clients, &c->link);

	} else if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		const char *media_class = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : NULL;
		if (!spa_streq(media_class, "Stream/Output/Audio"))
			return;

		struct pw_proxy *proxy = pw_registry_bind(pa->registry, id, type, PW_VERSION_NODE, 0);
		if (!proxy)
			return;

		struct node_obj *n = calloc(1, sizeof(*n));
		n->pa = pa;
		n->id = id;
		n->proxy = proxy;
		n->client_id = SPA_ID_INVALID;
		const char *client_id = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
		if (client_id)
			n->client_id = (uint32_t)strtoul(client_id, NULL, 10);
		pw_node_add_listener((struct pw_node *)proxy, &n->object_listener, &node_events, n);
		spa_list_append(&pa->nodes, &n->link);

	} else if (spa_streq(type, PW_TYPE_INTERFACE_Port) && props) {
		add_port(pa, id, props);
		update_links(pa);
	}
}

static void registry_global_remove(void *data, uint32_t id)
{
	struct sgc_pw_audio *pa = data;
	struct client_obj *c, *ct;
	struct node_obj *n, *nt;
	struct port_obj *p, *pt;

	spa_list_for_each_safe(c, ct, &pa->clients, link)
	{
		if (c->id == id)
			client_free(c);
	}
	spa_list_for_each_safe(n, nt, &pa->nodes, link)
	{
		if (n->id == id)
			node_free(n);
	}
	spa_list_for_each_safe(p, pt, &pa->ports, link)
	{
		if (p->id == id)
			port_free(p);
	}
	update_links(pa);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	(void)data;
	(void)seq;

	if (id == PW_ID_CORE)
		sgc_log(SGC_LOG_WARNING, "PipeWire error: %s (%s)", message, spa_strerror(res));
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	struct sgc_pw_audio *pa = data;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR)
		sgc_log(SGC_LOG_WARNING, "PipeWire capture stream error: %s", error ? error : "unknown");

	uint32_t node_id = pw_stream_get_node_id(pa->stream);
	if (node_id != pa->stream_node_id) {
		pa->stream_node_id = node_id;
		update_links(pa);
	}
}

static void stream_process(void *data)
{
	struct sgc_pw_audio *pa = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(pa->stream);

	if (!b)
		return;

	struct spa_data *d = &b->buffer->datas[0];
	if (d->data && d->chunk) {
		uint32_t offset = SPA_MIN(d->chunk->offset, d->maxsize);
		uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - offset);
		uint32_t frames = size / (uint32_t)(sizeof(float) * SGC_PW_CHANNELS);

		if (frames) {
			uint64_t duration = (uint64_t)frames * SPA_NSEC_PER_SEC / pa->rate;
			pa->cb(pa->cb_param, SPA_PTROFF(d->data, offset, const float), frames, pa->rate,
			       now_ns() - duration);
		}
	}

	pw_stream_queue_buffer(pa->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_state_changed,
	.process = stream_process,
};

static bool connect_stream(struct sgc_pw_audio *pa)
{
	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Production",
		PW_KEY_NODE_NAME, "obs-steam-game-capture", PW_KEY_NODE_DESCRIPTION, "OBS Steam Game Capture",
		PW_KEY_NODE_AUTOCONNECT, "false", PW_KEY_NODE_DONT_RECONNECT, "true", NULL);

	pa->stream = pw_stream_new(pa->core, "OBS Steam Game Capture", props);
	if (!pa->stream)
		return false;
	pw_stream_add_listener(pa->stream, &pa->stream_listener, &stream_events, pa);

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info_raw info = {
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = pa->rate,
		.channels = SGC_PW_CHANNELS,
	};
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	const struct spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

	/* No AUTOCONNECT: we create the links to the game's streams ourselves. */
	return pw_stream_connect(pa->stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1) >= 0;
}

struct sgc_pw_audio *sgc_pw_audio_create(uint32_t rate, sgc_audio_cb cb, void *param)
{
	struct sgc_pw_audio *pa = calloc(1, sizeof(*pa));

	pa->rate = rate;
	pa->cb = cb;
	pa->cb_param = param;
	pa->stream_node_id = SPA_ID_INVALID;
	spa_list_init(&pa->clients);
	spa_list_init(&pa->nodes);
	spa_list_init(&pa->ports);
	spa_list_init(&pa->links);

	pa->loop = pw_thread_loop_new("sgc-pipewire", NULL);
	if (!pa->loop)
		goto fail;
	pa->context = pw_context_new(pw_thread_loop_get_loop(pa->loop), NULL, 0);
	if (!pa->context || pw_thread_loop_start(pa->loop) < 0)
		goto fail;

	pw_thread_loop_lock(pa->loop);

	pa->core = pw_context_connect(pa->context, NULL, 0);
	if (!pa->core) {
		pw_thread_loop_unlock(pa->loop);
		sgc_log(SGC_LOG_WARNING, "Cannot connect to PipeWire, game audio disabled");
		goto fail;
	}
	pw_core_add_listener(pa->core, &pa->core_listener, &core_events, pa);

	pa->registry = pw_core_get_registry(pa->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(pa->registry, &pa->registry_listener, &registry_events, pa);

	bool ok = connect_stream(pa);
	pw_thread_loop_unlock(pa->loop);
	if (!ok) {
		sgc_log(SGC_LOG_WARNING, "Cannot create PipeWire capture stream");
		goto fail;
	}
	return pa;

fail:
	sgc_pw_audio_destroy(pa);
	return NULL;
}

void sgc_pw_audio_destroy(struct sgc_pw_audio *pa)
{
	if (!pa)
		return;

	if (pa->loop) {
		pw_thread_loop_lock(pa->loop);

		struct link_obj *l, *lt;
		spa_list_for_each_safe(l, lt, &pa->links, link) link_free(l);
		if (pa->stream) {
			spa_hook_remove(&pa->stream_listener);
			pw_stream_disconnect(pa->stream);
			pw_stream_destroy(pa->stream);
		}

		struct client_obj *c, *ct;
		struct node_obj *n, *nt;
		struct port_obj *p, *pt;
		spa_list_for_each_safe(c, ct, &pa->clients, link) client_free(c);
		spa_list_for_each_safe(n, nt, &pa->nodes, link) node_free(n);
		spa_list_for_each_safe(p, pt, &pa->ports, link) port_free(p);

		if (pa->registry) {
			spa_hook_remove(&pa->registry_listener);
			pw_proxy_destroy((struct pw_proxy *)pa->registry);
		}
		if (pa->core) {
			spa_hook_remove(&pa->core_listener);
			pw_core_disconnect(pa->core);
		}

		pw_thread_loop_unlock(pa->loop);
		pw_thread_loop_stop(pa->loop);
	}

	if (pa->context)
		pw_context_destroy(pa->context);
	if (pa->loop)
		pw_thread_loop_destroy(pa->loop);
	free(pa->match);
	free(pa);
}

void sgc_pw_audio_set_targets(struct sgc_pw_audio *pa, const uint32_t *pids, size_t n_pids, const char *match)
{
	pw_thread_loop_lock(pa->loop);

	pa->n_target_pids = 0;
	for (size_t i = 0; i < n_pids && pa->n_target_pids < MAX_TARGET_PIDS; i++) {
		if (pids[i])
			pa->target_pids[pa->n_target_pids++] = pids[i];
	}
	replace_str(&pa->match, match && *match ? match : NULL);
	update_links(pa);

	pw_thread_loop_unlock(pa->loop);
}

void sgc_pw_audio_get_status(struct sgc_pw_audio *pa, uint32_t *matched_streams, uint32_t *links)
{
	struct node_obj *n;
	struct link_obj *l;

	*matched_streams = 0;
	*links = 0;

	pw_thread_loop_lock(pa->loop);
	spa_list_for_each(n, &pa->nodes, link)
	{
		if (n->linked)
			(*matched_streams)++;
	}
	spa_list_for_each(l, &pa->links, link)
	{
		if (!l->dead)
			(*links)++;
	}
	pw_thread_loop_unlock(pa->loop);
}
