/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <gbm.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "example-log.h"
#include "poor_wayland_client_display.h"
#include "poor_wayland_swapchain.h"
#include "xdg-shell-client-protocol.h"

/*
 * A window with an animated pattern, drawn with OpenGL ES on the GPU that the compositor names in its dmabuf
 * feedback and presented through the swapchain, with explicit synchronization if the compositor and the driver
 * have it. It runs until its close button is pressed or until SIGINT or SIGTERM. WAYLAND_DEBUG=client shows the
 * protocol.
 */
constexpr int32_t DEFAULT_WIDTH = 640;
constexpr int32_t DEFAULT_HEIGHT = 480;
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

static const char vertex_source[] =
	"attribute vec2 position;\n"
	"void main() {\n"
	"	gl_Position = vec4(position, 0.0, 1.0);\n"
	"}\n";

static const char fragment_source[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform float time;\n"
	"void main() {\n"
	"	vec2 p = floor(gl_FragCoord.xy);\n"
	"	gl_FragColor = vec4(mod(p.x + time, 256.0), mod(p.y + time / 2.0, 256.0), mod(p.x + p.y - time, 256.0), 255.0) / 255.0;\n"
	"}\n";

/* A triangle that covers the viewport */
static const GLfloat vertices[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };

poor_loop_fd_declare(signal_file);

/* What a buffer of the swapchain is drawn into */
struct target {
	EGLImageKHR image;
	GLuint renderbuffer, framebuffer;
};

struct app {
	struct poor_loop loop;
	struct poor_wl_display display;
	struct poor_wl_registry registry;
	struct poor_wl_callback sync;
	struct poor_wl_compositor compositor;
	struct poor_zwp_linux_dmabuf_v1 dmabuf;
	struct poor_wp_linux_drm_syncobj_manager_v1 syncobj;
	struct poor_xdg_wm_base wm_base;
	struct poor_wl_surface surface;
	struct poor_xdg_surface shell_surface;
	struct poor_xdg_toplevel toplevel;
	struct poor_wl_swapchain swapchain;
	struct poor_wl_gbm gbm;
	struct target targets[POOR_WL_SWAPCHAIN_BUFFERS];
	EGLDisplay egl_display;
	EGLContext egl_context;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_renderbuffer;
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
	GLuint program;
	GLint time_location;
	signal_file signals;
	struct poor_loop_op signal_op;
	struct signalfd_siginfo siginfo;
	sigset_t sigset;
	int status;
	int32_t width, height;
	bool have_compositor, have_dmabuf, have_syncobj, have_wm_base, explicit_sync;
	bool swapchain_started, swapchain_closed, gbm_open, gbm_closed, closing, signals_closed;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

poor_loop_group_attach(signal_file, struct app, signals, signal_op, on_signal);

static struct app *app_of_swapchain(struct poor_wl_swapchain *swapchain)
{
	return container_of(swapchain, struct app, swapchain);
}

static void swapchain_done(struct poor_wl_swapchain *swapchain)
{
	app_of_swapchain(swapchain)->swapchain_closed = true;
}

/* The buffers go before the EGL objects that draw into them, and those before the GBM device */
static void app_quit(struct app *app, int status)
{
	if (app->closing)
		return;
	app->closing = true;
	app->status = status;
	if (app->swapchain_started)
		poor_wl_swapchain_destroy(&app->swapchain, swapchain_done);
	if (app->egl_display != EGL_NO_DISPLAY) {
		if (app->program)
			glDeleteProgram(app->program);
		eglMakeCurrent(app->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (app->egl_context != EGL_NO_CONTEXT)
			eglDestroyContext(app->egl_display, app->egl_context);
		eglTerminate(app->egl_display);
	}
	if (app->gbm_open)
		poor_wl_gbm_close(&app->gbm);
	poor_wl_display_disconnect(&app->display);
	if (app->signal_op.pending)
		io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(&app->loop)), &app->signal_op, 0);
}

static void target_fini(struct app *app, struct target *target)
{
	if (target->framebuffer)
		glDeleteFramebuffers(1, &target->framebuffer);
	if (target->renderbuffer)
		glDeleteRenderbuffers(1, &target->renderbuffer);
	if (target->image != EGL_NO_IMAGE_KHR)
		app->destroy_image(app->egl_display, target->image);
}

/* The buffer object becomes the color buffer of a framebuffer */
static bool target_init(struct app *app, struct target *target, struct gbm_bo *bo)
{
	target->renderbuffer = target->framebuffer = 0;
	target->image = app->create_image(app->egl_display, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, nullptr);
	if (target->image == EGL_NO_IMAGE_KHR)
		return false;
	glGenRenderbuffers(1, &target->renderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, target->renderbuffer);
	app->image_target_renderbuffer(GL_RENDERBUFFER, target->image);
	glGenFramebuffers(1, &target->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, target->renderbuffer);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static struct target *target_of(struct app *app, struct poor_wl_swapchain_buffer *buffer)
{
	return &app->targets[buffer - app->swapchain.buffers];
}

/* GBM allocates, the targets come and go with the buffers */
static int allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, const uint64_t *modifiers,
		    unsigned count, bool scanout)
{
	struct app *app = data;
	struct target *target = target_of(app, buffer);
	int ret = poor_wl_gbm_allocator.allocate(&app->gbm, buffer, format, modifiers, count, scanout);

	if (ret < 0)
		return ret;
	if (!target_init(app, target, buffer->data)) {
		target_fini(app, target);
		for (unsigned i = 0; i < buffer->plane_count; i++)
			poor_loop_close_fd_or_submit(&app->loop, buffer->planes[i].fd, 0);
		poor_wl_gbm_allocator.free(&app->gbm, buffer);
		return -EINVAL;
	}
	return 0;
}

static void free_buffer(void *data, struct poor_wl_swapchain_buffer *buffer)
{
	struct app *app = data;

	target_fini(app, target_of(app, buffer));
	poor_wl_gbm_allocator.free(&app->gbm, buffer);
}

static const struct poor_wl_swapchain_allocator allocator = {
	.allocate = allocate,
	.free = free_buffer,
};

static GLuint shader_compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	char info[512];
	GLint ok;

	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
		log_printf("simple-egl: shader: %s\n", info);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool program_init(struct app *app)
{
	GLuint vertex = shader_compile(GL_VERTEX_SHADER, vertex_source);
	GLuint fragment = shader_compile(GL_FRAGMENT_SHADER, fragment_source);
	GLint ok = 0;

	if (vertex && fragment) {
		app->program = glCreateProgram();
		glAttachShader(app->program, vertex);
		glAttachShader(app->program, fragment);
		glBindAttribLocation(app->program, 0, "position");
		glLinkProgram(app->program);
		glGetProgramiv(app->program, GL_LINK_STATUS, &ok);
	}
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (!ok)
		return false;
	glUseProgram(app->program);
	app->time_location = glGetUniformLocation(app->program, "time");
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glEnableVertexAttribArray(0);
	return true;
}

/* There is no window surface: the context renders into the buffers of the swapchain */
static bool egl_init(struct app *app)
{
	static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

	app->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, app->gbm.device, nullptr);
	if (app->egl_display == EGL_NO_DISPLAY || !eglInitialize(app->egl_display, nullptr, nullptr) ||
	    !eglBindAPI(EGL_OPENGL_ES_API))
		return false;
	app->egl_context = eglCreateContext(app->egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs);
	if (app->egl_context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(app->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, app->egl_context))
		return false;
	app->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	app->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	app->image_target_renderbuffer =
		(PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	if (strstr(eglQueryString(app->egl_display, EGL_EXTENSIONS), "EGL_ANDROID_native_fence_sync")) {
		app->create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
		app->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
		app->dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
	}
	return app->create_image && app->destroy_image && app->image_target_renderbuffer && program_init(app);
}

static void gbm_ready(struct poor_wl_gbm *gbm)
{
	struct app *app = container_of(gbm, struct app, gbm);
	int ret;

	if (gbm->error) {
		log_printf("simple-egl: can't open the GPU of the compositor: %s\n", strerror(gbm->error));
		app_quit(app, 1);
		return;
	}
	if (!egl_init(app)) {
		log_printf("simple-egl: EGL setup failed: %#x\n", eglGetError());
		app_quit(app, 1);
		return;
	}
	if (app->have_syncobj && app->create_sync && app->destroy_sync && app->dup_fence) {
		ret = poor_wl_swapchain_set_syncobj(&app->swapchain, &app->syncobj, gbm->fd);
		if (ret < 0)
			log_printf("simple-egl: no explicit synchronization: %s\n", strerror(-ret));
		app->explicit_sync = !ret;
	}
	log_printf("simple-egl: %s synchronization\n", app->explicit_sync ? "explicit" : "implicit");
	poor_wl_swapchain_set_allocator(&app->swapchain, &allocator, app);
}

static void gbm_done(struct poor_wl_gbm *gbm)
{
	container_of(gbm, struct app, gbm)->gbm_closed = true;
}

static void swapchain_device(struct poor_wl_swapchain *swapchain, dev_t device)
{
	struct app *app = app_of_swapchain(swapchain);

	if (app->gbm_open) {
		log_printf("simple-egl: the compositor moved to another GPU\n");
		app_quit(app, 1);
		return;
	}
	if (poor_wl_gbm_open(&app->gbm, &app->loop, device, gbm_ready, gbm_done) < 0) {
		log_printf("simple-egl: the compositor's device is no GPU: %s\n", strerror(errno));
		app_quit(app, 1);
		return;
	}
	app->gbm_open = true;
}

/* The fence of the drawing exists once it is flushed. Without one the drawing has to be done already. */
static int draw_fence(struct app *app)
{
	static const EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE };
	EGLSyncKHR sync;
	int fd = -1;

	if (!app->explicit_sync) {
		glFlush();
		return -1;
	}
	sync = app->create_sync(app->egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
	glFlush();
	if (sync != EGL_NO_SYNC_KHR) {
		fd = app->dup_fence(app->egl_display, sync);
		app->destroy_sync(app->egl_display, sync);
	}
	if (fd < 0)
		glFinish();
	return fd;
}

/* Each frame asks for the next one. The pattern repeats every 512 steps of time, which keeps floats small. */
static void swapchain_frame(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer, uint32_t time)
{
	struct app *app = app_of_swapchain(swapchain);

	glBindFramebuffer(GL_FRAMEBUFFER, target_of(app, buffer)->framebuffer);
	glViewport(0, 0, buffer->width, buffer->height);
	glUniform1f(app->time_location, time / 8 % 512);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	poor_wl_swapchain_present(swapchain, buffer, draw_fence(app));
	poor_wl_swapchain_request_frame(swapchain);
}

static void swapchain_error(struct poor_wl_swapchain *swapchain, int error)
{
	log_printf("simple-egl: swapchain: %s\n", strerror(error));
	app_quit(app_of_swapchain(swapchain), 1);
}

static void shell_surface_configure(void *data, struct poor_xdg_surface *shell_surface, uint32_t serial)
{
	struct app *app = data;

	poor_xdg_surface_ack_configure(shell_surface, serial);
	poor_wl_swapchain_resize(&app->swapchain, app->width, app->height);
	poor_wl_swapchain_request_frame(&app->swapchain);
}

static const struct poor_xdg_surface_listener shell_surface_listener = {
	.configure = shell_surface_configure,
};

static void toplevel_configure(void *data, struct poor_xdg_toplevel *, int32_t width, int32_t height, struct poor_wl_array *)
{
	struct app *app = data;

	app->width = width > 0 ? width : DEFAULT_WIDTH;
	app->height = height > 0 ? height : DEFAULT_HEIGHT;
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

/* The surface feedback needs version 4 of linux-dmabuf */
static void registry_global(void *data, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct app *app = data;

	if (!strcmp(interface, poor_wl_compositor_interface.name) && !app->have_compositor && version >= 4) {
		app->have_compositor = poor_wl_registry_bind(registry, name, &poor_wl_compositor_interface, 4, &app->compositor);
	} else if (!strcmp(interface, poor_zwp_linux_dmabuf_v1_interface.name) && !app->have_dmabuf && version >= 4) {
		app->have_dmabuf = poor_wl_registry_bind(registry, name, &poor_zwp_linux_dmabuf_v1_interface,
						     version < 6 ? version : 6, &app->dmabuf);
	} else if (!strcmp(interface, poor_wp_linux_drm_syncobj_manager_v1_interface.name) && !app->have_syncobj) {
		app->have_syncobj = poor_wl_registry_bind(registry, name, &poor_wp_linux_drm_syncobj_manager_v1_interface, 1,
							  &app->syncobj);
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
	if (!app->have_compositor || !app->have_dmabuf || !app->have_wm_base) {
		log_printf("simple-egl: the compositor has no wl_compositor 4, zwp_linux_dmabuf_v1 4 or xdg_wm_base\n");
		app_quit(app, 1);
		return;
	}
	poor_wl_compositor_create_surface(&app->compositor, &app->surface);
	poor_xdg_wm_base_get_xdg_surface(&app->wm_base, &app->shell_surface, &app->surface);
	poor_xdg_surface_add_listener(&app->shell_surface, &shell_surface_listener, app);
	poor_xdg_surface_get_toplevel(&app->shell_surface, &app->toplevel);
	poor_xdg_toplevel_add_listener(&app->toplevel, &toplevel_listener, app);
	poor_xdg_toplevel_set_title(&app->toplevel, "poor_wayland");
	poor_xdg_toplevel_set_app_id(&app->toplevel, "poor_wayland.simple-egl");
	poor_wl_swapchain_init(&app->swapchain, &app->loop, &app->dmabuf, &app->surface, GBM_FORMAT_XRGB8888, swapchain_device,
			       swapchain_frame, swapchain_error);
	app->swapchain_started = true;
	poor_wl_surface_commit(&app->surface);
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
		log_printf("simple-egl: protocol error %u on %s#%u\n", code, interface ? interface->name : "[unknown]", id);
	} else {
		log_printf("simple-egl: connection: %s\n", strerror(error));
	}
	app_quit(app, 1);
}

static void display_done(struct poor_wl_display *display)
{
	poor_loop_stop(&container_of(display, struct app, display)->loop);
}

/* Signals are unblocked afterwards, so a second one kills a client that hangs */
POOR_LOOP_GROUP_OP(on_signal, loop, file, op, cqe)
{
	struct app *app = container_of(file, struct app, signals);

	if (cqe->res == sizeof(app->siginfo))
		log_printf("simple-egl: %s\n", strsignal(app->siginfo.ssi_signo));
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

	app->have_compositor = app->have_dmabuf = app->have_syncobj = app->have_wm_base = app->explicit_sync = false;
	app->swapchain_started = app->swapchain_closed = app->gbm_open = app->closing = false;
	app->gbm_closed = false;
	app->signals_closed = true;
	app->egl_display = EGL_NO_DISPLAY;
	app->egl_context = EGL_NO_CONTEXT;
	app->create_sync = nullptr;
	app->destroy_sync = nullptr;
	app->dup_fence = nullptr;
	app->program = 0;
	app->status = 0;
	app->width = DEFAULT_WIDTH;
	app->height = DEFAULT_HEIGHT;
	if (poor_wl_display_connect(&app->display, &app->loop, &memory, nullptr, display_error, display_done) < 0) {
		log_printf("simple-egl: can't connect: %s\n", strerror(errno));
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
	if (fd < 0) {
		log_printf("simple-egl: signalfd: %s\n", strerror(errno));
		app_quit(app, 1);
		return 0;
	}
	poor_loop_fd_init(&app->signals, fd);
	poor_loop_op_init(&app->signal_op, on_signal);
	app->signals_closed = false;
	io_uring_prep_read(need_sqe(poor_loop_group_get_sqe_or_submit(&app->loop, &app->signals, &app->signal_op)), fd,
			   &app->siginfo, sizeof(app->siginfo), -1);
	return 0;
}

/* Write out the log, then wait for the closes, which nobody tracks */
static int app_finish(struct app *app)
{
	int ret = 0;

	while (!ret && (!app->signals_closed || (app->swapchain_started && !app->swapchain_closed) ||
			(app->gbm_open && !app->gbm_closed) || log_pending()))
		ret = poor_loop_run_once(&app->loop);
	if (!ret)
		ret = poor_loop_drain(&app->loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	poor_loop_exit(&app->loop);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-egl: io_uring: %s\n", strerror(-ret));
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
		dprintf(STDERR_FILENO, "simple-egl: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	log_init(&app.loop, "simple-egl");
	poor_wl_log_set_handler_client(log_vprintf);
	if (app_init(&app) < 0) {
		app.status = 1;
		return app_finish(&app);
	}
	ret = poor_loop_run(&app.loop);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-egl: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	return app_finish(&app);
}
