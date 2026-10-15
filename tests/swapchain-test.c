/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <poor_loop_fd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "poor_wayland_client_display.h"
#include "poor_wayland_swapchain.h"
#include "swapchain-server.h"
#include "test-ring.h"
#include "test-runner.h"

constexpr uint64_t TIMEOUT_NS = 5'000'000'000;
constexpr uint32_t SCANOUT = POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT;
constexpr uint32_t SAMPLING = POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING;

struct client {
	struct poor_wl_display display;
	struct poor_loop *loop;
	struct poor_wl_registry registry;
	struct poor_wl_callback callback;
	struct poor_wl_compositor compositor;
	struct poor_zwp_linux_dmabuf_v1 dmabuf;
	struct poor_wp_linux_drm_syncobj_manager_v1 syncobj;
	struct poor_wl_surface surface;
	struct poor_wl_swapchain swapchain;
	dev_t device;
	unsigned devices, frames, max_frames, allocations, frees;
	int swapchain_error, fence_fd;
	bool synced, done, error_called, fail_scanout, have_syncobj, swapchain_done;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

static void watchdog_fire(struct poor_loop *, struct poor_loop_timer *)
{
	fprintf(stderr, "loop timed out\n");
	abort();
}

static struct poor_loop_timer watchdog = POOR_LOOP_TIMER_INIT(watchdog_fire);

#define run_until(loop, cond) \
	do { \
		while (!(cond)) \
			assert(!poor_loop_run_once(loop)); \
	} while (0)

static void loop_init(struct poor_loop *loop)
{
	struct io_uring_params params = {};

	test_ring_params(&params);
	assert(!poor_loop_init(loop, 64, &params));
	poor_loop_timer_arm(loop, &watchdog, poor_loop_now() + TIMEOUT_NS);
}

static void loop_fini(struct poor_loop *loop)
{
	poor_loop_timer_disarm(&watchdog);
	assert(!poor_loop_drain(loop, poor_loop_now() + TIMEOUT_NS));
	poor_loop_exit(loop);
}

/* Formats and modifiers of the table: tranche 0 scans out X tiled XRGB8888, tranche 1 samples the rest */
static void server_config(uint32_t version)
{
	static const uint32_t formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
	static const uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR, I915_FORMAT_MOD_X_TILED, DRM_FORMAT_MOD_LINEAR,
					      DRM_FORMAT_MOD_INVALID };

	swapchain_server.dmabuf_version = version;
	swapchain_server.main_device = makedev(226, 0);
	swapchain_server.entries = ARRAY_SIZE(formats);
	for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
		swapchain_server.table[i].format = formats[i];
		swapchain_server.table[i].padding = 0;
		swapchain_server.table[i].modifier = modifiers[i];
	}
	swapchain_server.syncobj_fd = -1;
	swapchain_server.tranche_count = 2;
	swapchain_server.tranches[0] = (struct swapchain_server_tranche){ makedev(226, 0), SCANOUT, 1, { 1 } };
	swapchain_server.tranches[1] = (struct swapchain_server_tranche){ makedev(226, 128), version >= 6 ? SAMPLING : 0, 3, { 0, 2, 3 } };
}

static void server_stop(struct poor_loop *loop)
{
	swapchain_server_stop();
	run_until(loop, swapchain_server.display_done);
}

static struct client *client_of(struct poor_wl_swapchain *swapchain)
{
	return container_of(swapchain, struct client, swapchain);
}

static int memfd_allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t, const uint64_t *modifiers, unsigned count,
			  bool scanout)
{
	struct client *c = data;
	int fd;

	if (scanout && c->fail_scanout)
		return -ENOTSUP;
	assert(count);
	fd = memfd_create("swapchain-test", MFD_CLOEXEC);
	assert(fd >= 0 && !ftruncate(fd, buffer->width * buffer->height * 4));
	buffer->data = c;
	buffer->modifier = modifiers[0];
	buffer->plane_count = 1;
	buffer->planes[0].fd = fd;
	buffer->planes[0].offset = 0;
	buffer->planes[0].stride = buffer->width * 4;
	c->allocations++;
	return 0;
}

static void memfd_free(void *data, struct poor_wl_swapchain_buffer *buffer)
{
	assert(buffer->data == data);
	((struct client *)data)->frees++;
}

static const struct poor_wl_swapchain_allocator memfd_allocator = {
	.allocate = memfd_allocate,
	.free = memfd_free,
};

static void swapchain_device(struct poor_wl_swapchain *swapchain, dev_t device)
{
	struct client *c = client_of(swapchain);

	c->device = device;
	c->devices++;
}

/* A sync_file of a signaled syncobj */
static int signaled_fence(int drm_fd)
{
	struct drm_syncobj_create create = { .flags = DRM_SYNCOBJ_CREATE_SIGNALED };
	struct drm_syncobj_handle export;
	struct drm_syncobj_destroy destroy;

	assert(!ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create));
	export = (struct drm_syncobj_handle){ .handle = create.handle, .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE };
	assert(!ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export));
	destroy = (struct drm_syncobj_destroy){ .handle = create.handle };
	assert(!ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy));
	return export.fd;
}

