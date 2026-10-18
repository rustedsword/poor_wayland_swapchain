/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <poor_loop_fd.h>
#include <poor_loop_uring_array.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "poor_wayland_swapchain.h"
#include "swapchain-private.h"

poor_loop_group_attach(poor_wl_swapchain_io, struct poor_wl_swapchain, io, release_op, swapchain_on_release);
poor_loop_group_attach(poor_wl_swapchain_io, struct poor_wl_swapchain, io, timer, swapchain_on_timer);

/* An entry of the format table of the feedback */
struct format_modifier {
	uint32_t format;
	uint32_t padding;
	uint64_t modifier;
};

static struct poor_wl_swapchain *swapchain_of(poor_wl_swapchain_io *io)
{
	return container_of(io, struct poor_wl_swapchain, io);
}

/* Callbacks that a call of the caller leads to come from the loop, never from inside that call */
static void swapchain_kick(struct poor_wl_swapchain *swapchain)
{
	if (!poor_loop_timer_armed(&swapchain->timer))
		poor_loop_group_timer_arm(swapchain->loop, &swapchain->io, &swapchain->timer, 0);
}

static void swapchain_fail(struct poor_wl_swapchain *swapchain, int error)
{
	assert(error > 0);
	if (swapchain->failed)
		return;
	swapchain->failed = true;
	swapchain->error = error;
	swapchain_kick(swapchain);
}

static int drm_ioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do
		ret = ioctl(fd, request, arg);
	while (ret < 0 && (errno == EINTR || errno == EAGAIN));
	return ret;
}

static int syncobj_create(int drm_fd, uint32_t *handle)
{
	struct drm_syncobj_create create = {};

	if (drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0)
		return -errno;
	*handle = create.handle;
	return 0;
}

static void syncobj_destroy(int drm_fd, uint32_t handle)
{
	struct drm_syncobj_destroy destroy = { .handle = handle };

	drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}

static int syncobj_signal(int drm_fd, uint32_t handle, uint64_t point)
{
	struct drm_syncobj_timeline_array signal = {
		.handles = (uintptr_t)&handle, .points = (uintptr_t)&point, .count_handles = 1
	};

	return drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) < 0 ? -errno : 0;
}

static int syncobj_import_fence(int drm_fd, uint32_t handle, uint64_t point, int fence)
{
	struct drm_syncobj_handle import = {
		.handle = handle,
		.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE | DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE,
		.fd = fence,
		.point = point,
	};

	return drm_ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) < 0 ? -errno : 0;
}

/* Each buffer has its own timeline, since the compositor may release buffers in another order than they came */
static int buffer_timeline_init(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer)
{
	struct drm_syncobj_handle export;
	int ret = syncobj_create(swapchain->drm_fd, &buffer->syncobj);

	if (ret < 0)
		return ret;
	export = (struct drm_syncobj_handle){ .handle = buffer->syncobj };
	if (drm_ioctl(swapchain->drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export) < 0) {
		ret = -errno;
		syncobj_destroy(swapchain->drm_fd, buffer->syncobj);
		return ret;
	}
	poor_wp_linux_drm_syncobj_manager_v1_import_timeline(swapchain->syncobj_manager, &buffer->timeline, export.fd);
	buffer->point = 0;
	return 0;
}

/* The acquire point signals with the fence, or right away without one, and the release point once the compositor is done */
static int buffer_set_points(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer, int fence)
{
	uint64_t acquire = buffer->point + 1, release = buffer->point + 2;
	int ret = fence >= 0 ? syncobj_import_fence(swapchain->drm_fd, buffer->syncobj, acquire, fence) :
			       syncobj_signal(swapchain->drm_fd, buffer->syncobj, acquire);

	if (ret < 0)
		return ret;
	buffer->point = release;
	poor_wp_linux_drm_syncobj_surface_v1_set_acquire_point(&swapchain->syncobj_surface, &buffer->timeline, acquire >> 32,
								 acquire & 0xffffffff);
	poor_wp_linux_drm_syncobj_surface_v1_set_release_point(&swapchain->syncobj_surface, &buffer->timeline, release >> 32,
								 release & 0xffffffff);
	return 0;
}

