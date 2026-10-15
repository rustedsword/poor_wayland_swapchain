/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "example-log.h"
#include "poor_wayland_client_display.h"
#include "poor_wayland_client.h"
#include "xdg-shell-client-protocol.h"

/*
 * A window with an animated pattern, drawn into shared memory once per frame. It runs until its close button is
 * pressed or until SIGINT or SIGTERM. WAYLAND_DEBUG=client shows the protocol.
 */
constexpr int32_t DEFAULT_WIDTH = 640;
constexpr int32_t DEFAULT_HEIGHT = 480;
constexpr int32_t MAX_WIDTH = 3840;
constexpr int32_t MAX_HEIGHT = 2160;
constexpr int32_t BUFFER_SIZE = MAX_WIDTH * MAX_HEIGHT * 4;
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

poor_loop_fd_declare(signal_file);

struct app;

struct buffer {
	struct poor_wl_buffer poor_wl_buffer;
	struct app *app;
	uint32_t *pixels;
	int32_t width, height;
	bool busy;
};

struct app {
	struct poor_loop loop;
	struct poor_wl_display display;
	struct poor_wl_registry registry;
	struct poor_wl_callback sync, frame;
	struct poor_wl_compositor compositor;
	struct poor_wl_shm shm;
	struct poor_xdg_wm_base wm_base;
	struct poor_wl_surface surface;
	struct poor_xdg_surface shell_surface;
	struct poor_xdg_toplevel toplevel;
	struct poor_wl_shm_pool pool;
	struct buffer buffers[2];
	signal_file signals;
	struct poor_loop_op signal_op, truncate_op;
	struct signalfd_siginfo siginfo;
	sigset_t sigset;
	char *pixels;
	int memfd, status;
	int32_t width, height;
	uint32_t time;
	bool have_compositor, have_shm, have_wm_base;
	bool synced, truncated, frame_pending, draw_pending, closing, signals_closed;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

poor_loop_group_attach(signal_file, struct app, signals, signal_op, on_signal);

/* The truncate holds the memfd, so its completion closes it */
static void app_quit(struct app *app, int status)
{
	if (app->closing)
		return;
	app->closing = true;
	app->status = status;
	poor_wl_display_disconnect(&app->display);
	if (app->signal_op.pending)
		io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(&app->loop)), &app->signal_op, 0);
	if (app->memfd >= 0 && !app->truncate_op.pending) {
		poor_loop_close_fd_or_submit(&app->loop, app->memfd, 0);
		app->memfd = -1;
	}
}

static void paint(uint32_t *pixels, int32_t width, int32_t height, uint32_t time)
{
	uint32_t t = time / 8;

	for (int32_t y = 0; y < height; y++) {
		for (int32_t x = 0; x < width; x++) {
			uint32_t r = (x + t) & 0xff, g = (y + t / 2) & 0xff, b = ((x ^ y) - t) & 0xff;

			pixels[y * width + x] = 0xff000000 | r << 16 | g << 8 | b;
		}
	}
}

static void draw(struct app *app);

static void buffer_release(void *data, struct poor_wl_buffer *)
{
	struct buffer *buffer = data;
	struct app *app = buffer->app;

	buffer->busy = false;
	if (app->draw_pending) {
		app->draw_pending = false;
		draw(app);
	}
}

static const struct poor_wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void frame_done(void *data, struct poor_wl_callback *frame, uint32_t time)
{
	struct app *app = data;

	poor_wl_callback_destroy(frame);
	app->frame_pending = false;
	app->time = time;
	draw(app);
}

static const struct poor_wl_callback_listener frame_listener = {
	.done = frame_done,
};

/* A buffer of another size is replaced, its memory is free once it is destroyed */
static void draw(struct app *app)
{
	struct buffer *buffer = nullptr;

	for (int i = 0; i < 2 && !buffer; i++)
		if (!app->buffers[i].busy)
			buffer = &app->buffers[i];
	if (!buffer) {
		app->draw_pending = true;
		return;
	}
	if (buffer->width != app->width || buffer->height != app->height) {
		if (buffer->width)
			poor_wl_buffer_destroy(&buffer->poor_wl_buffer);
		poor_wl_shm_pool_create_buffer(&app->pool, &buffer->poor_wl_buffer, (buffer - app->buffers) * BUFFER_SIZE, app->width,
					  app->height, app->width * 4, POOR_WL_SHM_FORMAT_XRGB8888);
		poor_wl_buffer_add_listener(&buffer->poor_wl_buffer, &buffer_listener, buffer);
		buffer->width = app->width;
		buffer->height = app->height;
	}
	paint(buffer->pixels, buffer->width, buffer->height, app->time);
	poor_wl_surface_attach(&app->surface, &buffer->poor_wl_buffer, 0, 0);
	poor_wl_surface_damage(&app->surface, 0, 0, INT32_MAX, INT32_MAX);
	poor_wl_surface_frame(&app->surface, &app->frame);
	poor_wl_callback_add_listener(&app->frame, &frame_listener, app);
	poor_wl_surface_commit(&app->surface);
	buffer->busy = true;
	app->frame_pending = true;
}

