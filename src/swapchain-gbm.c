/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <poor_loop_fd.h>
#include <poor_stdio.h>
#include <sys/sysmacros.h>

#include "poor_wayland_swapchain.h"
#include "swapchain-private.h"

constexpr unsigned DRM_MAJOR = 226;
constexpr unsigned RENDER_MINOR_FIRST = 128;
constexpr unsigned RENDER_MINOR_END = 192;

poor_loop_group_attach(poor_wl_gbm_io, struct poor_wl_gbm, io, stat_op, gbm_on_stat);
poor_loop_group_attach(poor_wl_gbm_io, struct poor_wl_gbm, io, open_op, gbm_on_open);
poor_loop_group_attach(poor_wl_gbm_io, struct poor_wl_gbm, io, timer, gbm_on_timer);

static struct poor_wl_gbm *gbm_of(poor_wl_gbm_io *io)
{
	return container_of(io, struct poor_wl_gbm, io);
}

static struct io_uring_sqe *gbm_sqe(struct poor_wl_gbm *gbm, struct poor_loop_op *op)
{
	return need_sqe(poor_loop_group_get_sqe_or_submit(gbm->loop, &gbm->io, op));
}

/* The device link of a node leads to the device in sysfs, which all nodes of a DRM device share */
static void gbm_stat(struct poor_wl_gbm *gbm)
{
	snprintf_array(gbm->path, "/sys/dev/char/%u:%u/device", DRM_MAJOR, gbm->minor);
	io_uring_prep_statx(gbm_sqe(gbm, &gbm->stat_op), AT_FDCWD, gbm->path, 0, STATX_INO, &gbm->stx);
}

static void gbm_open_node(struct poor_wl_gbm *gbm)
{
	snprintf_array(gbm->path, "/dev/dri/renderD%u", gbm->minor);
	io_uring_prep_openat(gbm_sqe(gbm, &gbm->open_op), AT_FDCWD, gbm->path, O_RDWR | O_CLOEXEC, 0);
}

static void gbm_fail(struct poor_wl_gbm *gbm, int error)
{
	gbm->error = error;
	gbm->ready(gbm);
}

/* Below the render nodes, the stat is the one of the device of the feedback, then render nodes are tried in turn */
POOR_LOOP_GROUP_OP(gbm_on_stat, loop, io, op, cqe)
{
	struct poor_wl_gbm *gbm = gbm_of(io);

	if (gbm->closing)
		return;
	if (gbm->minor < RENDER_MINOR_FIRST) {
		if (cqe->res < 0) {
			gbm_fail(gbm, -cqe->res);
			return;
		}
		gbm->ino = gbm->stx.stx_ino;
		gbm->ino_dev_major = gbm->stx.stx_dev_major;
		gbm->ino_dev_minor = gbm->stx.stx_dev_minor;
		gbm->minor = RENDER_MINOR_FIRST;
	} else if (!cqe->res && gbm->stx.stx_ino == gbm->ino && gbm->stx.stx_dev_major == gbm->ino_dev_major &&
		   gbm->stx.stx_dev_minor == gbm->ino_dev_minor) {
		gbm_open_node(gbm);
		return;
	} else if (++gbm->minor == RENDER_MINOR_END) {
		gbm_fail(gbm, ENOENT);
		return;
	}
	gbm_stat(gbm);
}

POOR_LOOP_GROUP_OP(gbm_on_open, loop, io, op, cqe)
{
	struct poor_wl_gbm *gbm = gbm_of(io);

	if (cqe->res >= 0)
		gbm->fd = cqe->res;
	if (gbm->closing)
		return;
	if (cqe->res < 0) {
		gbm_fail(gbm, -cqe->res);
		return;
	}
	gbm->device = gbm_create_device(gbm->fd);
	if (!gbm->device) {
		gbm_fail(gbm, ENODEV);
		return;
	}
	gbm->ready(gbm);
}

POOR_LOOP_GROUP_TIMER(gbm_on_timer, loop, io, timer)
{
}

POOR_LOOP_GROUP_DONE(poor_wl_gbm_io, loop, io)
{
	struct poor_wl_gbm *gbm = gbm_of(io);

	if (gbm->fd >= 0)
		poor_loop_close_fd_or_submit(loop, gbm->fd, IOSQE_ASYNC);
	gbm->done(gbm);
}