static void buffer_free(struct poor_wl_swapchain_buffer *buffer)
{
	struct poor_wl_swapchain *swapchain = buffer->swapchain;

	poor_wl_buffer_destroy(&buffer->buffer);
	if (swapchain->syncobj_manager) {
		poor_wp_linux_drm_syncobj_timeline_v1_destroy(&buffer->timeline);
		syncobj_destroy(swapchain->drm_fd, buffer->syncobj);
	}
	buffer->allocator->free(buffer->allocator_data, buffer);
	buffer->allocated = buffer->busy = buffer->stale = buffer->waiting = false;
}

/* The eventfd counts the release points that signal */
static void swapchain_wait_releases(struct poor_wl_swapchain *swapchain)
{
	foreach_array_ref(swapchain->buffers, buffer) {
		struct drm_syncobj_eventfd wait = { .handle = buffer->syncobj, .point = buffer->point, .fd = swapchain->eventfd };

		if (!buffer->allocated || !buffer->busy || buffer->waiting)
			continue;
		if (drm_ioctl(swapchain->drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &wait) < 0) {
			swapchain_fail(swapchain, errno);
			return;
		}
		buffer->waiting = true;
	}
}

/* A buffer that the compositor or the caller still uses goes once they are done with it */
static void swapchain_drop_buffers(struct poor_wl_swapchain *swapchain)
{
	foreach_array_ref(swapchain->buffers, buffer) {
		if (!buffer->allocated)
			continue;
		if (buffer->busy || buffer == swapchain->rendering)
			buffer->stale = true;
		else
			buffer_free(buffer);
	}
	if (swapchain->syncobj_manager)
		swapchain_wait_releases(swapchain);
}

static void swapchain_render(struct poor_wl_swapchain *swapchain);

/* With explicit synchronization the release points tell when buffers come back, and these events may not come */
static void buffer_release(void *data, struct poor_wl_buffer *)
{
	struct poor_wl_swapchain_buffer *buffer = data;
	struct poor_wl_swapchain *swapchain = buffer->swapchain;

	if (swapchain->syncobj_manager)
		return;
	buffer->busy = false;
	if (buffer->stale)
		buffer_free(buffer);
	swapchain_render(swapchain);
}

static void swapchain_read_releases(struct poor_wl_swapchain *swapchain)
{
	io_uring_prep_read_array(need_sqe(poor_loop_group_get_sqe_or_submit(swapchain->loop, &swapchain->io, &swapchain->release_op)),
				 swapchain->eventfd, array_ptr(&swapchain->released), -1);
}

/* The timelines tell which buffers the compositor is done with */
static void swapchain_check_releases(struct poor_wl_swapchain *swapchain)
{
	struct poor_wl_swapchain_buffer *busy[ARRAY_SIZE(swapchain->buffers)];
	uint32_t handles[ARRAY_SIZE(busy)];
	uint64_t points[ARRAY_SIZE(busy)];
	struct drm_syncobj_timeline_array query = { .handles = (uintptr_t)handles, .points = (uintptr_t)points };

	foreach_array_ref(swapchain->buffers, buffer) {
		if (buffer->allocated && buffer->busy) {
			busy[query.count_handles] = buffer;
			handles[query.count_handles++] = buffer->syncobj;
		}
	}
	if (!query.count_handles)
		return;
	if (drm_ioctl(swapchain->drm_fd, DRM_IOCTL_SYNCOBJ_QUERY, &query) < 0) {
		swapchain_fail(swapchain, errno);
		return;
	}
	foreach_array_index(arrview_first(query.count_handles, busy), i) {
		if (points[i] < busy[i]->point)
			continue;
		busy[i]->busy = busy[i]->waiting = false;
		if (busy[i]->stale)
			buffer_free(busy[i]);
	}
}

