/* SPDX-License-Identifier: MIT */
#ifndef POOR_WAYLAND_SWAPCHAIN_H
#define POOR_WAYLAND_SWAPCHAIN_H

#include <linux/stat.h>
#include <poor_array.h>
#include <poor_loop_group.h>
#include <stdint.h>
#include <sys/types.h>

#include "poor_linux_dmabuf_v1_client_protocol.h"
#include "poor_linux_drm_syncobj_v1_client_protocol.h"
#include "poor_wayland_client.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * libpoor_wayland_swapchain presents dmabufs on a surface. It follows the dmabuf feedback of the surface, gets
 * buffers from an allocator in the formats and modifiers that the compositor prefers, and tells when to render
 * the next frame. Rendering is up to the caller, e.g. with EGL on the GBM device that poor_wl_gbm opens for the
 * device of the feedback. Renderers without implicit synchronization, like Vulkan, need explicit synchronization.
 * Callbacks come from the loop, never from inside a call of the library, except the ones of allocators.
 */

constexpr unsigned POOR_WL_SWAPCHAIN_BUFFERS = 3;
constexpr unsigned POOR_WL_SWAPCHAIN_FORMATS = 4;
constexpr unsigned POOR_WL_SWAPCHAIN_TRANCHES = 4;
constexpr unsigned POOR_WL_SWAPCHAIN_MODIFIERS = 64;
constexpr unsigned POOR_WL_SWAPCHAIN_PLANES = 4;

struct poor_wl_swapchain;
struct poor_wl_swapchain_allocator;
struct gbm_device;

struct poor_wl_swapchain_plane {
	int fd;
	uint32_t offset;
	uint32_t stride;
};

/* A rectangle in buffer coordinates */
struct poor_wl_swapchain_rect {
	int32_t x, y, width, height;
};

/*
 * The allocator fills 'data', 'modifier', 'planes' and 'plane_count'. The swapchain takes the fds of the planes and
 * sets 'format', the one of its formats that the buffer was allocated in. poor_wl_swapchain_acquire() sets 'age': the
 * buffer holds the frame presented 'age' frames ago, 1 being the last one, or undefined contents if it is 0.
 */
struct poor_wl_swapchain_buffer {
	struct poor_wl_buffer buffer;
	struct poor_wp_linux_drm_syncobj_timeline_v1 timeline;
	struct poor_wl_swapchain *swapchain;
	const struct poor_wl_swapchain_allocator *allocator;
	void *allocator_data;
	void *data;
	uint64_t modifier;
	uint64_t point;
	uint64_t age, presented;
	struct poor_wl_swapchain_plane planes[POOR_WL_SWAPCHAIN_PLANES];
	unsigned plane_count;
	uint32_t format, syncobj;
	int32_t width, height;
	bool allocated, busy, stale, waiting;
};

struct poor_wl_swapchain_allocator {
	/*
	 * Allocate 'buffer' with its size, 'format' and one of 'modifiers', or with an implicit modifier if they
	 * contain DRM_FORMAT_MOD_INVALID. 'scanout' asks for a buffer that the display can show. Returns 0 or a
	 * negative errno value.
	 */
	int (*allocate)(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, unsigned count,
			const uint64_t (*modifiers)[count], bool scanout);
	void (*free)(void *data, struct poor_wl_swapchain_buffer *buffer);
};

/* The modifiers of each format of the swapchain in a tranche of the feedback */
struct poor_wl_swapchain_tranche {
	dev_t device;
	uint32_t flags;
	unsigned counts[POOR_WL_SWAPCHAIN_FORMATS];
	uint64_t modifiers[POOR_WL_SWAPCHAIN_FORMATS][POOR_WL_SWAPCHAIN_MODIFIERS];
};

/* The buffers have to come from 'device', which is set again if it changes */
typedef void poor_wl_swapchain_device_fn(struct poor_wl_swapchain *swapchain, dev_t device);
/*
 * Draw a frame if there is something new to show: take its buffer with poor_wl_swapchain_acquire() and present it.
 * 'time' is the one of the frame callback, in milliseconds.
 */
