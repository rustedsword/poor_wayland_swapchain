/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
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
#include "test-loop.h"
#include "test-runner.h"

constexpr uint32_t SCANOUT = POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT;
constexpr uint32_t SAMPLING = POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING;

struct client {
	struct poor_wl_display display;
	struct poor_loop *loop;
	struct swapchain_server *server;
	struct poor_wl_registry registry;
	struct poor_wl_callback callback;
	struct poor_wl_compositor compositor;
	struct poor_zwp_linux_dmabuf_v1 dmabuf;
	struct poor_wp_linux_drm_syncobj_manager_v1 syncobj;
	struct poor_wl_surface surface;
	struct poor_wl_swapchain swapchain;
	dev_t device;
	unsigned devices, frames, drawn, idle, max_frames, allocations, frees, swapchain_dones;
	uint64_t ages[32];
	uint32_t format, fail_format;
	int swapchain_error, fence_fd;
	bool synced, done, error_called, fail_scanout, have_syncobj, partial, idle_resize;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

/* Formats and modifiers of the table: tranche 0 scans out X tiled XRGB8888, tranche 1 samples the rest */
static void server_config(struct swapchain_server *server, uint32_t version)
{
	static const uint32_t formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
	static const uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR, I915_FORMAT_MOD_X_TILED, DRM_FORMAT_MOD_LINEAR,
					      DRM_FORMAT_MOD_INVALID };

	server->dmabuf_version = version;
	server->main_device = makedev(226, 0);
	server->entries = ARRAY_SIZE(formats);
	foreach_array_index(formats, i) {
		server->table[i].format = formats[i];
		server->table[i].padding = 0;
		server->table[i].modifier = modifiers[i];
	}
	server->syncobj_fd = -1;
	server->hold_releases = false;
	server->tranche_count = 2;
	server->tranches[0] = (struct swapchain_server_tranche){ makedev(226, 0), SCANOUT, 1, { 1 } };
	server->tranches[1] = (struct swapchain_server_tranche){ makedev(226, 128), version >= 6 ? SAMPLING : 0, 3, { 0, 2, 3 } };
}

static void server_stop(struct swapchain_server *server)
{
	swapchain_server_stop(server);
	run_until(server->loop, server->display_done);
}

static struct client *client_of(struct poor_wl_swapchain *swapchain)
{
	return container_of(swapchain, struct client, swapchain);
}

static int memfd_allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, unsigned count,
			  const uint64_t (*modifiers)[count], bool scanout)
{
	struct client *c = data;
	int fd;

	if ((scanout && c->fail_scanout) || format == c->fail_format)
		return -ENOTSUP;
	assert(count);
	fd = memfd_create("swapchain-test", MFD_CLOEXEC);
	assert(fd >= 0 && !ftruncate(fd, buffer->width * buffer->height * 4));
	buffer->data = c;
	buffer->modifier = *array_first_ref(modifiers);
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

static const uint32_t xrgb8888[] = { DRM_FORMAT_XRGB8888 };
static const uint32_t rgb565[] = { DRM_FORMAT_RGB565 };
static const uint32_t argb_or_xrgb[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };

static const struct poor_wl_swapchain_rect partial_damage[] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 } };

static void swapchain_frame(struct poor_wl_swapchain *swapchain, uint32_t)
{
	struct client *c = client_of(swapchain);
	struct poor_wl_swapchain_buffer *buffer;
	int fence;

	c->frames++;
	if (c->idle) {
		c->idle--;
		if (c->idle_resize)
			poor_wl_swapchain_resize(swapchain, 2 * swapchain->width, swapchain->height);
		if (c->frames < c->max_frames)
			poor_wl_swapchain_request_frame(swapchain);
		return;
	}
	buffer = poor_wl_swapchain_acquire(swapchain);
	fence = c->fence_fd >= 0 ? signaled_fence(c->fence_fd) : -1;
	assert(buffer->width == swapchain->width && buffer->height == swapchain->height && c->drawn < ARRAY_SIZE(c->ages));
	c->ages[c->drawn++] = buffer->age;
	c->format = buffer->format;
	if (c->partial)
		poor_wl_swapchain_present_damage(swapchain, buffer, fence, partial_damage);
	else
		poor_wl_swapchain_present(swapchain, buffer, fence);
	if (c->frames < c->max_frames)
		poor_wl_swapchain_request_frame(swapchain);
}