POOR_LOOP_GROUP_OP(swapchain_on_release, loop, io, op, cqe)
{
	struct poor_wl_swapchain *swapchain = swapchain_of(io);

	if (swapchain->failed)
		return;
	if (cqe->res < 0) {
		swapchain_fail(swapchain, -cqe->res);
		return;
	}
	swapchain_read_releases(swapchain);
	swapchain_check_releases(swapchain);
	swapchain_render(swapchain);
}

POOR_LOOP_GROUP_TIMER(swapchain_on_timer, loop, io, timer)
{
	struct poor_wl_swapchain *swapchain = swapchain_of(io);
	int error = swapchain->error;

	if (!error) {
		swapchain_render(swapchain);
		return;
	}
	swapchain->error = 0;
	swapchain->listener->error(swapchain, error);
}

POOR_LOOP_GROUP_DONE(poor_wl_swapchain_io, loop, io)
{
	struct poor_wl_swapchain *swapchain = swapchain_of(io);

	if (swapchain->eventfd >= 0)
		poor_loop_close_fd_or_submit(loop, swapchain->eventfd, 0);
	swapchain->listener->done(swapchain);
}

static const struct poor_wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

/* The tranches come in the order that the compositor prefers, the first one that the allocator manages is used */
static const struct poor_wl_swapchain_tranche *allocate_format(struct poor_wl_swapchain *swapchain,
							      struct poor_wl_swapchain_buffer *buffer, size_t format, int *ret)
{
	foreach_array_const_ref(arrview_first(swapchain->tranche_count, swapchain->tranches), tranche) {
		if (!tranche->counts[format])
			continue;
		*ret = swapchain->allocator->allocate(swapchain->allocator_data, buffer, swapchain->formats[format],
						      tranche->counts[format],
						      arrview_first(tranche->counts[format], tranche->modifiers[format]),
						      tranche->flags & POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT);
		if (*ret >= 0)
			return tranche;
	}
	return nullptr;
}

/* The formats come in the order that the caller prefers */
static int buffer_allocate(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer)
{
	const struct poor_wl_swapchain_tranche *tranche = nullptr;
	int ret = -ENOTSUP;

	buffer->width = swapchain->width;
	buffer->height = swapchain->height;
	foreach_array_index(arrview_first(swapchain->format_count, swapchain->formats), format) {
		tranche = allocate_format(swapchain, buffer, format, &ret);
		if (tranche) {
			buffer->format = swapchain->formats[format];
			break;
		}
	}
	if (!tranche)
		return ret;
	assert(buffer->plane_count && buffer->plane_count <= ARRAY_SIZE(buffer->planes));
	auto planes = arrview_first(buffer->plane_count, buffer->planes);
	buffer->allocator = swapchain->allocator;
	buffer->allocator_data = swapchain->allocator_data;
	if (swapchain->syncobj_manager && (ret = buffer_timeline_init(swapchain, buffer)) < 0) {
		foreach_array_const_ref(planes, plane)
			poor_loop_close_fd_or_submit(swapchain->loop, plane->fd, 0);
		buffer->allocator->free(buffer->allocator_data, buffer);
		return ret;
	}
	poor_zwp_linux_dmabuf_v1_create_params(swapchain->dmabuf, &swapchain->params);
	if (tranche->flags & POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING &&
	    poor_zwp_linux_buffer_params_v1_get_version(&swapchain->params) >=
		    POOR_ZWP_LINUX_BUFFER_PARAMS_V1_SET_SAMPLING_DEVICE_SINCE_VERSION) {
		dev_t device = tranche->device;
		struct poor_wl_array array = { .size = sizeof(device), .alloc = sizeof(device), .data = &device };

		poor_zwp_linux_buffer_params_v1_set_sampling_device(&swapchain->params, &array);
	}
	foreach_array_ref(planes, plane) {
		poor_zwp_linux_buffer_params_v1_add(&swapchain->params, plane->fd, array_ref_index(planes, plane), plane->offset,
						    plane->stride, buffer->modifier >> 32, buffer->modifier & 0xffffffff);
		plane->fd = -1;
	}
	poor_zwp_linux_buffer_params_v1_create_immed(&swapchain->params, &buffer->buffer, buffer->width, buffer->height,
						     buffer->format, 0);
	poor_zwp_linux_buffer_params_v1_destroy(&swapchain->params);
	poor_wl_buffer_add_listener(&buffer->buffer, &buffer_listener, buffer);
	buffer->allocated = true;
	buffer->busy = buffer->stale = buffer->waiting = false;
	buffer->presented = 0;
	return 0;
}

