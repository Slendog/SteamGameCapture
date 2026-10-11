// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * PipeWire side of the capture: a stereo input stream that gets linked to
 * every playback stream belonging to the game. Streams are matched by the
 * PipeWire client's application.process.id, plus an optional substring
 * match on application/node name and binary.
 *
 * pw_init() must have been called before sgc_pw_audio_create().
 */

#define SGC_PW_CHANNELS 2

struct sgc_pw_audio;

/* Called from the PipeWire thread with interleaved float samples. */
typedef void (*sgc_audio_cb)(void *param, const float *samples, uint32_t frames, uint32_t rate,
			     uint64_t timestamp_ns);

struct sgc_pw_audio *sgc_pw_audio_create(uint32_t rate, sgc_audio_cb cb, void *param);
void sgc_pw_audio_destroy(struct sgc_pw_audio *pa);

/* pids may contain zeros, which are ignored. match may be NULL or "". */
void sgc_pw_audio_set_targets(struct sgc_pw_audio *pa, const uint32_t *pids, size_t n_pids, const char *match);

void sgc_pw_audio_get_status(struct sgc_pw_audio *pa, uint32_t *matched_streams, uint32_t *links);