typedef void poor_wl_swapchain_frame_fn(struct poor_wl_swapchain *swapchain, uint32_t time);
/* The swapchain can't go on, e.g. the compositor takes no buffers of the format */
typedef void poor_wl_swapchain_error_fn(struct poor_wl_swapchain *swapchain, int error);
typedef void poor_wl_swapchain_done_fn(struct poor_wl_swapchain *swapchain);

poor_loop_group_declare(poor_wl_swapchain_io);

struct poor_wl_swapchain {
	poor_wl_swapchain_io io;
	struct poor_loop_op release_op;
	struct poor_loop_timer timer;
	struct poor_loop *loop;
	struct poor_zwp_linux_dmabuf_v1 *dmabuf;
	struct poor_wp_linux_drm_syncobj_manager_v1 *syncobj_manager;
	struct poor_wl_surface *surface;
	struct poor_zwp_linux_dmabuf_feedback_v1 feedback;
	struct poor_zwp_linux_buffer_params_v1 params;
	struct poor_wp_linux_drm_syncobj_surface_v1 syncobj_surface;
	struct poor_wl_callback frame;
	struct poor_wl_swapchain_buffer buffers[POOR_WL_SWAPCHAIN_BUFFERS];
	struct poor_wl_swapchain_buffer *rendering;
	struct poor_wl_swapchain_tranche tranches[POOR_WL_SWAPCHAIN_TRANCHES];
	struct poor_wl_swapchain_tranche pending[POOR_WL_SWAPCHAIN_TRANCHES];
	struct poor_wl_swapchain_tranche tranche;
	unsigned tranche_count, pending_count, format_count;
	const struct poor_wl_swapchain_allocator *allocator;
	void *allocator_data;
	poor_wl_swapchain_device_fn *device_fn;
	poor_wl_swapchain_frame_fn *frame_fn;
	poor_wl_swapchain_error_fn *error_fn;
	poor_wl_swapchain_done_fn *done_fn;
	void *table;
	size_t table_size;
	uint64_t released, presents;
	dev_t main_device, device;
	uint32_t formats[POOR_WL_SWAPCHAIN_FORMATS];
	uint32_t time;
	int drm_fd, eventfd, error;
	int32_t width, height;
	bool has_main_device, has_device, frame_pending, frame_wanted, failed;
};

/*
 * Present buffers on 'surface' in the first of 'formats' that the compositor takes and the allocator manages.
 * 'formats' is an array of up to POOR_WL_SWAPCHAIN_FORMATS DRM formats in order of preference, or a pointer to one.
 * 'dmabuf' has at least version 4. 'device' tells where to allocate, buffers come once
 * poor_wl_swapchain_set_allocator() was called.
 */
void
poor_wl_swapchain_init_impl(struct poor_wl_swapchain *swapchain, struct poor_loop *loop,
			    struct poor_zwp_linux_dmabuf_v1 *dmabuf, struct poor_wl_surface *surface, size_t count,
			    const uint32_t (*formats)[count], poor_wl_swapchain_device_fn *device,
			    poor_wl_swapchain_frame_fn *frame, poor_wl_swapchain_error_fn *error);
#define poor_wl_swapchain_init(swapchain, loop, dmabuf, surface, formats, device, frame, error) \
	poor_wl_swapchain_init_impl(swapchain, loop, dmabuf, surface, ARRAY_SIZE(formats), &auto_arr(formats), device, \
				    frame, error)

/*
 * Synchronize explicitly through 'manager': the compositor waits for the fences of poor_wl_swapchain_present(), and
 * a buffer comes back once the compositor signals that it is done with it. 'drm_fd' is a DRM device with timeline
 * syncobjs, e.g. the render node of the allocator, that stays open until poor_wl_swapchain_destroy(). Call it before
 * poor_wl_swapchain_set_allocator(). Returns 0 or a negative errno value.
 */
int
poor_wl_swapchain_set_syncobj(struct poor_wl_swapchain *swapchain, struct poor_wp_linux_drm_syncobj_manager_v1 *manager,
			      int drm_fd);

