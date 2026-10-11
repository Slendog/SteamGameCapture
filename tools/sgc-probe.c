// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sgc-probe: exercises the capture backends without OBS.
 *
 *   sgc-probe                     list detected Steam game windows
 *   sgc-probe --grab out.ppm      save one frame of the selected game
 *   sgc-probe --bench 120         time N frame grabs
 *   sgc-probe --audio 5           capture game audio for N seconds
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "sgc-pipewire.h"
#include "sgc-x11.h"

#define MAX_GAMES 32

static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t audio_frames;
static float audio_peak;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void on_audio(void *param, const float *samples, uint32_t frames, uint32_t rate, uint64_t timestamp_ns)
{
	(void)param;
	(void)rate;
	(void)timestamp_ns;

	pthread_mutex_lock(&audio_mutex);
	audio_frames += frames;
	for (uint32_t i = 0; i < frames * SGC_PW_CHANNELS; i++) {
		float v = fabsf(samples[i]);
		if (v > audio_peak)
			audio_peak = v;
	}
	pthread_mutex_unlock(&audio_mutex);
}

static int grab_frame(struct sgc_x11 *x, struct sgc_x11_frame *frame)
{
	for (int attempt = 0; attempt < 20; attempt++) {
		switch (sgc_x11_grab(x, frame)) {
		case SGC_GRAB_OK:
			return 0;
		case SGC_GRAB_UNAVAILABLE:
			usleep(100000);
			break;
		default:
			return -1;
		}
	}
	return -1;
}

static int write_ppm(const char *path, const struct sgc_x11_frame *f)
{
	FILE *file = fopen(path, "wb");
	if (!file)
		return -1;

	fprintf(file, "P6\n%u %u\n255\n", f->width, f->height);
	for (uint32_t y = 0; y < f->height; y++) {
		const uint8_t *row = f->data + (size_t)y * f->linesize;
		for (uint32_t px = 0; px < f->width; px++) {
			const uint8_t rgb[3] = {row[px * 4 + 2], row[px * 4 + 1], row[px * 4]};
			fwrite(rgb, 1, 3, file);
		}
	}
	return fclose(file);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--display NAME] [--app-id N] [--grab FILE.ppm] [--bench FRAMES] [--audio SECONDS] "
		"[--match TEXT]\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *display = NULL, *grab_path = NULL, *match = NULL;
	unsigned int app_id = 0, bench = 0, audio_seconds = 0;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *val = i + 1 < argc ? argv[i + 1] : NULL;

		if (!val) {
			usage(argv[0]);
			return 2;
		}
		i++;
		if (!strcmp(arg, "--display"))
			display = val;
		else if (!strcmp(arg, "--app-id"))
			app_id = (unsigned int)strtoul(val, NULL, 10);
		else if (!strcmp(arg, "--grab"))
			grab_path = val;
		else if (!strcmp(arg, "--bench"))
			bench = (unsigned int)strtoul(val, NULL, 10);
		else if (!strcmp(arg, "--audio"))
			audio_seconds = (unsigned int)strtoul(val, NULL, 10);
		else if (!strcmp(arg, "--match"))
			match = val;
		else {
			usage(argv[0]);
			return 2;
		}
	}

	struct sgc_x11 *x = sgc_x11_create(display);
	if (!x) {
		fprintf(stderr, "Cannot open X display %s\n", display ? display : getenv("DISPLAY"));
		return 1;
	}

	struct sgc_window_info games[MAX_GAMES];
	size_t count = sgc_x11_find_games(x, games, MAX_GAMES);
	printf("%zu Steam game window(s):\n", count);
	for (size_t i = 0; i < count; i++) {
		const struct sgc_window_info *g = &games[i];
		printf("  0x%08x  AppID %-8u pid %-7u xres-pid %-7u %ux%u %s  class=%s  \"%s\"\n", g->window,
		       g->app_id, g->pid, g->xres_pid, g->width, g->height, g->viewable ? "viewable" : "hidden",
		       g->wm_class, g->title);
	}

	int best = sgc_x11_pick_game(games, count, app_id);
	if (best < 0) {
		sgc_x11_destroy(x);
		return count ? 1 : 0;
	}
	const struct sgc_window_info *game = &games[best];
	printf("Selected: 0x%08x \"%s\"\n", game->window, game->title);

	int ret = 0;
	sgc_x11_attach(x, game->window);

	if (grab_path) {
		struct sgc_x11_frame frame;
		if (grab_frame(x, &frame) == 0 && write_ppm(grab_path, &frame) == 0) {
			printf("Wrote %ux%u frame to %s\n", frame.width, frame.height, grab_path);
		} else {
			fprintf(stderr, "Frame grab failed\n");
			ret = 1;
		}
	}

	if (bench) {
		struct sgc_x11_frame frame;
		unsigned int ok = 0;
		double start = now_s();
		for (unsigned int i = 0; i < bench; i++)
			ok += sgc_x11_grab(x, &frame) == SGC_GRAB_OK;
		double elapsed = now_s() - start;
		printf("Grabbed %u/%u frames in %.3f s (%.2f ms/frame, %.1f fps max)\n", ok, bench, elapsed,
		       elapsed * 1000.0 / bench, bench / elapsed);
	}

	if (audio_seconds) {
		pw_init(&argc, &argv);
		struct sgc_pw_audio *pa = sgc_pw_audio_create(48000, on_audio, NULL);
		if (!pa) {
			fprintf(stderr, "PipeWire capture unavailable\n");
			ret = 1;
		} else {
			uint32_t pids[2] = {game->pid, game->xres_pid};
			sgc_pw_audio_set_targets(pa, pids, 2, match);

			for (unsigned int s = 0; s < audio_seconds; s++) {
				sleep(1);
				uint32_t streams, links;
				sgc_pw_audio_get_status(pa, &streams, &links);

				pthread_mutex_lock(&audio_mutex);
				printf("audio: %u stream(s), %u link(s), %llu frames, peak %.1f dBFS\n", streams, links,
				       (unsigned long long)audio_frames,
				       audio_peak > 0.0f ? 20.0 * log10(audio_peak) : -INFINITY);
				audio_frames = 0;
				audio_peak = 0.0f;
				pthread_mutex_unlock(&audio_mutex);
			}
			sgc_pw_audio_destroy(pa);
		}
		pw_deinit();
	}

	sgc_x11_destroy(x);
	return ret;
}
