/*
 * nebulights — a screensaver for Wayland compositors (wlroots).
 *
 * Draws a fullscreen overlay on the "overlay" layer once idle is
 * detected (ext-idle-notify-v1) and disappears on the first activity.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "ext-idle-notify-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "inhibit.h"
#include "scene.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

#define FADE_SECONDS 1.5

/* ------------------------------------------------------------------ */

struct app;

struct output {
	struct wl_list link;
	struct app *v;

	struct wl_output *wl_output;
	uint32_t global_name;
	int32_t x, y;        /* position in the compositor's layout */

	/* alive only while the screensaver is active */
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wl_egl_window *egl_window;
	EGLSurface egl_surface;
	struct wl_callback *frame;

	int32_t width, height;
	bool configured;

	/* fps_cap: frame postponed until next_ms, the callback came too early */
	double next_ms;
	bool due;
};

struct app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct ext_idle_notifier_v1 *idle_notifier;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct ext_idle_notification_v1 *idle_notification;

	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLConfig egl_config;

	struct wl_list outputs;

	bool active;
	bool pending;      /* idle arrived, but something inhibits it */
	bool running;
	int timeout_sec;
	bool oneshot;
	struct timespec activated_at;
};

static struct app state = {0};

/* Whether to honour inhibitors reported over D-Bus (--ignore-inhibit disables). */
static bool respect_inhibit = true;

/* ------------------------------------------------------------------ */

static double now_since(const struct timespec *t0)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t0->tv_sec) + (now.tv_nsec - t0->tv_nsec) / 1e9;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void die(const char *msg)
{
	fprintf(stderr, "nebulights: %s\n", msg);
	exit(1);
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */

static void output_render(struct output *o);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct output *o = data;
	(void)time;
	wl_callback_destroy(cb);
	o->frame = NULL;
	if (!o->v->active)
		return;
	/* Every output keeps its own pace: with fps_cap, a frame callback that
	   arrives early is postponed and the main loop draws the frame later. */
	if (scene_fps_cap() > 0 && now_ms() + 1.0 < o->next_ms)
		o->due = true;
	else
		output_render(o);
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

/*
 * One camera for all monitors: each shows its own slice of a shared
 * view, laid out like the outputs in the compositor. The view is
 * centred horizontally on the whole layout and vertically on the
 * biggest monitor, which also sets the scale.
 */
static void output_view(struct output *o, float view[4], float all[4])
{
	struct output *p, *ref = NULL;
	int32_t x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;

	wl_list_for_each(p, &o->v->outputs, link) {
		if (!p->configured)
			continue;
		if (p->x < x0) x0 = p->x;
		if (p->y < y0) y0 = p->y;
		if (p->x + p->width  > x1) x1 = p->x + p->width;
		if (p->y + p->height > y1) y1 = p->y + p->height;
		if (!ref || (int64_t)p->width * p->height >
		            (int64_t)ref->width * ref->height)
			ref = p;
	}

	float half = (float)ref->height * 0.5f;
	float cx = (float)(x0 + x1) * 0.5f;
	float cy = (float)ref->y + half;

	view[0] = ((float)o->x - cx) / half;
	view[1] = ((float)(o->x + o->width) - cx) / half;
	view[2] = (cy - (float)(o->y + o->height)) / half;
	view[3] = (cy - (float)o->y) / half;
	all[0] = ((float)x0 - cx) / half;
	all[1] = ((float)x1 - cx) / half;
	all[2] = (cy - (float)y1) / half;
	all[3] = (cy - (float)y0) / half;
}

static void output_render(struct output *o)
{
	struct app *v = o->v;

	if (!o->configured || o->egl_surface == EGL_NO_SURFACE)
		return;

	if (!eglMakeCurrent(v->egl_display, o->egl_surface, o->egl_surface,
	                    v->egl_context))
		return;

	double t = now_since(&v->activated_at);
	float fade = (float)(t / FADE_SECONDS);
	if (fade > 1.0f)
		fade = 1.0f;

	float view[4], all[4];
	output_view(o, view, all);
	scene_draw(o->width, o->height, view, all, t, fade);

	int cap = scene_fps_cap();
	if (cap > 0) {
		double period = 1000.0 / cap, now = now_ms();
		o->next_ms += period;
		if (o->next_ms < now)
			o->next_ms = now + period;
	}
	o->due = false;

	o->frame = wl_surface_frame(o->surface);
	wl_callback_add_listener(o->frame, &frame_listener, o);

	eglSwapBuffers(v->egl_display, o->egl_surface);
}

/* ------------------------------------------------------------------ */
/* layer surface                                                       */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t w, uint32_t h)
{
	struct output *o = data;

	zwlr_layer_surface_v1_ack_configure(layer, serial);

	if (w == 0 || h == 0)
		return;

	o->width  = (int32_t)w;
	o->height = (int32_t)h;

	if (o->egl_window) {
		wl_egl_window_resize(o->egl_window, o->width, o->height, 0, 0);
	} else {
		o->egl_window = wl_egl_window_create(o->surface, o->width, o->height);
		o->egl_surface = eglCreateWindowSurface(
			o->v->egl_display, o->v->egl_config,
			(EGLNativeWindowType)o->egl_window, NULL);
		if (o->egl_surface == EGL_NO_SURFACE)
			die("failed to create EGL surface");
	}

	struct wl_region *opaque =
		wl_compositor_create_region(o->v->compositor);
	wl_region_add(opaque, 0, 0, o->width, o->height);
	wl_surface_set_opaque_region(o->surface, opaque);
	wl_region_destroy(opaque);

	o->configured = true;
	if (!o->frame)
		output_render(o);
}