static void swapchain_frame(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer, uint32_t)
{
	struct client *c = client_of(swapchain);

	assert(buffer->width == swapchain->width && buffer->height == swapchain->height);
	c->frames++;
	poor_wl_swapchain_present(swapchain, buffer, c->fence_fd >= 0 ? signaled_fence(c->fence_fd) : -1);
	if (c->frames < c->max_frames)
		poor_wl_swapchain_request_frame(swapchain);
}

static void swapchain_error(struct poor_wl_swapchain *swapchain, int error)
{
	client_of(swapchain)->swapchain_error = error;
}

static void swapchain_done(struct poor_wl_swapchain *swapchain)
{
	client_of(swapchain)->swapchain_done = true;
}

static void registry_global(void *data, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct client *c = data;

	if (!strcmp(interface, poor_wl_compositor_interface.name))
		poor_wl_registry_bind(registry, name, &poor_wl_compositor_interface, 4, &c->compositor);
	else if (!strcmp(interface, poor_zwp_linux_dmabuf_v1_interface.name))
		poor_wl_registry_bind(registry, name, &poor_zwp_linux_dmabuf_v1_interface, version, &c->dmabuf);
	else if (!strcmp(interface, poor_wp_linux_drm_syncobj_manager_v1_interface.name))
		c->have_syncobj = poor_wl_registry_bind(registry, name, &poor_wp_linux_drm_syncobj_manager_v1_interface, 1,
							&c->syncobj);
}

static void registry_global_remove(void *, struct poor_wl_registry *, uint32_t)
{
}

static const struct poor_wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void sync_done(void *data, struct poor_wl_callback *callback, uint32_t)
{
	((struct client *)data)->synced = true;
	poor_wl_callback_destroy(callback);
}

static const struct poor_wl_callback_listener sync_listener = {
	.done = sync_done,
};

static void client_roundtrip(struct client *c)
{
	c->synced = false;
	poor_wl_display_sync(&c->display, &c->callback);
	poor_wl_callback_add_listener(&c->callback, &sync_listener, c);
	run_until(c->loop, c->synced || c->error_called);
	assert(!c->error_called);
}

static void client_error(struct poor_wl_display *display)
{
	container_of(display, struct client, display)->error_called = true;
}

static void client_done(struct poor_wl_display *display)
{
	container_of(display, struct client, display)->done = true;
}

/* The feedback comes with the roundtrip after the swapchain asked for it */
static void client_start(struct client *c, struct poor_loop *loop, uint32_t format)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);

	c->loop = loop;
	c->devices = c->frames = c->max_frames = c->allocations = c->frees = 0;
	c->swapchain_error = 0;
	c->fence_fd = -1;
	c->synced = c->done = c->error_called = c->fail_scanout = c->have_syncobj = c->swapchain_done = false;
	poor_wl_display_connect_to_fd(&c->display, loop, &memory, swapchain_server_connect(), client_error, client_done);
	poor_wl_display_get_registry(&c->display, &c->registry);
	poor_wl_registry_add_listener(&c->registry, &registry_listener, c);
	client_roundtrip(c);
	poor_wl_compositor_create_surface(&c->compositor, &c->surface);
	poor_wl_swapchain_init(&c->swapchain, loop, &c->dmabuf, &c->surface, format, swapchain_device, swapchain_frame,
			       swapchain_error);
	client_roundtrip(c);
}

static void client_stop(struct client *c)
{
	poor_wl_swapchain_destroy(&c->swapchain, swapchain_done);
	poor_wl_surface_destroy(&c->surface);
	client_roundtrip(c);
	run_until(c->loop, c->swapchain_done);
	assert(c->frees == c->allocations && swapchain_server.buffers_destroyed == swapchain_server.buffers_created);
	poor_wl_display_disconnect(&c->display);
	run_until(c->loop, c->done);
}

static void client_present(struct client *c, int32_t width, int32_t height, unsigned frames)
{
	unsigned commits = swapchain_server.commits;

	c->max_frames = c->frames + frames;
	poor_wl_swapchain_resize(&c->swapchain, width, height);
	poor_wl_swapchain_request_frame(&c->swapchain);
	run_until(c->loop, swapchain_server.commits == commits + frames);
	assert(c->frames == c->max_frames && swapchain_server.width == width && swapchain_server.height == height);
}