static struct poor_wl_swapchain_buffer *swapchain_free_buffer(struct poor_wl_swapchain *swapchain)
{
	struct poor_wl_swapchain_buffer *unallocated = nullptr;

	foreach_array_ref(swapchain->buffers, buffer) {
		if (buffer->allocated && !buffer->busy && !buffer->stale)
			return buffer;
		if (!buffer->allocated && !unallocated)
			unallocated = buffer;
	}
	return unallocated;
}

static bool swapchain_can_start(const struct poor_wl_swapchain *swapchain)
{
	return !swapchain->frame_pending && !swapchain->rendering && !swapchain->failed && swapchain->tranche_count &&
	       swapchain->allocator && swapchain->width && swapchain->height;
}

/*
 * With explicit synchronization the release points are checked only before a new buffer would be needed, and waited
 * for if none has signaled yet.
 */
static struct poor_wl_swapchain_buffer *swapchain_next_buffer(struct poor_wl_swapchain *swapchain)
{
	struct poor_wl_swapchain_buffer *buffer = swapchain_free_buffer(swapchain);
	int ret;

	if (swapchain->syncobj_manager && (!buffer || !buffer->allocated)) {
		swapchain_check_releases(swapchain);
		buffer = swapchain_free_buffer(swapchain);
		if (!buffer)
			swapchain_wait_releases(swapchain);
	}
	if (!buffer || swapchain->failed)
		return nullptr;
	if (!buffer->allocated && (ret = buffer_allocate(swapchain, buffer)) < 0) {
		swapchain_fail(swapchain, -ret);
		return nullptr;
	}
	return buffer;
}

/* The buffer is allocated before 'frame', which then gets it from poor_wl_swapchain_acquire() */
static void swapchain_render(struct poor_wl_swapchain *swapchain)
{
	if (!swapchain->frame_wanted || !swapchain_can_start(swapchain) || !swapchain_next_buffer(swapchain))
		return;
	swapchain->frame_wanted = false;
	swapchain->listener->frame(swapchain, swapchain->time);
}

static void swapchain_render_later(struct poor_wl_swapchain *swapchain)
{
	if (swapchain->frame_wanted && swapchain_can_start(swapchain))
		swapchain_kick(swapchain);
}

static void frame_done(void *data, struct poor_wl_callback *callback, uint32_t time)
{
	struct poor_wl_swapchain *swapchain = data;

	poor_wl_callback_destroy(callback);
	swapchain->frame_pending = false;
	swapchain->time = time;
	swapchain_render(swapchain);
}

static const struct poor_wl_callback_listener frame_listener = {
	.done = frame_done,
};

static bool tranches_equal(const struct poor_wl_swapchain *swapchain, const struct poor_wl_swapchain_tranche *a,
			   const struct poor_wl_swapchain_tranche *b)
{
	if (a->device != b->device || a->flags != b->flags)
		return false;
	foreach_array_index(arrview_first(swapchain->format_count, a->counts), i)
		if (a->counts[i] != b->counts[i] ||
		    memcmp(a->modifiers[i], b->modifiers[i], ARRAY_SIZE_BYTES(arrview_first(a->counts[i], a->modifiers[i]))))
			return false;
	return true;
}

static bool tranches_changed(const struct poor_wl_swapchain *swapchain)
{
	if (swapchain->pending_count != swapchain->tranche_count)
		return true;
	foreach_array_index(arrview_first(swapchain->pending_count, swapchain->pending), i)
		if (!tranches_equal(swapchain, &swapchain->pending[i], &swapchain->tranches[i]))
			return true;
	return false;
}

