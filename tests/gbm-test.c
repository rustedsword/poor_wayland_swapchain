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

struct gbm_test {
	struct poor_wl_gbm gbm;
	bool ready, done;
};

static void gbm_ready(struct poor_wl_gbm *gbm)
{
	container_of(gbm, struct gbm_test, gbm)->ready = true;
}

static void gbm_done(struct poor_wl_gbm *gbm)
{
	container_of(gbm, struct gbm_test, gbm)->done = true;
}

/* Without a GPU, as in CI, the devices are not there */
TEST(gbm_device)
{
	const dev_t devices[] = { makedev(226, 0), makedev(226, 128) };
	static const uint64_t implicit[] = { DRM_FORMAT_MOD_INVALID };
	struct poor_wl_swapchain_buffer buffer;
	struct poor_loop_timer watchdog;
	struct poor_loop loop;
	struct gbm_test t;

	loop_init(&loop, &watchdog);
	assert(poor_wl_gbm_open(&t.gbm, &loop, makedev(1, 3), gbm_ready, gbm_done) == -1 && errno == ENODEV);
	foreach_array_const_ref(devices, device) {
		t.ready = t.done = false;
		assert(!poor_wl_gbm_open(&t.gbm, &loop, *device, gbm_ready, gbm_done));
		run_until(&loop, t.ready);
		assert(!t.gbm.device != !t.gbm.error);
		if (t.gbm.device) {
			keep_loaded();
			buffer.width = buffer.height = 64;
			assert(!poor_wl_gbm_allocator.allocate(&t.gbm, &buffer, DRM_FORMAT_XRGB8888, ARRAY_SIZE(implicit), &implicit, false));
			assert(buffer.modifier == DRM_FORMAT_MOD_INVALID && buffer.plane_count >= 1 && buffer.planes[0].stride >= 64 * 4);
			foreach_array_const_ref(arrview_first(buffer.plane_count, buffer.planes), plane)
				poor_loop_close_fd_or_submit(&loop, plane->fd, 0);
			poor_wl_gbm_allocator.free(&t.gbm, &buffer);
		}
		poor_wl_gbm_close(&t.gbm);
		run_until(&loop, t.done);
	}
	loop_fini(&loop, &watchdog);
}