TEST(feedback_picks_device_and_modifiers)
{
	const struct poor_wl_swapchain_tranche *tranches;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	server_config(4);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	assert(c.devices == 1 && c.device == makedev(226, 0) && c.swapchain.tranche_count == 2);
	tranches = c.swapchain.tranches;
	assert(tranches[0].flags == SCANOUT && tranches[0].count == 1 && tranches[0].modifiers[0] == I915_FORMAT_MOD_X_TILED);
	assert(tranches[1].count == 2 && tranches[1].modifiers[0] == DRM_FORMAT_MOD_LINEAR);
	assert(tranches[1].modifiers[1] == DRM_FORMAT_MOD_INVALID);
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* The compositor shows each buffer right away and releases the previous one, so two buffers are enough */
TEST(frames_are_presented)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	server_config(4);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 10);
	assert(swapchain_server.format == DRM_FORMAT_XRGB8888 && swapchain_server.modifier == I915_FORMAT_MOD_X_TILED);
	assert(swapchain_server.planes == 1 && swapchain_server.stride == 64 * 4);
	assert(c.allocations == 2 && swapchain_server.buffers_created == 2 && !c.frees);
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(allocation_falls_back_to_next_tranche)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	server_config(4);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	c.fail_scanout = true;
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 3);
	assert(swapchain_server.modifier == DRM_FORMAT_MOD_LINEAR);
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* Buffers of the old size go when the compositor releases them */
TEST(resize_replaces_buffers)
{
	struct poor_loop loop;
	unsigned allocations;
	struct client c;

	loop_init(&loop);
	server_config(4);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 5);
	allocations = c.allocations;
	client_present(&c, 32, 16, 5);
	assert(c.frees == allocations && c.allocations == 2 * allocations);
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(feedback_change_replaces_buffers)
{
	struct poor_loop loop;
	unsigned allocations;
	struct client c;

	loop_init(&loop);
	server_config(4);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 3);
	allocations = c.allocations;
	swapchain_server.tranches[0].indices[0] = 0;
	swapchain_server_send_feedback();
	client_roundtrip(&c);
	assert(c.devices == 1 && c.swapchain.tranches[0].modifiers[0] == DRM_FORMAT_MOD_LINEAR);
	client_present(&c, 64, 32, 3);
	assert(swapchain_server.modifier == DRM_FORMAT_MOD_LINEAR && c.frees == allocations);
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(device_of_sampling_tranche_without_main_device)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	server_config(6);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	assert(c.devices == 1 && c.device == makedev(226, 128));
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* Tranche 0 only scans out, so only buffers of tranche 1 name the device to import them to */
TEST(sampling_tranche_sets_sampling_device)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	server_config(6);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 3);
	assert(swapchain_server.modifier == I915_FORMAT_MOD_X_TILED && !swapchain_server.sampling_device);
	c.fail_scanout = true;
	client_present(&c, 32, 32, 3);
	assert(swapchain_server.modifier == DRM_FORMAT_MOD_LINEAR && swapchain_server.sampling_device == makedev(226, 128));
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(format_without_tranche)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	server_config(4);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_RGB565);
	assert(c.swapchain_error == ENOTSUP && !c.devices);
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static bool node_opened;
static int node_fd;

static void node_open_done(struct poor_loop *, struct poor_loop_op *, const struct io_uring_cqe *cqe)
{
	node_fd = cqe->res;
	node_opened = true;
}

/* The first render node, if it has timeline syncobjs */
static int syncobj_device_open(struct poor_loop *loop)
{
	struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
	struct poor_loop_op op;
	struct io_uring_sqe *sqe;

	poor_loop_op_init(&op, node_open_done);
	node_opened = false;
	sqe = poor_loop_get_sqe_or_submit(loop, &op);
	assert(sqe);
	io_uring_prep_openat(sqe, AT_FDCWD, "/dev/dri/renderD128", O_RDWR | O_CLOEXEC, 0);
	run_until(loop, node_opened);
	if (node_fd >= 0 && (ioctl(node_fd, DRM_IOCTL_GET_CAP, &cap) < 0 || !cap.value)) {
		poor_loop_close_fd_or_submit(loop, node_fd, 0);
		return -1;
	}
	return node_fd;
}

/* The compositor sends no releases with explicit synchronization, so buffers come back through the release points */
TEST(explicit_sync)
{
	struct poor_loop loop;
	struct client c;
	int fd;

	loop_init(&loop);
	fd = syncobj_device_open(&loop);
	if (fd < 0) {
		loop_fini(&loop);
		return;
	}
	server_config(4);
	swapchain_server.syncobj_fd = fd;
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	assert(c.have_syncobj && !poor_wl_swapchain_set_syncobj(&c.swapchain, &c.syncobj, fd));
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 10);
	assert(swapchain_server.acquires == 10 && swapchain_server.acquire_point % 2);
	assert(swapchain_server.release_point == swapchain_server.acquire_point + 1);
	c.fence_fd = fd;
	client_present(&c, 64, 32, 10);
	assert(swapchain_server.acquires == 20 && c.allocations <= POOR_WL_SWAPCHAIN_BUFFERS);
	client_stop(&c);
	server_stop(&loop);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	loop_fini(&loop);
}

TEST(explicit_sync_needs_drm_device)
{
	struct poor_loop loop;
	struct client c;
	int fd = memfd_create("swapchain-test", MFD_CLOEXEC);

	assert(fd >= 0);
	loop_init(&loop);
	server_config(4);
	swapchain_server_start(&loop);
	client_start(&c, &loop, DRM_FORMAT_XRGB8888);
	assert(c.have_syncobj && poor_wl_swapchain_set_syncobj(&c.swapchain, &c.syncobj, fd) == -ENOTTY);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 2);
	client_stop(&c);
	server_stop(&loop);
	loop_fini(&loop);
}