/* A render node is opened right away */
POOR_WL_EXPORT int poor_wl_gbm_open(struct poor_wl_gbm *gbm, struct poor_loop *loop, dev_t device, poor_wl_gbm_ready_fn *ready,
				    poor_wl_gbm_done_fn *done)
{
	if (major(device) != DRM_MAJOR) {
		errno = ENODEV;
		return -1;
	}
	poor_loop_group_init(&gbm->io);
	poor_loop_op_init(&gbm->stat_op, gbm_on_stat);
	poor_loop_op_init(&gbm->open_op, gbm_on_open);
	poor_loop_timer_init(&gbm->timer, gbm_on_timer);
	gbm->loop = loop;
	gbm->ready = ready;
	gbm->done = done;
	gbm->device = nullptr;
	gbm->minor = minor(device);
	gbm->fd = -1;
	gbm->error = 0;
	gbm->closing = false;
	poor_loop_group_acquire(&gbm->io);
	if (gbm->minor >= RENDER_MINOR_FIRST)
		gbm_open_node(gbm);
	else
		gbm_stat(gbm);
	return 0;
}

/* The timer keeps 'done' out of this call */
POOR_WL_EXPORT void poor_wl_gbm_close(struct poor_wl_gbm *gbm)
{
	assert(!gbm->closing);
	gbm->closing = true;
	cancel_op(gbm->loop, &gbm->stat_op);
	cancel_op(gbm->loop, &gbm->open_op);
	if (gbm->device)
		gbm_device_destroy(gbm->device);
	gbm->device = nullptr;
	poor_loop_group_timer_arm(gbm->loop, &gbm->io, &gbm->timer, 0);
	if (poor_loop_group_release(&gbm->io))
		poor_loop_group_call_done(poor_wl_gbm_io, gbm->loop, &gbm->io);
}

/* Explicit modifiers are tried first, an implicit one if the compositor takes it */
static int gbm_allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, unsigned count,
			const uint64_t (*modifiers)[count], bool scanout)
{
	struct poor_wl_gbm *gbm = data;
	uint32_t flags = GBM_BO_USE_RENDERING | (scanout ? GBM_BO_USE_SCANOUT : 0);
	uint64_t explicit[POOR_WL_SWAPCHAIN_MODIFIERS];
	struct gbm_bo *bo = nullptr;
	bool implicit = false;
	unsigned n = 0;
	int planes, ret;

	assert(count <= ARRAY_SIZE(explicit));
	foreach_array_const_ref(modifiers, modifier) {
		if (*modifier == DRM_FORMAT_MOD_INVALID)
			implicit = true;
		else
			explicit[n++] = *modifier;
	}
	if (n)
		bo = gbm_bo_create_with_modifiers2(gbm->device, buffer->width, buffer->height, format, explicit, n, flags);
	if (bo)
		buffer->modifier = gbm_bo_get_modifier(bo);
	else if (implicit && (bo = gbm_bo_create(gbm->device, buffer->width, buffer->height, format, flags)))
		buffer->modifier = DRM_FORMAT_MOD_INVALID;
	if (!bo)
		return -ENOTSUP;
	planes = gbm_bo_get_plane_count(bo);
	if (planes <= 0 || planes > (int)ARRAY_SIZE(buffer->planes)) {
		gbm_bo_destroy(bo);
		return -ENOTSUP;
	}
	foreach_array_index(arrview_first(planes, buffer->planes), i) {
		buffer->planes[i].fd = gbm_bo_get_fd_for_plane(bo, i);
		if (buffer->planes[i].fd < 0) {
			ret = -errno;
			foreach_array_const_ref(arrview_first(i, buffer->planes), plane)
				poor_loop_close_fd_or_submit(gbm->loop, plane->fd, IOSQE_ASYNC);
			gbm_bo_destroy(bo);
			return ret;
		}
		buffer->planes[i].offset = gbm_bo_get_offset(bo, i);
		buffer->planes[i].stride = gbm_bo_get_stride_for_plane(bo, i);
	}
	buffer->plane_count = planes;
	buffer->data = bo;
	return 0;
}

static void gbm_free(void *, struct poor_wl_swapchain_buffer *buffer)
{
	gbm_bo_destroy(buffer->data);
}

POOR_WL_EXPORT const struct poor_wl_swapchain_allocator poor_wl_gbm_allocator = {
	.allocate = gbm_allocate,
	.free = gbm_free,
};