/* The buffers of the previous allocator are freed as soon as the compositor releases them */
void
poor_wl_swapchain_set_allocator(struct poor_wl_swapchain *swapchain, const struct poor_wl_swapchain_allocator *allocator,
				void *data);

void
poor_wl_swapchain_resize(struct poor_wl_swapchain *swapchain, int32_t width, int32_t height);

/*
 * Ask for a call of 'frame' once a frame can start: after the frame callback or a release of a buffer, or on the next
 * run of the loop if one already can.
 */
void
poor_wl_swapchain_request_frame(struct poor_wl_swapchain *swapchain);

/*
 * The buffer of a new frame to draw into and present, or NULL if none can start yet, e.g. before the frame callback of
 * the last one. 'frame' always gets one, unless it changed the swapchain before.
 */
struct poor_wl_swapchain_buffer *
poor_wl_swapchain_acquire(struct poor_wl_swapchain *swapchain);

/*
 * Attach, damage and commit the buffer of poor_wl_swapchain_acquire(), with a frame callback. 'fence' is a sync_file
 * that signals once the buffer is rendered, or -1 if it already is, and the swapchain closes it. Fences need explicit
 * synchronization. Other state of the commit, like a viewport or presentation feedback, goes in before this call.
 */
void
poor_wl_swapchain_present(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer, int fence);

/*
 * Like poor_wl_swapchain_present(), with 'damage', an array of struct poor_wl_swapchain_rect or a pointer to one, as
 * the parts that changed since the last frame. Surfaces older than version 4 get their whole area damaged.
 */
void
poor_wl_swapchain_present_damage_impl(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer,
				      int fence, size_t count, const struct poor_wl_swapchain_rect (*damage)[count]);
#define poor_wl_swapchain_present_damage(swapchain, buffer, fence, damage) \
	poor_wl_swapchain_present_damage_impl(swapchain, buffer, fence, ARRAY_SIZE(damage), &auto_arr(damage))

/* Free all buffers, before the device of the allocator goes away. 'done' is called once the swapchain's memory can go. */
void
poor_wl_swapchain_destroy(struct poor_wl_swapchain *swapchain, poor_wl_swapchain_done_fn *done);

poor_loop_group_declare(poor_wl_gbm_io);

struct poor_wl_gbm;

typedef void poor_wl_gbm_ready_fn(struct poor_wl_gbm *gbm);
typedef void poor_wl_gbm_done_fn(struct poor_wl_gbm *gbm);

/* A GBM device on the render node of a DRM device, opened through the loop */
struct poor_wl_gbm {
	poor_wl_gbm_io io;
	struct poor_loop *loop;
	struct poor_loop_op stat_op, open_op;
	struct poor_loop_timer timer;
	struct statx stx;
	struct gbm_device *device;
	poor_wl_gbm_ready_fn *ready;
	poor_wl_gbm_done_fn *done;
	uint64_t ino;
	uint32_t ino_dev_major, ino_dev_minor;
	unsigned minor;
	int fd, error;
	bool closing;
	char path[64];
};

/*
 * Open the render node of 'device' and create a GBM device on it. 'ready' is called with 'device' set, or with
 * 'error' set to an errno value. Returns 0, or -1 with errno set to ENODEV if 'device' is no DRM device.
 */
int
poor_wl_gbm_open(struct poor_wl_gbm *gbm, struct poor_loop *loop, dev_t device, poor_wl_gbm_ready_fn *ready,
		 poor_wl_gbm_done_fn *done);

/* Destroy the GBM device after the buffers on it, 'done' is called once the render node is closed */
void
poor_wl_gbm_close(struct poor_wl_gbm *gbm);

/* Allocates GBM buffer objects for rendering, 'data' is the struct poor_wl_gbm and buffers' 'data' a struct gbm_bo */
extern const struct poor_wl_swapchain_allocator poor_wl_gbm_allocator;

#ifdef  __cplusplus
}
#endif

#endif