/* Version 6 has no main device, then the first device that samples the buffers is the one to allocate on */
static dev_t feedback_device(const struct poor_wl_swapchain *swapchain)
{
	if (swapchain->has_main_device)
		return swapchain->main_device;
	foreach_array_const_ref(arrview_first(swapchain->tranche_count, swapchain->tranches), tranche)
		if (tranche->flags & POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING)
			return tranche->device;
	return swapchain->tranches[0].device;
}

static void feedback_done(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *)
{
	struct poor_wl_swapchain *swapchain = data;
	bool changed;
	dev_t device;

	if (!swapchain->pending_count) {
		swapchain->tranche_count = 0;
		swapchain_fail(swapchain, ENOTSUP);
		return;
	}
	changed = tranches_changed(swapchain);
	copy_array(swapchain->tranches, arrview_first(swapchain->pending_count, swapchain->pending));
	swapchain->tranche_count = swapchain->pending_count;
	swapchain->pending_count = 0;
	device = feedback_device(swapchain);
	if (changed)
		swapchain_drop_buffers(swapchain);
	if (!swapchain->has_device || device != swapchain->device) {
		swapchain->has_device = true;
		swapchain->device = device;
		swapchain->listener->device(swapchain, device);
	}
	swapchain_render(swapchain);
}

/* The table stays until the next one comes */
static void feedback_format_table(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t size)
{
	struct poor_wl_swapchain *swapchain = data;
	void *table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);

	poor_loop_close_fd_or_submit(swapchain->loop, fd, 0);
	if (table == MAP_FAILED) {
		swapchain_fail(swapchain, errno);
		return;
	}
	if (swapchain->table)
		munmap(swapchain->table, swapchain->table_size);
	swapchain->table = table;
	swapchain->table_size = size;
}

static void feedback_main_device(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, struct poor_wl_array *device)
{
	struct poor_wl_swapchain *swapchain = data;

	if (device->size != sizeof(dev_t))
		return;
	memcpy(&swapchain->main_device, device->data, sizeof(dev_t));
	swapchain->has_main_device = true;
}

static void tranche_reset(struct poor_wl_swapchain *swapchain)
{
	swapchain->tranche.device = 0;
	swapchain->tranche.flags = 0;
	fill_array(arrview_first(swapchain->format_count, swapchain->tranche.counts), 0);
}

static bool tranche_has_formats(const struct poor_wl_swapchain *swapchain, const struct poor_wl_swapchain_tranche *tranche)
{
	foreach_array_const_ref(arrview_first(swapchain->format_count, tranche->counts), count)
		if (*count)
			return true;
	return false;
}

/* Tranches without formats of the swapchain are left out, as are the ones that don't fit */
static void feedback_tranche_done(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *)
{
	struct poor_wl_swapchain *swapchain = data;

	if (tranche_has_formats(swapchain, &swapchain->tranche) && swapchain->pending_count < ARRAY_SIZE(swapchain->pending))
		swapchain->pending[swapchain->pending_count++] = swapchain->tranche;
	tranche_reset(swapchain);
}

static void feedback_tranche_target_device(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, struct poor_wl_array *device)
{
	struct poor_wl_swapchain *swapchain = data;

	if (device->size == sizeof(dev_t))
		memcpy(&swapchain->tranche.device, device->data, sizeof(dev_t));
}

static void feedback_tranche_formats(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, struct poor_wl_array *indices)
{
	struct poor_wl_swapchain *swapchain = data;
	auto table = array_ptr((const struct format_modifier *)swapchain->table,
			       swapchain->table_size / sizeof(struct format_modifier));
	struct poor_wl_swapchain_tranche *tranche = &swapchain->tranche;
	const uint16_t *index;

	poor_wl_array_for_each(index, indices) {
		if (*index >= ARRAY_SIZE(table))
			continue;
		foreach_array_index(arrview_first(swapchain->format_count, swapchain->formats), i) {
			if (swapchain->formats[i] != (*table)[*index].format)
				continue;
			if (tranche->counts[i] < ARRAY_SIZE(tranche->modifiers[i]))
				tranche->modifiers[i][tranche->counts[i]++] = (*table)[*index].modifier;
			break;
		}
	}
}