static void deactivate(struct app *v);
static void egl_setup(struct app *v);
static void egl_release(struct app *v);

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	struct output *o = data;
	(void)layer;
	deactivate(o->v);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

/* ------------------------------------------------------------------ */
/* activation / deactivation                                           */

static void output_teardown(struct output *o)
{
	struct app *v = o->v;

	if (o->frame) {
		wl_callback_destroy(o->frame);
		o->frame = NULL;
	}
	o->due = false;
	o->next_ms = 0.0;
	if (o->egl_surface != EGL_NO_SURFACE) {
		eglMakeCurrent(v->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		               EGL_NO_CONTEXT);
		eglDestroySurface(v->egl_display, o->egl_surface);
		o->egl_surface = EGL_NO_SURFACE;
	}
	if (o->egl_window) {
		wl_egl_window_destroy(o->egl_window);
		o->egl_window = NULL;
	}
	if (o->layer) {
		zwlr_layer_surface_v1_destroy(o->layer);
		o->layer = NULL;
	}
	if (o->surface) {
		wl_surface_destroy(o->surface);
		o->surface = NULL;
	}
	o->configured = false;
}

static void output_setup(struct output *o)
{
	struct app *v = o->v;

	o->surface = wl_compositor_create_surface(v->compositor);
	o->layer = zwlr_layer_shell_v1_get_layer_surface(
		v->layer_shell, o->surface, o->wl_output,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "nebulights");

	zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
	zwlr_layer_surface_v1_set_anchor(o->layer,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);

	wl_surface_commit(o->surface);
}

static void activate(struct app *v)
{
	if (v->active)
		return;

	/* The GPU context exists only while the screensaver runs — idle
	   waiting should not hold the driver and the render targets. */
	if (v->egl_context == EGL_NO_CONTEXT)
		egl_setup(v);

	v->active = true;
	clock_gettime(CLOCK_MONOTONIC, &v->activated_at);
	scene_reset_clock();

	int n = 0;
	struct output *o;
	wl_list_for_each(o, &v->outputs, link) {
		output_setup(o);
		n++;
	}
	fprintf(stderr, "nebulights: screensaver on (%d %s)\n",
	        n, n == 1 ? "output" : "outputs");
}

static void deactivate(struct app *v)
{
	if (!v->active)
		return;

	v->active = false;

	double secs = now_since(&v->activated_at);
	fprintf(stderr, "nebulights: screensaver off after %.0f s\n", secs);

	struct output *o;
	wl_list_for_each(o, &v->outputs, link)
		output_teardown(o);
	egl_release(v);

	wl_display_flush(v->display);

	if (v->oneshot)
		v->running = false;
}

/* ------------------------------------------------------------------ */
/* idle notify                                                         */

static void idle_idled(void *data, struct ext_idle_notification_v1 *n)
{
	struct app *v = data;
	(void)n;

	if (respect_inhibit && inhibit_active()) {
		/* A video or a game. Don't start, but stay ready —
		   check every few seconds until the inhibitor goes away. */
		v->pending = true;
		fprintf(stderr, "nebulights: idle, but something inhibits "
		        "the screensaver, waiting\n");
		return;
	}
	activate(v);
}

static void idle_resumed(void *data, struct ext_idle_notification_v1 *n)
{
	struct app *v = data;
	(void)n;
	v->pending = false;
	deactivate(v);
}

static const struct ext_idle_notification_v1_listener idle_listener = {
	.idled = idle_idled,
	.resumed = idle_resumed,
};