static void shell_surface_configure(void *data, struct poor_xdg_surface *shell_surface, uint32_t serial)
{
	struct app *app = data;

	poor_xdg_surface_ack_configure(shell_surface, serial);
	if (!app->frame_pending)
		draw(app);
}

static const struct poor_xdg_surface_listener shell_surface_listener = {
	.configure = shell_surface_configure,
};

static int32_t window_size(int32_t size, int32_t fallback, int32_t max)
{
	return size <= 0 ? fallback : size < max ? size : max;
}

static void toplevel_configure(void *data, struct poor_xdg_toplevel *, int32_t width, int32_t height, struct poor_wl_array *)
{
	struct app *app = data;

	app->width = window_size(width, DEFAULT_WIDTH, MAX_WIDTH);
	app->height = window_size(height, DEFAULT_HEIGHT, MAX_HEIGHT);
}

static void toplevel_close(void *data, struct poor_xdg_toplevel *)
{
	app_quit(data, 0);
}

static const struct poor_xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void wm_base_ping(void *, struct poor_xdg_wm_base *wm_base, uint32_t serial)
{
	poor_xdg_wm_base_pong(wm_base, serial);
}

static const struct poor_xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* The window needs the globals and the memfd, whichever is ready last creates it */
static void app_start(struct app *app)
{
	if (!app->synced || !app->truncated)
		return;
	app->pixels = mmap(nullptr, 2 * BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, app->memfd, 0);
	if (app->pixels == MAP_FAILED) {
		app->pixels = nullptr;
		log_printf("simple-shm: mmap: %s\n", strerror(errno));
		app_quit(app, 1);
		return;
	}
	poor_wl_shm_create_pool(&app->shm, &app->pool, app->memfd, 2 * BUFFER_SIZE);
	poor_loop_close_fd_or_submit(&app->loop, app->memfd, 0);
	app->memfd = -1;
	for (int i = 0; i < 2; i++)
		app->buffers[i].pixels = (uint32_t *)(app->pixels + i * BUFFER_SIZE);
	poor_wl_compositor_create_surface(&app->compositor, &app->surface);
	poor_xdg_wm_base_get_xdg_surface(&app->wm_base, &app->shell_surface, &app->surface);
	poor_xdg_surface_add_listener(&app->shell_surface, &shell_surface_listener, app);
	poor_xdg_surface_get_toplevel(&app->shell_surface, &app->toplevel);
	poor_xdg_toplevel_add_listener(&app->toplevel, &toplevel_listener, app);
	poor_xdg_toplevel_set_title(&app->toplevel, "poor_wayland");
	poor_xdg_toplevel_set_app_id(&app->toplevel, "poor_wayland.simple-shm");
	poor_wl_surface_commit(&app->surface);
}

static void registry_global(void *data, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t)
{
	struct app *app = data;

	if (!strcmp(interface, poor_wl_compositor_interface.name) && !app->have_compositor) {
		app->have_compositor = poor_wl_registry_bind(registry, name, &poor_wl_compositor_interface, 1, &app->compositor);
	} else if (!strcmp(interface, poor_wl_shm_interface.name) && !app->have_shm) {
		app->have_shm = poor_wl_registry_bind(registry, name, &poor_wl_shm_interface, 1, &app->shm);
	} else if (!strcmp(interface, poor_xdg_wm_base_interface.name) && !app->have_wm_base) {
		app->have_wm_base = poor_wl_registry_bind(registry, name, &poor_xdg_wm_base_interface, 1, &app->wm_base);
		poor_xdg_wm_base_add_listener(&app->wm_base, &wm_base_listener, app);
	}
}

static void registry_global_remove(void *, struct poor_wl_registry *, uint32_t)
{
}

static const struct poor_wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* The sync callback comes after all globals were announced */
static void synced(void *data, struct poor_wl_callback *callback, uint32_t)
{
	struct app *app = data;

	poor_wl_callback_destroy(callback);
	if (!app->have_compositor || !app->have_shm || !app->have_wm_base) {
		log_printf("simple-shm: the compositor has no wl_compositor, wl_shm or xdg_wm_base\n");
		app_quit(app, 1);
		return;
	}
	app->synced = true;
	app_start(app);
}

static const struct poor_wl_callback_listener sync_listener = {
	.done = synced,
};

static void display_error(struct poor_wl_display *display)
{
	struct app *app = container_of(display, struct app, display);
	int error = poor_wl_display_get_error(display);
	const struct poor_wl_interface *interface;
	uint32_t code, id;

	if (error == EPROTO) {
		code = poor_wl_display_get_protocol_error(display, &interface, &id);
		log_printf("simple-shm: protocol error %u on %s#%u\n", code, interface ? interface->name : "[unknown]", id);
	} else {
		log_printf("simple-shm: connection: %s\n", strerror(error));
	}
	app_quit(app, 1);
}