static void swapchain_error(struct poor_wl_swapchain *swapchain, int error)
{
	client_of(swapchain)->swapchain_error = error;
}

static void swapchain_done(struct poor_wl_swapchain *swapchain)
{
	client_of(swapchain)->swapchain_dones++;
}

static const struct poor_wl_swapchain_listener swapchain_listener = {
	.device = swapchain_device,
	.frame = swapchain_frame,
	.error = swapchain_error,
	.done = swapchain_done,
};

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
static void client_start_impl(struct client *c, struct swapchain_server *server, size_t count, const uint32_t (*formats)[count])
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);

	c->loop = server->loop;
	c->server = server;
	c->devices = c->frames = c->drawn = c->idle = c->max_frames = c->allocations = c->frees = c->swapchain_dones = 0;
	c->swapchain_error = 0;
	c->fail_format = 0;
	c->fence_fd = -1;
	c->synced = c->done = c->error_called = c->fail_scanout = c->have_syncobj = c->partial = false;
	c->idle_resize = false;
	poor_wl_display_connect_to_fd(&c->display, c->loop, &memory, swapchain_server_connect(server), client_error, client_done);
	poor_wl_display_get_registry(&c->display, &c->registry);
	poor_wl_registry_add_listener(&c->registry, &registry_listener, c);
	client_roundtrip(c);
	poor_wl_compositor_create_surface(&c->compositor, &c->surface);
	poor_wl_swapchain_init(&c->swapchain, c->loop, &c->dmabuf, &c->surface, formats, &swapchain_listener);
	client_roundtrip(c);
}

#define client_start(c, server, formats) client_start_impl(c, server, ARRAY_SIZE(formats), &auto_arr(formats))

static void client_stop(struct client *c)
{
	poor_wl_swapchain_destroy(&c->swapchain);
	poor_wl_surface_destroy(&c->surface);
	client_roundtrip(c);
	run_until(c->loop, c->swapchain_dones);
	assert(c->frees == c->allocations && c->server->buffers_destroyed == c->server->buffers_created);
	poor_wl_display_disconnect(&c->display);
	run_until(c->loop, c->done);
	assert(c->swapchain_dones == 1);
}

static void client_present(struct client *c, int32_t width, int32_t height, unsigned frames)
{
	unsigned commits = c->server->commits;

	c->max_frames = c->frames + frames;
	poor_wl_swapchain_resize(&c->swapchain, width, height);
	poor_wl_swapchain_request_frame(&c->swapchain);
	run_until(c->loop, c->server->commits == commits + frames);
	assert(c->frames == c->max_frames && c->server->width == width && c->server->height == height);
}

TEST(feedback_picks_device_and_modifiers)
{
	const struct poor_wl_swapchain_tranche *tranches;
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	assert(c.devices == 1 && c.device == makedev(226, 0) && c.swapchain.tranche_count == 2);
	tranches = c.swapchain.tranches;
	assert(tranches[0].flags == SCANOUT && tranches[0].counts[0] == 1 && tranches[0].modifiers[0][0] == I915_FORMAT_MOD_X_TILED);
	assert(tranches[1].counts[0] == 2 && tranches[1].modifiers[0][0] == DRM_FORMAT_MOD_LINEAR);
	assert(tranches[1].modifiers[0][1] == DRM_FORMAT_MOD_INVALID);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

/* The compositor shows each buffer right away and releases the previous one, so two buffers are enough */
TEST(frames_are_presented)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 10);
	assert(server.format == DRM_FORMAT_XRGB8888 && server.modifier == I915_FORMAT_MOD_X_TILED);
	assert(server.planes == 1 && server.stride == 64 * 4);
	assert(c.allocations == 2 && server.buffers_created == 2 && !c.frees);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(damage_reaches_compositor)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 1);
	assert(server.damage_count == 1 && !server.damage[0].x && !server.damage[0].y &&
	       server.damage[0].width == INT32_MAX && server.damage[0].height == INT32_MAX);
	c.partial = true;
	client_present(&c, 16, 16, 1);
	assert(server.damage_count == ARRAY_SIZE(partial_damage));
	foreach_array_index(partial_damage, i) {
		const struct swapchain_server_rect *rect = &server.damage[i];

		assert(rect->x == partial_damage[i].x && rect->y == partial_damage[i].y &&
		       rect->width == partial_damage[i].width && rect->height == partial_damage[i].height);
	}
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