/* ------------------------------------------------------------------ */
/* keyboard — exit immediately                                         */

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t fmt,
                       int32_t fd, uint32_t size)
{
	(void)d; (void)k; (void)fmt; (void)size;
	close(fd);
}
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *surf, struct wl_array *keys)
{ (void)d; (void)k; (void)s; (void)surf; (void)keys; }
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *surf)
{ (void)d; (void)k; (void)s; (void)surf; }
static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
	(void)k; (void)serial; (void)time; (void)key;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		deactivate(data);
}
static void kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t s,
                          uint32_t md, uint32_t ml, uint32_t lo, uint32_t g)
{ (void)d; (void)k; (void)s; (void)md; (void)ml; (void)lo; (void)g; }
static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl)
{ (void)d; (void)k; (void)r; (void)dl; }

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kbd_keymap,
	.enter = kbd_enter,
	.leave = kbd_leave,
	.key = kbd_key,
	.modifiers = kbd_modifiers,
	.repeat_info = kbd_repeat,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct app *v = data;

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !v->keyboard) {
		v->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(v->keyboard, &keyboard_listener, v);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && v->keyboard) {
		wl_keyboard_release(v->keyboard);
		v->keyboard = NULL;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

/* ------------------------------------------------------------------ */
/* output position                                                     */

static void out_geometry(void *data, struct wl_output *wo, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub, const char *make,
                         const char *model, int32_t transform)
{
	struct output *o = data;
	(void)wo; (void)pw; (void)ph; (void)sub; (void)make; (void)model;
	(void)transform;
	o->x = x;
	o->y = y;
}
static void out_mode(void *d, struct wl_output *wo, uint32_t f,
                     int32_t w, int32_t h, int32_t r)
{ (void)d; (void)wo; (void)f; (void)w; (void)h; (void)r; }
static void out_done(void *d, struct wl_output *wo)
{ (void)d; (void)wo; }
static void out_scale(void *d, struct wl_output *wo, int32_t s)
{ (void)d; (void)wo; (void)s; }
static void out_name(void *d, struct wl_output *wo, const char *n)
{ (void)d; (void)wo; (void)n; }
static void out_desc(void *d, struct wl_output *wo, const char *n)
{ (void)d; (void)wo; (void)n; }

static const struct wl_output_listener output_listener = {
	.geometry = out_geometry,
	.mode = out_mode,
	.done = out_done,
	.scale = out_scale,
	.name = out_name,
	.description = out_desc,
};

/* ------------------------------------------------------------------ */
/* registry                                                            */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
	struct app *v = data;

	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		v->compositor = wl_registry_bind(reg, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		v->layer_shell = wl_registry_bind(reg, name,
			&zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, ext_idle_notifier_v1_interface.name) == 0) {
		v->idle_notifier = wl_registry_bind(reg, name,
			&ext_idle_notifier_v1_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		if (!v->seat) {
			v->seat = wl_registry_bind(reg, name,
				&wl_seat_interface, version < 7 ? version : 7);
			wl_seat_add_listener(v->seat, &seat_listener, v);
		}
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		struct output *o = calloc(1, sizeof(*o));
		o->v = v;
		o->global_name = name;
		o->egl_surface = EGL_NO_SURFACE;
		o->wl_output = wl_registry_bind(reg, name,
			&wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(o->wl_output, &output_listener, o);
		wl_list_insert(&v->outputs, &o->link);

		if (v->active)
			output_setup(o);
	}
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	struct app *v = data;
	(void)reg;

	struct output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &v->outputs, link) {
		if (o->global_name != name)
			continue;
		output_teardown(o);
		wl_list_remove(&o->link);
		wl_output_destroy(o->wl_output);
		free(o);
		break;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

/* ------------------------------------------------------------------ */
/* EGL                                                                 */

static void egl_setup(struct app *v)
{
	v->egl_display = eglGetDisplay((EGLNativeDisplayType)v->display);
	if (v->egl_display == EGL_NO_DISPLAY)
		die("no EGL display");

	if (!eglInitialize(v->egl_display, NULL, NULL))
		die("eglInitialize failed");

	if (!eglBindAPI(EGL_OPENGL_ES_API))
		die("eglBindAPI failed");

	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE,        8,
		EGL_GREEN_SIZE,      8,
		EGL_BLUE_SIZE,       8,
		EGL_NONE,
	};
	EGLint n = 0;
	if (!eglChooseConfig(v->egl_display, cfg_attr, &v->egl_config, 1, &n) || n < 1)
		die("no matching EGL config");

	static const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE,
	};
	v->egl_context = eglCreateContext(v->egl_display, v->egl_config,
	                                  EGL_NO_CONTEXT, ctx_attr);
	if (v->egl_context == EGL_NO_CONTEXT)
		die("failed to create EGL context");
}

static void egl_release(struct app *v)
{
	if (v->egl_context != EGL_NO_CONTEXT) {
		eglMakeCurrent(v->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		               EGL_NO_CONTEXT);
		eglDestroyContext(v->egl_display, v->egl_context);
		v->egl_context = EGL_NO_CONTEXT;
		scene_fini();
	}
	if (v->egl_display != EGL_NO_DISPLAY) {
		eglTerminate(v->egl_display);
		v->egl_display = EGL_NO_DISPLAY;
	}
}

/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	state.running = false;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -t, --timeout SEC     idle time before starting (default 300)\n"
		"  -n, --now             start immediately, exit on input\n"
		"  -I, --ignore-inhibit  start even when idle is inhibited (e.g. video)\n"
		"  -h, --help            show this help\n"
		"  -V, --version         show version\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct app *v = &state;
	v->timeout_sec = 300;
	v->running = true;
	wl_list_init(&v->outputs);

	static const struct option opts[] = {
		{"timeout", required_argument, NULL, 't'},
		{"now",     no_argument,       NULL, 'n'},
		{"ignore-inhibit", no_argument,  NULL, 'I'},
		{"help",    no_argument,       NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{0, 0, 0, 0},
	};

	int c;
	while ((c = getopt_long(argc, argv, "t:nhIV", opts, NULL)) != -1) {
		switch (c) {
		case 't':
			v->timeout_sec = atoi(optarg);
			if (v->timeout_sec < 1)
				die("timeout must be positive");
			break;
		case 'n': v->oneshot = true; break;
		case 'I': respect_inhibit = false; break;
		case 'h': usage(argv[0]); return 0;
		case 'V': printf("nebulights %s\n", VERSION); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	v->display = wl_display_connect(NULL);
	if (!v->display)
		die("cannot connect to the Wayland compositor");

	v->registry = wl_display_get_registry(v->display);
	wl_registry_add_listener(v->registry, &registry_listener, v);
	wl_display_roundtrip(v->display);
	wl_display_roundtrip(v->display);   /* for seat/output events */

	if (!v->compositor)
		die("compositor does not provide wl_compositor");
	if (!v->layer_shell)
		die("compositor does not support wlr-layer-shell-unstable-v1");
	if (wl_list_empty(&v->outputs))
		die("no outputs found");

	if (v->oneshot) {
		activate(v);
	} else {
		fprintf(stderr, "nebulights: waiting for %d s of idle\n",
		        v->timeout_sec);
		if (!v->idle_notifier)
			die("compositor does not support ext-idle-notify-v1");
		if (!v->seat)
			die("no seat found");
		v->idle_notification = ext_idle_notifier_v1_get_idle_notification(
			v->idle_notifier, (uint32_t)v->timeout_sec * 1000, v->seat);
		ext_idle_notification_v1_add_listener(v->idle_notification,
		                                      &idle_listener, v);
	}

	double inhibit_checked = now_ms();

	while (v->running) {
		while (wl_display_prepare_read(v->display) != 0)
			wl_display_dispatch_pending(v->display);
		wl_display_flush(v->display);

		struct pollfd pfd = {
			.fd = wl_display_get_fd(v->display),
			.events = POLLIN,
		};
		/* Postponed activation and the running screensaver need periodic checks
		   whether an inhibitor has appeared or gone away. */
		int wait_ms = (respect_inhibit && (v->pending || v->active))
		            ? 3000 : -1;
		struct output *o;
		wl_list_for_each(o, &v->outputs, link) {
			if (!o->due)
				continue;
			int ms = (int)(o->next_ms - now_ms());
			if (ms < 0)
				ms = 0;
			if (wait_ms < 0 || ms < wait_ms)
				wait_ms = ms;
		}
		int rc = poll(&pfd, 1, wait_ms);
		if (rc < 0) {
			wl_display_cancel_read(v->display);
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd.revents & POLLIN) {
			wl_display_read_events(v->display);
			wl_display_dispatch_pending(v->display);
		} else {
			wl_display_cancel_read(v->display);
		}

		wl_list_for_each(o, &v->outputs, link)
			if (o->due && v->active && now_ms() + 1.0 >= o->next_ms)
				output_render(o);

		/* Not on every poll timeout: with fps_cap that is every frame,
		   and each check is a D-Bus round trip. */
		if (respect_inhibit && (v->pending || v->active) &&
		    now_ms() - inhibit_checked >= 3000.0) {
			inhibit_checked = now_ms();
			bool blocked = inhibit_active();
			if (v->pending && !blocked) {
				fprintf(stderr, "nebulights: inhibitor gone\n");
				v->pending = false;
				activate(v);
			} else if (v->active && blocked) {
				fprintf(stderr, "nebulights: inhibitor appeared\n");
				deactivate(v);
				v->pending = true;
			}
		}
	}

	deactivate(v);
	egl_release(v);
	inhibit_fini();
	wl_display_disconnect(v->display);

	return 0;
}