static void feedback_tranche_flags(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, uint32_t flags)
{
	((struct poor_wl_swapchain *)data)->tranche.flags = flags;
}

static const struct poor_zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
	.done = feedback_done,
	.format_table = feedback_format_table,
	.main_device = feedback_main_device,
	.tranche_done = feedback_tranche_done,
	.tranche_target_device = feedback_tranche_target_device,
	.tranche_formats = feedback_tranche_formats,
	.tranche_flags = feedback_tranche_flags,
};

POOR_WL_EXPORT void poor_wl_swapchain_init_impl(struct poor_wl_swapchain *swapchain, struct poor_loop *loop,
						struct poor_zwp_linux_dmabuf_v1 *dmabuf, struct poor_wl_surface *surface,
						size_t count, const uint32_t (*formats)[count],
						const struct poor_wl_swapchain_listener *listener)
{
	assert(poor_zwp_linux_dmabuf_v1_get_version(dmabuf) >= POOR_ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION);
	assert(count && count <= ARRAY_SIZE(swapchain->formats));
	poor_loop_group_init(&swapchain->io);
	poor_loop_op_init(&swapchain->release_op, swapchain_on_release);
	poor_loop_timer_init(&swapchain->timer, swapchain_on_timer);
	poor_loop_group_acquire(&swapchain->io);
	swapchain->loop = loop;
	swapchain->dmabuf = dmabuf;
	swapchain->syncobj_manager = nullptr;
	swapchain->drm_fd = swapchain->eventfd = -1;
	swapchain->surface = surface;
	copy_array(swapchain->formats, formats);
	swapchain->format_count = count;
	swapchain->listener = listener;
	swapchain->allocator = nullptr;
	swapchain->allocator_data = nullptr;
	swapchain->rendering = nullptr;
	swapchain->table = nullptr;
	swapchain->table_size = 0;
	swapchain->presents = 0;
	swapchain->tranche_count = swapchain->pending_count = 0;
	tranche_reset(swapchain);
	swapchain->main_device = swapchain->device = 0;
	swapchain->time = 0;
	swapchain->error = 0;
	swapchain->width = swapchain->height = 0;
	swapchain->has_main_device = swapchain->has_device = false;
	swapchain->frame_pending = swapchain->frame_wanted = swapchain->failed = false;
	foreach_array_ref(swapchain->buffers, buffer) {
		buffer->swapchain = swapchain;
		buffer->allocated = buffer->busy = buffer->stale = buffer->waiting = false;
	}
	poor_zwp_linux_dmabuf_v1_get_surface_feedback(dmabuf, &swapchain->feedback, surface);
	poor_zwp_linux_dmabuf_feedback_v1_add_listener(&swapchain->feedback, &feedback_listener, swapchain);
}

/* Release points wake the swapchain through one eventfd */
POOR_WL_EXPORT int poor_wl_swapchain_set_syncobj(struct poor_wl_swapchain *swapchain,
						 struct poor_wp_linux_drm_syncobj_manager_v1 *manager, int drm_fd)
{
	struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
	int fd;

	assert(!swapchain->syncobj_manager && !swapchain->allocator);
	if (drm_ioctl(drm_fd, DRM_IOCTL_GET_CAP, &cap) < 0)
		return -errno;
	if (!cap.value)
		return -EOPNOTSUPP;
	fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0)
		return -errno;
	swapchain->syncobj_manager = manager;
	swapchain->drm_fd = drm_fd;
	swapchain->eventfd = fd;
	poor_wp_linux_drm_syncobj_manager_v1_get_surface(manager, &swapchain->syncobj_surface, swapchain->surface);
	swapchain_read_releases(swapchain);
	return 0;
}

POOR_WL_EXPORT void poor_wl_swapchain_set_allocator(struct poor_wl_swapchain *swapchain,
						    const struct poor_wl_swapchain_allocator *allocator, void *data)
{
	swapchain_drop_buffers(swapchain);
	swapchain->allocator = allocator;
	swapchain->allocator_data = data;
	swapchain_render_later(swapchain);
}