/* Two buffers take turns, so the one that comes back holds the frame before the last one, and new ones hold nothing */
TEST(buffers_know_their_age)
{
	static const uint64_t ages[] = { 0, 0, 2, 2, 0, 0, 2 };
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 4);
	client_present(&c, 32, 16, 3);
	assert(c.frames == ARRAY_SIZE(ages) && !memcmp(c.ages, ages, ARRAY_SIZE_BYTES(ages)));
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(frames_may_draw_nothing)
{
	static const uint64_t ages[] = { 0, 0, 2 };
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	unsigned commits;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 2);
	client_roundtrip(&c);
	commits = server.commits;
	c.idle = 2;
	c.max_frames = c.frames + 3;
	poor_wl_swapchain_request_frame(&c.swapchain);
	run_until(&loop, c.frames == c.max_frames);
	client_roundtrip(&c);
	assert(server.commits == commits + 1 && c.allocations == 2);
	assert(c.drawn == ARRAY_SIZE(ages) && !memcmp(c.ages, ages, ARRAY_SIZE_BYTES(ages)));
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(frames_that_draw_nothing_free_replaced_buffers)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 2);
	client_roundtrip(&c);
	c.idle = 1;
	c.idle_resize = true;
	c.max_frames = c.frames + 2;
	poor_wl_swapchain_request_frame(&c.swapchain);
	run_until(&loop, c.frames == c.max_frames);
	client_roundtrip(&c);
	assert(server.width == 32 && c.allocations == 3 && c.frees == 2);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(frames_come_from_loop)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	c.max_frames = 1;
	poor_wl_swapchain_request_frame(&c.swapchain);
	poor_wl_swapchain_resize(&c.swapchain, 16, 16);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	assert(!c.frames);
	run_until(&loop, c.frames == 1);
	client_roundtrip(&c);
	c.max_frames = 2;
	poor_wl_swapchain_request_frame(&c.swapchain);
	poor_wl_swapchain_resize(&c.swapchain, 32, 16);
	assert(c.frames == 1);
	run_until(&loop, c.frames == 2);
	client_roundtrip(&c);
	assert(c.frames == 2 && server.width == 32);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(acquire_starts_frames_right_away)
{
	struct poor_wl_swapchain_buffer *buffer;
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	unsigned commits;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	assert(!poor_wl_swapchain_acquire(&c.swapchain));
	poor_wl_swapchain_resize(&c.swapchain, 16, 16);
	commits = server.commits;
	buffer = poor_wl_swapchain_acquire(&c.swapchain);
	assert(buffer && buffer->width == 16 && !buffer->age && !poor_wl_swapchain_acquire(&c.swapchain));
	poor_wl_swapchain_present(&c.swapchain, buffer, -1);
	assert(!poor_wl_swapchain_acquire(&c.swapchain));
	client_roundtrip(&c);
	assert(server.commits == commits + 1 && !c.frames);
	buffer = poor_wl_swapchain_acquire(&c.swapchain);
	assert(buffer && !buffer->age);
	poor_wl_swapchain_present(&c.swapchain, buffer, -1);
	c.max_frames = 1;
	poor_wl_swapchain_request_frame(&c.swapchain);
	run_until(&loop, c.frames == 1);
	client_roundtrip(&c);
	assert(server.commits == commits + 3 && c.ages[0] == 2 && c.allocations == 2);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(errors_come_from_loop)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	c.fail_format = DRM_FORMAT_XRGB8888;
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	poor_wl_swapchain_resize(&c.swapchain, 16, 16);
	assert(!poor_wl_swapchain_acquire(&c.swapchain) && !c.swapchain_error);
	run_until(&loop, c.swapchain_error);
	assert(c.swapchain_error == ENOTSUP && !c.allocations);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(destroy_drops_pending_error)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	c.fail_format = DRM_FORMAT_XRGB8888;
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	poor_wl_swapchain_resize(&c.swapchain, 16, 16);
	assert(!poor_wl_swapchain_acquire(&c.swapchain));
	client_stop(&c);
	assert(!c.swapchain_error);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

/* Only the second tranche has ARGB8888, which goes before XRGB8888 of the first one */
TEST(first_offered_format_wins)
{
	static const uint32_t formats[] = { DRM_FORMAT_RGB565, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, formats);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 2);
	assert(c.format == DRM_FORMAT_ARGB8888 && server.format == DRM_FORMAT_ARGB8888);
	assert(server.modifier == DRM_FORMAT_MOD_LINEAR);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(allocation_falls_back_to_next_format)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, argb_or_xrgb);
	c.fail_format = DRM_FORMAT_ARGB8888;
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 2);
	assert(c.format == DRM_FORMAT_XRGB8888 && server.modifier == I915_FORMAT_MOD_X_TILED);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(feedback_change_switches_format)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	unsigned allocations;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, argb_or_xrgb);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 2);
	assert(c.format == DRM_FORMAT_ARGB8888);
	allocations = c.allocations;
	server.tranches[1] = (struct swapchain_server_tranche){ makedev(226, 128), 0, 2, { 0, 3 } };
	swapchain_server_send_feedback(&server);
	client_roundtrip(&c);
	client_present(&c, 16, 16, 2);
	assert(c.format == DRM_FORMAT_XRGB8888 && server.format == DRM_FORMAT_XRGB8888 && c.frees == allocations);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(allocation_falls_back_to_next_tranche)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	c.fail_scanout = true;
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 3);
	assert(server.modifier == DRM_FORMAT_MOD_LINEAR);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

