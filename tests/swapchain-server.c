/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm.h>
#include <poor_loop_fd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "poor_linux_dmabuf_v1_server_protocol.h"
#include "poor_linux_drm_syncobj_v1_server_protocol.h"
#include "poor_wayland_server_display.h"
#include "poor_wayland_server.h"
#include "swapchain-server.h"

constexpr unsigned MAX_CLIENTS = 4;
constexpr unsigned MAX_FRAMES = 4;

struct test_client {
	struct poor_wl_client client;
	bool used;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

struct surface;

struct buffer {
	struct poor_wl_buffer_resource resource;
	struct surface *pending_on, *current_on;
	int32_t width, height;
	uint32_t format, stride;
	uint64_t modifier;
	unsigned planes;
	dev_t sampling_device;
	uint64_t release_point;
	uint32_t release_syncobj;
};

struct frame {
	struct poor_wl_callback_resource resource;
	struct surface *surface;
};

struct surface {
	struct poor_wl_surface_resource resource;
	struct buffer *pending, *current;
	struct surface_sync *sync;
	struct frame *frames[MAX_FRAMES];
	unsigned frame_count;
};

struct timeline {
	struct poor_wp_linux_drm_syncobj_timeline_v1_resource resource;
	uint32_t syncobj;
};

struct surface_sync {
	struct poor_wp_linux_drm_syncobj_surface_v1_resource resource;
	struct surface *surface;
	struct timeline *acquire, *release;
	uint64_t acquire_point, release_point;
};

struct params {
	struct poor_zwp_linux_buffer_params_v1_resource resource;
	unsigned planes;
	uint32_t stride;
	uint64_t modifier;
	dev_t sampling_device;
};

struct feedback {
	struct poor_zwp_linux_dmabuf_feedback_v1_resource resource;
	struct poor_wl_list link;
};

struct swapchain_server_state swapchain_server;

static struct {
	struct poor_loop *loop;
	struct poor_wl_display display;
	struct poor_wl_global compositor, dmabuf, syncobj;
	struct poor_wl_list feedbacks;
	struct test_client clients[MAX_CLIENTS];
} server;

static void *server_alloc(struct poor_wl_display *, struct poor_wl_client *, size_t size)
{
	return malloc(size);
}

static void server_free(struct poor_wl_display *, void *ptr)
{
	free(ptr);
}

static void free_resource(struct poor_wl_resource *resource)
{
	free(resource);
}

static void syncobj_ioctl(unsigned long request, void *arg)
{
	assert(swapchain_server.syncobj_fd >= 0 && !ioctl(swapchain_server.syncobj_fd, request, arg));
}

static uint64_t syncobj_query(uint32_t syncobj)
{
	uint64_t point;
	struct drm_syncobj_timeline_array query = {
		.handles = (uintptr_t)&syncobj, .points = (uintptr_t)&point, .count_handles = 1
	};

	syncobj_ioctl(DRM_IOCTL_SYNCOBJ_QUERY, &query);
	return point;
}

static void syncobj_signal(uint32_t syncobj, uint64_t point)
{
	struct drm_syncobj_timeline_array signal = {
		.handles = (uintptr_t)&syncobj, .points = (uintptr_t)&point, .count_handles = 1
	};

	syncobj_ioctl(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal);
}

static void buffer_destroy_request(struct poor_wl_client *, struct poor_wl_buffer_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_buffer_interface buffer_impl = {
	.destroy = buffer_destroy_request,
};

static void buffer_destroy(struct poor_wl_resource *resource)
{
	struct buffer *buffer = container_of(resource, struct buffer, resource.resource);

	if (buffer->pending_on)
		buffer->pending_on->pending = nullptr;
	if (buffer->current_on)
		buffer->current_on->current = nullptr;
	swapchain_server.buffers_destroyed++;
	free(buffer);
}

static void frame_destroy(struct poor_wl_resource *resource)
{
	struct frame *frame = container_of(resource, struct frame, resource.resource);
	struct surface *surface = frame->surface;

	for (unsigned i = 0; surface && i < surface->frame_count; i++) {
		if (surface->frames[i] == frame) {
			surface->frames[i] = surface->frames[--surface->frame_count];
			break;
		}
	}
	free(frame);
}

static void surface_destroy_request(struct poor_wl_client *, struct poor_wl_surface_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void surface_attach(struct poor_wl_client *, struct poor_wl_surface_resource *resource, struct poor_wl_buffer_resource *buffer,
			   int32_t, int32_t)
{
	struct surface *surface = container_of(resource, struct surface, resource);

	if (surface->pending)
		surface->pending->pending_on = nullptr;
	surface->pending = buffer ? container_of(buffer, struct buffer, resource) : nullptr;
	if (surface->pending)
		surface->pending->pending_on = surface;
}

static void surface_damage(struct poor_wl_client *, struct poor_wl_surface_resource *, int32_t, int32_t, int32_t, int32_t)
{
}

static void surface_frame(struct poor_wl_client *client, struct poor_wl_surface_resource *resource, uint32_t id)
{
	struct surface *surface = container_of(resource, struct surface, resource);
	struct frame *frame = malloc(sizeof(*frame));

	assert(frame && surface->frame_count < MAX_FRAMES);
	if (poor_wl_callback_resource_init(&frame->resource, client, 1, id) < 0) {
		free(frame);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wl_resource_set_destructor(&frame->resource.resource, frame_destroy);
	frame->surface = surface;
	surface->frames[surface->frame_count++] = frame;
}

/* The rendering of the buffer is done by the commit, which the client waits for with explicit synchronization */
static void surface_sync_commit(struct surface_sync *sync, struct buffer *buffer)
{
	assert(!buffer == !sync->acquire && !buffer == !sync->release);
	if (!buffer)
		return;
	assert(sync->acquire != sync->release || sync->acquire_point < sync->release_point);
	assert(syncobj_query(sync->acquire->syncobj) >= sync->acquire_point);
	buffer->release_syncobj = sync->release->syncobj;
	buffer->release_point = sync->release_point;
	swapchain_server.acquires++;
	swapchain_server.acquire_point = sync->acquire_point;
	swapchain_server.release_point = sync->release_point;
	sync->acquire = sync->release = nullptr;
}

/* The buffer is shown right away: the previous one is released and the frame callbacks are done */
static void surface_commit(struct poor_wl_client *, struct poor_wl_surface_resource *resource)
{
	struct surface *surface = container_of(resource, struct surface, resource);
	struct buffer *buffer = surface->pending;
	unsigned count = surface->frame_count;

	swapchain_server.commits++;
	if (surface->sync)
		surface_sync_commit(surface->sync, buffer);
	if (buffer) {
		surface->pending = nullptr;
		buffer->pending_on = nullptr;
		if (surface->current && surface->current != buffer) {
			if (surface->sync)
				syncobj_signal(surface->current->release_syncobj, surface->current->release_point);
			else
				poor_wl_buffer_send_release(&surface->current->resource);
			surface->current->current_on = nullptr;
		}
		surface->current = buffer;
		buffer->current_on = surface;
		swapchain_server.width = buffer->width;
		swapchain_server.height = buffer->height;
		swapchain_server.format = buffer->format;
		swapchain_server.stride = buffer->stride;
		swapchain_server.modifier = buffer->modifier;
		swapchain_server.planes = buffer->planes;
		swapchain_server.sampling_device = buffer->sampling_device;
	}
	surface->frame_count = 0;
	for (unsigned i = 0; i < count; i++) {
		surface->frames[i]->surface = nullptr;
		poor_wl_callback_send_done(&surface->frames[i]->resource, swapchain_server.commits);
		poor_wl_resource_destroy(&surface->frames[i]->resource.resource);
	}
}

static const struct poor_wl_surface_interface surface_impl = {
	.destroy = surface_destroy_request,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.commit = surface_commit,
	.damage_buffer = surface_damage,
};

static void surface_destroy(struct poor_wl_resource *resource)
{
	struct surface *surface = container_of(resource, struct surface, resource.resource);

	if (surface->pending)
		surface->pending->pending_on = nullptr;
	if (surface->current)
		surface->current->current_on = nullptr;
	if (surface->sync)
		surface->sync->surface = nullptr;
	for (unsigned i = 0; i < surface->frame_count; i++)
		surface->frames[i]->surface = nullptr;
	free(surface);
}

static void compositor_create_surface(struct poor_wl_client *client, struct poor_wl_compositor_resource *resource, uint32_t id)
{
	struct surface *surface = malloc(sizeof(*surface));

	assert(surface);
	if (poor_wl_surface_resource_init(&surface->resource, client, poor_wl_resource_get_version(&resource->resource), id) < 0) {
		free(surface);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wl_surface_resource_set_implementation(&surface->resource, &surface_impl, nullptr, surface_destroy);
	surface->pending = surface->current = nullptr;
	surface->sync = nullptr;
	surface->frame_count = 0;
}

static const struct poor_wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
};

static void bind_compositor(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	struct poor_wl_compositor_resource *compositor = malloc(sizeof(*compositor));

	assert(compositor);
	if (poor_wl_compositor_resource_init(compositor, client, version, id) < 0) {
		free(compositor);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wl_compositor_resource_set_implementation(compositor, &compositor_impl, nullptr, free_resource);
}

static void params_destroy_request(struct poor_wl_client *, struct poor_zwp_linux_buffer_params_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void params_add(struct poor_wl_client *, struct poor_zwp_linux_buffer_params_v1_resource *resource, int32_t fd, uint32_t plane,
		       uint32_t, uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct params *params = container_of(resource, struct params, resource);

	poor_loop_close_fd_or_submit(server.loop, fd, 0);
	if (!plane)
		params->stride = stride;
	params->planes++;
	params->modifier = (uint64_t)modifier_hi << 32 | modifier_lo;
}

static void params_create_immed(struct poor_wl_client *client, struct poor_zwp_linux_buffer_params_v1_resource *resource, uint32_t id,
				int32_t width, int32_t height, uint32_t format, uint32_t)
{
	struct params *params = container_of(resource, struct params, resource);
	struct buffer *buffer = malloc(sizeof(*buffer));

	assert(buffer);
	if (poor_wl_buffer_resource_init(&buffer->resource, client, 1, id) < 0) {
		free(buffer);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wl_buffer_resource_set_implementation(&buffer->resource, &buffer_impl, nullptr, buffer_destroy);
	buffer->pending_on = buffer->current_on = nullptr;
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->stride = params->stride;
	buffer->modifier = params->modifier;
	buffer->planes = params->planes;
	buffer->sampling_device = params->sampling_device;
	swapchain_server.buffers_created++;
}

static void params_set_sampling_device(struct poor_wl_client *, struct poor_zwp_linux_buffer_params_v1_resource *resource,
				       struct poor_wl_array *device)
{
	struct params *params = container_of(resource, struct params, resource);

	assert(device->size == sizeof(params->sampling_device));
	memcpy(&params->sampling_device, device->data, sizeof(params->sampling_device));
}

static const struct poor_zwp_linux_buffer_params_v1_interface params_impl = {
	.destroy = params_destroy_request,
	.add = params_add,
	.create_immed = params_create_immed,
	.set_sampling_device = params_set_sampling_device,
};

static void feedback_destroy_request(struct poor_wl_client *, struct poor_zwp_linux_dmabuf_feedback_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
	.destroy = feedback_destroy_request,
};

static void feedback_destroy(struct poor_wl_resource *resource)
{
	struct feedback *feedback = container_of(resource, struct feedback, resource.resource);

	poor_wl_list_remove(&feedback->link);
	free(feedback);
}

static void send_device(struct poor_wl_array *array, dev_t device)
{
	array->size = 0;
	memcpy(poor_wl_array_add(array, sizeof(device)), &device, sizeof(device));
}

/* Version 6 has no main device */
static void send_feedback(struct poor_zwp_linux_dmabuf_feedback_v1_resource *feedback)
{
	size_t size = swapchain_server.entries * sizeof(swapchain_server.table[0]);
	int fd = memfd_create("swapchain-server", MFD_CLOEXEC);
	char storage[64];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(storage);

	assert(fd >= 0 && write(fd, swapchain_server.table, size) == (ssize_t)size);
	poor_zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, fd, size);
	poor_loop_close_fd_or_submit(server.loop, fd, 0);
	if (poor_wl_resource_get_version(&feedback->resource) < 6) {
		send_device(&array, swapchain_server.main_device);
		poor_zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &array);
	}
	for (unsigned i = 0; i < swapchain_server.tranche_count; i++) {
		const struct swapchain_server_tranche *tranche = &swapchain_server.tranches[i];

		send_device(&array, tranche->device);
		poor_zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &array);
		array.size = 0;
		memcpy(poor_wl_array_add(&array, tranche->count * sizeof(tranche->indices[0])), tranche->indices,
		       tranche->count * sizeof(tranche->indices[0]));
		poor_zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &array);
		poor_zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, tranche->flags);
		poor_zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
	}
	poor_zwp_linux_dmabuf_feedback_v1_send_done(feedback);
	swapchain_server.feedbacks++;
}

static void feedback_new(struct poor_wl_client *client, struct poor_zwp_linux_dmabuf_v1_resource *resource, uint32_t id)
{
	struct feedback *feedback = malloc(sizeof(*feedback));

	assert(feedback);
	if (poor_zwp_linux_dmabuf_feedback_v1_resource_init(&feedback->resource, client,
							    poor_wl_resource_get_version(&resource->resource), id) < 0) {
		free(feedback);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_zwp_linux_dmabuf_feedback_v1_resource_set_implementation(&feedback->resource, &feedback_impl, nullptr, feedback_destroy);
	poor_wl_list_insert(&server.feedbacks, &feedback->link);
	send_feedback(&feedback->resource);
}

static void dmabuf_destroy_request(struct poor_wl_client *, struct poor_zwp_linux_dmabuf_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void dmabuf_create_params(struct poor_wl_client *client, struct poor_zwp_linux_dmabuf_v1_resource *resource, uint32_t id)
{
	struct params *params = malloc(sizeof(*params));

	assert(params);
	if (poor_zwp_linux_buffer_params_v1_resource_init(&params->resource, client, poor_wl_resource_get_version(&resource->resource),
							  id) < 0) {
		free(params);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_zwp_linux_buffer_params_v1_resource_set_implementation(&params->resource, &params_impl, nullptr, free_resource);
	params->planes = 0;
	params->stride = 0;
	params->modifier = 0;
	params->sampling_device = 0;
}

static void dmabuf_get_default_feedback(struct poor_wl_client *client, struct poor_zwp_linux_dmabuf_v1_resource *resource, uint32_t id)
{
	feedback_new(client, resource, id);
}

static void dmabuf_get_surface_feedback(struct poor_wl_client *client, struct poor_zwp_linux_dmabuf_v1_resource *resource, uint32_t id,
					struct poor_wl_surface_resource *)
{
	feedback_new(client, resource, id);
}

static const struct poor_zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = dmabuf_destroy_request,
	.create_params = dmabuf_create_params,
	.get_default_feedback = dmabuf_get_default_feedback,
	.get_surface_feedback = dmabuf_get_surface_feedback,
};

static void bind_dmabuf(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	struct poor_zwp_linux_dmabuf_v1_resource *dmabuf = malloc(sizeof(*dmabuf));

	assert(dmabuf);
	if (poor_zwp_linux_dmabuf_v1_resource_init(dmabuf, client, version, id) < 0) {
		free(dmabuf);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_zwp_linux_dmabuf_v1_resource_set_implementation(dmabuf, &dmabuf_impl, nullptr, free_resource);
}

static void timeline_destroy_request(struct poor_wl_client *, struct poor_wp_linux_drm_syncobj_timeline_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
	.destroy = timeline_destroy_request,
};

static void timeline_destroy(struct poor_wl_resource *resource)
{
	struct timeline *timeline = container_of(resource, struct timeline, resource.resource);
	struct drm_syncobj_destroy destroy = { .handle = timeline->syncobj };

	syncobj_ioctl(DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
	free(timeline);
}

static void surface_sync_destroy_request(struct poor_wl_client *, struct poor_wp_linux_drm_syncobj_surface_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void surface_sync_set_acquire_point(struct poor_wl_client *,
					   struct poor_wp_linux_drm_syncobj_surface_v1_resource *resource,
					   struct poor_wp_linux_drm_syncobj_timeline_v1_resource *timeline, uint32_t point_hi,
					   uint32_t point_lo)
{
	struct surface_sync *sync = container_of(resource, struct surface_sync, resource);

	sync->acquire = container_of(timeline, struct timeline, resource);
	sync->acquire_point = (uint64_t)point_hi << 32 | point_lo;
}

static void surface_sync_set_release_point(struct poor_wl_client *,
					   struct poor_wp_linux_drm_syncobj_surface_v1_resource *resource,
					   struct poor_wp_linux_drm_syncobj_timeline_v1_resource *timeline, uint32_t point_hi,
					   uint32_t point_lo)
{
	struct surface_sync *sync = container_of(resource, struct surface_sync, resource);

	sync->release = container_of(timeline, struct timeline, resource);
	sync->release_point = (uint64_t)point_hi << 32 | point_lo;
}

static const struct poor_wp_linux_drm_syncobj_surface_v1_interface surface_sync_impl = {
	.destroy = surface_sync_destroy_request,
	.set_acquire_point = surface_sync_set_acquire_point,
	.set_release_point = surface_sync_set_release_point,
};

static void surface_sync_destroy(struct poor_wl_resource *resource)
{
	struct surface_sync *sync = container_of(resource, struct surface_sync, resource.resource);

	if (sync->surface)
		sync->surface->sync = nullptr;
	free(sync);
}

static void syncobj_destroy_request(struct poor_wl_client *, struct poor_wp_linux_drm_syncobj_manager_v1_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void syncobj_get_surface(struct poor_wl_client *client, struct poor_wp_linux_drm_syncobj_manager_v1_resource *,
				uint32_t id, struct poor_wl_surface_resource *surface_resource)
{
	struct surface *surface = container_of(surface_resource, struct surface, resource);
	struct surface_sync *sync = malloc(sizeof(*sync));

	assert(sync && !surface->sync);
	if (poor_wp_linux_drm_syncobj_surface_v1_resource_init(&sync->resource, client, 1, id) < 0) {
		free(sync);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wp_linux_drm_syncobj_surface_v1_resource_set_implementation(&sync->resource, &surface_sync_impl, nullptr,
									  surface_sync_destroy);
	sync->surface = surface;
	sync->acquire = sync->release = nullptr;
	surface->sync = sync;
}

static void syncobj_import_timeline(struct poor_wl_client *client, struct poor_wp_linux_drm_syncobj_manager_v1_resource *,
				    uint32_t id, int32_t fd)
{
	struct timeline *timeline = malloc(sizeof(*timeline));
	struct drm_syncobj_handle import = { .fd = fd };

	assert(timeline);
	syncobj_ioctl(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import);
	poor_loop_close_fd_or_submit(server.loop, fd, 0);
	if (poor_wp_linux_drm_syncobj_timeline_v1_resource_init(&timeline->resource, client, 1, id) < 0) {
		free(timeline);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wp_linux_drm_syncobj_timeline_v1_resource_set_implementation(&timeline->resource, &timeline_impl, nullptr,
									   timeline_destroy);
	timeline->syncobj = import.handle;
}

static const struct poor_wp_linux_drm_syncobj_manager_v1_interface syncobj_impl = {
	.destroy = syncobj_destroy_request,
	.get_surface = syncobj_get_surface,
	.import_timeline = syncobj_import_timeline,
};

static void bind_syncobj(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	struct poor_wp_linux_drm_syncobj_manager_v1_resource *syncobj = malloc(sizeof(*syncobj));

	assert(syncobj);
	if (poor_wp_linux_drm_syncobj_manager_v1_resource_init(syncobj, client, version, id) < 0) {
		free(syncobj);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wp_linux_drm_syncobj_manager_v1_resource_set_implementation(syncobj, &syncobj_impl, nullptr, free_resource);
}

static void client_done(struct poor_wl_client *client)
{
	container_of(client, struct test_client, client)->used = false;
	swapchain_server.clients_done++;
}

int swapchain_server_connect(void)
{
	struct poor_wl_connection_memory memory;
	struct test_client *c = nullptr;
	int fds[2];

	for (unsigned i = 0; i < MAX_CLIENTS && !c; i++)
		if (!server.clients[i].used)
			c = &server.clients[i];
	assert(c && !socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	c->used = true;
	memory = POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);
	poor_wl_client_init(&c->client, &server.display, fds[0], &memory, client_done);
	return fds[1];
}

static void display_done(struct poor_wl_display *)
{
	swapchain_server.display_done = true;
}

void swapchain_server_start(struct poor_loop *loop)
{
	server.loop = loop;
	poor_wl_list_init(&server.feedbacks);
	poor_wl_display_init(&server.display, loop, display_done);
	poor_wl_display_set_allocator(&server.display, server_alloc, server_free);
	poor_wl_global_init(&server.compositor, &server.display, &poor_wl_compositor_interface, 4, nullptr, bind_compositor);
	poor_wl_global_init(&server.dmabuf, &server.display, &poor_zwp_linux_dmabuf_v1_interface, swapchain_server.dmabuf_version,
			    nullptr, bind_dmabuf);
	poor_wl_global_init(&server.syncobj, &server.display, &poor_wp_linux_drm_syncobj_manager_v1_interface, 1, nullptr,
			    bind_syncobj);
}

void swapchain_server_send_feedback(void)
{
	struct feedback *feedback;

	poor_wl_list_for_each(feedback, &server.feedbacks, link)
		send_feedback(&feedback->resource);
}

void swapchain_server_stop(void)
{
	poor_wl_display_destroy(&server.display);
}