POOR_WL_EXPORT void poor_wl_swapchain_resize(struct poor_wl_swapchain *swapchain, int32_t width, int32_t height)
{
	assert(width > 0 && height > 0);
	if (width == swapchain->width && height == swapchain->height)
		return;
	swapchain->width = width;
	swapchain->height = height;
	swapchain_drop_buffers(swapchain);
	swapchain_render_later(swapchain);
}

POOR_WL_EXPORT void poor_wl_swapchain_request_frame(struct poor_wl_swapchain *swapchain)
{
	swapchain->frame_wanted = true;
	swapchain_render_later(swapchain);
}

POOR_WL_EXPORT struct poor_wl_swapchain_buffer *poor_wl_swapchain_acquire(struct poor_wl_swapchain *swapchain)
{
	struct poor_wl_swapchain_buffer *buffer;

	if (!swapchain_can_start(swapchain))
		return nullptr;
	buffer = swapchain_next_buffer(swapchain);
	if (!buffer)
		return nullptr;
	buffer->age = buffer->presented ? swapchain->presents - buffer->presented + 1 : 0;
	swapchain->rendering = buffer;
	return buffer;
}

POOR_WL_EXPORT void poor_wl_swapchain_present_damage_impl(struct poor_wl_swapchain *swapchain,
							   struct poor_wl_swapchain_buffer *buffer, int fence, size_t count,
							   const struct poor_wl_swapchain_rect (*damage)[count])
{
	struct poor_wl_surface *surface = swapchain->surface;
	int ret = 0;

	assert(buffer == swapchain->rendering && (fence < 0 || swapchain->syncobj_manager));
	swapchain->rendering = nullptr;
	if (swapchain->syncobj_manager)
		ret = buffer_set_points(swapchain, buffer, fence);
	if (fence >= 0)
		poor_loop_close_fd_or_submit(swapchain->loop, fence, 0);
	if (ret < 0) {
		swapchain_fail(swapchain, -ret);
		return;
	}
	poor_wl_surface_attach(surface, &buffer->buffer, 0, 0);
	if (poor_wl_surface_get_version(surface) >= POOR_WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
		foreach_array_const_ref(damage, rect)
			poor_wl_surface_damage_buffer(surface, rect->x, rect->y, rect->width, rect->height);
	} else {
		poor_wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
	}
	poor_wl_surface_frame(surface, &swapchain->frame);
	poor_wl_callback_add_listener(&swapchain->frame, &frame_listener, swapchain);
	poor_wl_surface_commit(surface);
	buffer->presented = ++swapchain->presents;
	buffer->busy = true;
	swapchain->frame_pending = true;
}

POOR_WL_EXPORT void poor_wl_swapchain_present(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer,
					      int fence)
{
	static const struct poor_wl_swapchain_rect whole[] = { { 0, 0, INT32_MAX, INT32_MAX } };

	poor_wl_swapchain_present_damage(swapchain, buffer, fence, whole);
}

/*
 * A destroyed swapchain neither renders nor reports errors anymore, e.g. when it is destroyed from a callback. The
 * timer keeps 'done' out of this call.
 */
POOR_WL_EXPORT void poor_wl_swapchain_destroy(struct poor_wl_swapchain *swapchain)
{
	swapchain->failed = true;
	swapchain->error = 0;
	foreach_array_ref(swapchain->buffers, buffer)
		if (buffer->allocated)
			buffer_free(buffer);
	if (swapchain->frame_pending)
		poor_wl_callback_destroy(&swapchain->frame);
	if (swapchain->syncobj_manager)
		poor_wp_linux_drm_syncobj_surface_v1_destroy(&swapchain->syncobj_surface);
	poor_zwp_linux_dmabuf_feedback_v1_destroy(&swapchain->feedback);
	if (swapchain->table)
		munmap(swapchain->table, swapchain->table_size);
	cancel_op(swapchain->loop, &swapchain->release_op);
	swapchain_kick(swapchain);
	if (poor_loop_group_release(&swapchain->io))
		poor_loop_group_call_done(poor_wl_swapchain_io, swapchain->loop, &swapchain->io);
}
