/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <poor_loop_fd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysmacros.h>

#include "keep-loaded.h"
#include "poor_wayland_swapchain.h"
#include "test-loop.h"
#include "test-runner.h"

static bool gbm_ready_called, gbm_done_called;

static void gbm_ready(struct poor_wl_gbm *)
{
	gbm_ready_called = true;
}

static void gbm_done(struct poor_wl_gbm *)
{
	gbm_done_called = true;
}

/* Without a GPU, as in CI, the devices are not there */
TEST(gbm_device)
{
	const dev_t devices[] = { makedev(226, 0), makedev(226, 128) };
	static const uint64_t implicit[] = { DRM_FORMAT_MOD_INVALID };
	struct poor_wl_swapchain_buffer buffer;
	struct poor_wl_gbm gbm;
	struct poor_loop loop;

	loop_init(&loop);
	assert(poor_wl_gbm_open(&gbm, &loop, makedev(1, 3), gbm_ready, gbm_done) == -1 && errno == ENODEV);
	foreach_array_const_ref(devices, device) {
		gbm_ready_called = gbm_done_called = false;
		assert(!poor_wl_gbm_open(&gbm, &loop, *device, gbm_ready, gbm_done));
		run_until(&loop, gbm_ready_called);
		assert(!gbm.device != !gbm.error);
		if (gbm.device) {
			keep_loaded();
			buffer.width = buffer.height = 64;
			assert(!poor_wl_gbm_allocator.allocate(&gbm, &buffer, DRM_FORMAT_XRGB8888, ARRAY_SIZE(implicit), &implicit, false));
			assert(buffer.modifier == DRM_FORMAT_MOD_INVALID && buffer.plane_count >= 1 && buffer.planes[0].stride >= 64 * 4);
			foreach_array_const_ref(arrview_first(buffer.plane_count, buffer.planes), plane)
				poor_loop_close_fd_or_submit(&loop, plane->fd, 0);
			poor_wl_gbm_allocator.free(&gbm, &buffer);
		}
		poor_wl_gbm_close(&gbm);
		run_until(&loop, gbm_done_called);
	}
	loop_fini(&loop);
}