static void display_done(struct poor_wl_display *display)
{
	poor_loop_stop(&container_of(display, struct app, display)->loop);
}

static void on_truncate(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct app *app = container_of(op, struct app, truncate_op);

	if (app->closing) {
		poor_loop_close_fd_or_submit(&app->loop, app->memfd, 0);
		app->memfd = -1;
		return;
	}
	if (cqe->res < 0) {
		log_printf("simple-shm: ftruncate: %s\n", strerror(-cqe->res));
		app_quit(app, 1);
		return;
	}
	app->truncated = true;
	app_start(app);
}

/* Signals are unblocked afterwards, so a second one kills a client that hangs */
POOR_LOOP_GROUP_OP(on_signal, loop, file, op, cqe)
{
	struct app *app = container_of(file, struct app, signals);

	if (cqe->res == sizeof(app->siginfo))
		log_printf("simple-shm: %s\n", strsignal(app->siginfo.ssi_signo));
	sigprocmask(SIG_UNBLOCK, &app->sigset, nullptr);
	app_quit(app, 0);
}

POOR_LOOP_FD_DONE(signal_file, loop, file)
{
	container_of(file, struct app, signals)->signals_closed = true;
}

[[nodiscard]] static int app_init(struct app *app)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(app->in, app->out, app->objects, app->server_objects);
	int fd;

	app->have_compositor = app->have_shm = app->have_wm_base = false;
	app->pixels = nullptr;
	app->memfd = -1;
	app->status = 0;
	app->width = DEFAULT_WIDTH;
	app->height = DEFAULT_HEIGHT;
	app->time = 0;
	app->synced = app->truncated = app->frame_pending = app->draw_pending = app->closing = false;
	app->signals_closed = true;
	for (int i = 0; i < 2; i++) {
		app->buffers[i].app = app;
		app->buffers[i].width = app->buffers[i].height = 0;
		app->buffers[i].busy = false;
	}
	if (poor_wl_display_connect(&app->display, &app->loop, &memory, nullptr, display_error, display_done) < 0) {
		log_printf("simple-shm: can't connect: %s\n", strerror(errno));
		return -1;
	}
	poor_wl_display_get_registry(&app->display, &app->registry);
	poor_wl_registry_add_listener(&app->registry, &registry_listener, app);
	poor_wl_display_sync(&app->display, &app->sync);
	poor_wl_callback_add_listener(&app->sync, &sync_listener, app);

	sigemptyset(&app->sigset);
	sigaddset(&app->sigset, SIGINT);
	sigaddset(&app->sigset, SIGTERM);
	sigprocmask(SIG_BLOCK, &app->sigset, nullptr);
	fd = signalfd(-1, &app->sigset, SFD_CLOEXEC);
	app->memfd = memfd_create("simple-shm", MFD_CLOEXEC);
	if (fd < 0 || app->memfd < 0) {
		log_printf("simple-shm: %s: %s\n", fd < 0 ? "signalfd" : "memfd_create", strerror(errno));
		if (fd >= 0)
			poor_loop_close_fd_or_submit(&app->loop, fd, 0);
		app_quit(app, 1);
		return 0;
	}
	poor_loop_fd_init(&app->signals, fd);
	poor_loop_op_init(&app->signal_op, on_signal);
	app->signals_closed = false;
	io_uring_prep_read(need_sqe(poor_loop_group_get_sqe_or_submit(&app->loop, &app->signals, &app->signal_op)), fd,
			   &app->siginfo, sizeof(app->siginfo), -1);
	poor_loop_op_init(&app->truncate_op, on_truncate);
	io_uring_prep_ftruncate(need_sqe(poor_loop_get_sqe_or_submit(&app->loop, &app->truncate_op)), app->memfd,
				2 * (loff_t)BUFFER_SIZE);
	return 0;
}

/* Write out the log, then wait for the closes, which nobody tracks */
static int app_finish(struct app *app)
{
	int ret = 0;

	while (!ret && (!app->signals_closed || log_pending()))
		ret = poor_loop_run_once(&app->loop);
	if (!ret)
		ret = poor_loop_drain(&app->loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	if (app->pixels)
		munmap(app->pixels, 2 * BUFFER_SIZE);
	poor_loop_exit(&app->loop);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-shm: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	return app->status;
}

int main(void)
{
	static struct app app;
	struct io_uring_params params = {};
	int ret;

	ret = poor_loop_init(&app.loop, 64, &params);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-shm: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	log_init(&app.loop, "simple-shm");
	poor_wl_log_set_handler_client(log_vprintf);
	if (app_init(&app) < 0) {
		app.status = 1;
		return app_finish(&app);
	}
	ret = poor_loop_run(&app.loop);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-shm: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	return app_finish(&app);
}