/* Buffers of the old size go when the compositor releases them */
TEST(resize_replaces_buffers)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	unsigned allocations;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 5);
	allocations = c.allocations;
	client_present(&c, 32, 16, 5);
	assert(c.frees == allocations && c.allocations == 2 * allocations);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(feedback_change_replaces_buffers)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	unsigned allocations;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 3);
	allocations = c.allocations;
	server.tranches[0].indices[0] = 0;
	swapchain_server_send_feedback(&server);
	client_roundtrip(&c);
	assert(c.devices == 1 && c.swapchain.tranches[0].modifiers[0][0] == DRM_FORMAT_MOD_LINEAR);
	client_present(&c, 64, 32, 3);
	assert(server.modifier == DRM_FORMAT_MOD_LINEAR && c.frees == allocations);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(device_of_sampling_tranche_without_main_device)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 6);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	assert(c.devices == 1 && c.device == makedev(226, 128));
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

/* Tranche 0 only scans out, so only buffers of tranche 1 name the device to import them to */
TEST(sampling_tranche_sets_sampling_device)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 6);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 3);
	assert(server.modifier == I915_FORMAT_MOD_X_TILED && !server.sampling_device);
	c.fail_scanout = true;
	client_present(&c, 32, 32, 3);
	assert(server.modifier == DRM_FORMAT_MOD_LINEAR && server.sampling_device == makedev(226, 128));
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

TEST(format_without_tranche)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, rgb565);
	assert(c.swapchain_error == ENOTSUP && !c.devices);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}

/* The compositor sends no releases with explicit synchronization, so buffers come back through the release points */
TEST(explicit_sync)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;
	int fd;

	loop_init(&loop, &watchdog);
	fd = render_node_open(&loop);
	if (fd < 0) {
		loop_fini(&loop, &watchdog);
		return;
	}
	server_config(&server, 4);
	server.syncobj_fd = fd;
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	assert(c.have_syncobj && !poor_wl_swapchain_set_syncobj(&c.swapchain, &c.syncobj, fd));
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 10);
	assert(server.acquires == 10 && server.acquire_point % 2);
	assert(server.release_point == server.acquire_point + 1);
	c.fence_fd = fd;
	client_present(&c, 64, 32, 10);
	assert(server.acquires == 20 && c.allocations <= ARRAY_SIZE(c.swapchain.buffers));
	client_stop(&c);
	server_stop(&server);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	loop_fini(&loop, &watchdog);
}

/* With all buffers held by the compositor, the next frame waits for a release point */
TEST(explicit_sync_waits_for_releases)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;
	int fd;

	loop_init(&loop, &watchdog);
	fd = render_node_open(&loop);
	if (fd < 0) {
		loop_fini(&loop, &watchdog);
		return;
	}
	server_config(&server, 4);
	server.syncobj_fd = fd;
	server.hold_releases = true;
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	assert(!poor_wl_swapchain_set_syncobj(&c.swapchain, &c.syncobj, fd));
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	c.max_frames = 10;
	poor_wl_swapchain_resize(&c.swapchain, 64, 32);
	poor_wl_swapchain_request_frame(&c.swapchain);
	run_until(&loop, server.commits == ARRAY_SIZE(c.swapchain.buffers));
	client_roundtrip(&c);
	assert(server.commits == ARRAY_SIZE(c.swapchain.buffers) && c.allocations == ARRAY_SIZE(c.swapchain.buffers));
	server.hold_releases = false;
	swapchain_server_release_held(&server);
	run_until(&loop, server.commits == 10);
	assert(c.frames == 10 && c.allocations == ARRAY_SIZE(c.swapchain.buffers));
	client_stop(&c);
	server_stop(&server);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	loop_fini(&loop, &watchdog);
}

/* Buffers that a resize replaced go once the compositor releases them, without another frame */
TEST(explicit_sync_frees_replaced_buffers)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;
	int fd;

	loop_init(&loop, &watchdog);
	fd = render_node_open(&loop);
	if (fd < 0) {
		loop_fini(&loop, &watchdog);
		return;
	}
	server_config(&server, 4);
	server.syncobj_fd = fd;
	server.hold_releases = true;
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	assert(!poor_wl_swapchain_set_syncobj(&c.swapchain, &c.syncobj, fd));
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 64, 32, 2);
	assert(c.allocations == 2 && !c.frees);
	poor_wl_swapchain_resize(&c.swapchain, 32, 32);
	swapchain_server_release_held(&server);
	run_until(&loop, c.frees == 1);
	client_stop(&c);
	server_stop(&server);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	loop_fini(&loop, &watchdog);
}

TEST(explicit_sync_needs_drm_device)
{
	struct poor_loop_timer watchdog;
	struct swapchain_server server;
	struct poor_loop loop;
	struct client c;
	int fd = memfd_create("swapchain-test", MFD_CLOEXEC);

	assert(fd >= 0);
	loop_init(&loop, &watchdog);
	server_config(&server, 4);
	swapchain_server_start(&server, &loop);
	client_start(&c, &server, xrgb8888);
	assert(c.have_syncobj && poor_wl_swapchain_set_syncobj(&c.swapchain, &c.syncobj, fd) == -ENOTTY);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	poor_wl_swapchain_set_allocator(&c.swapchain, &memfd_allocator, &c);
	client_present(&c, 16, 16, 2);
	client_stop(&c);
	server_stop(&server);
	loop_fini(&loop, &watchdog);
}
