/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <dlfcn.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <link.h>
#include <poor_loop_fd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysmacros.h>

#include "poor_wayland_swapchain.h"
#include "test-ring.h"
#include "test-runner.h"

constexpr uint64_t TIMEOUT_NS = 5'000'000'000;

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

static bool gbm_ready_called, gbm_done_called;

static void gbm_ready(struct poor_wl_gbm *)
{
	gbm_ready_called = true;
}

static void gbm_done(struct poor_wl_gbm *)
{
	gbm_done_called = true;
}

static const char *loaded[256];
static unsigned loaded_count;

static int collect_loaded(struct dl_phdr_info *info, size_t, void *)
{
	if (info->dlpi_name[0] && loaded_count < ARRAY_SIZE(loaded))
		loaded[loaded_count++] = info->dlpi_name;
	return 0;
}

/* Mesa's driver is unloaded with the last GBM device, and what its globals point to would look leaked */
static void keep_loaded(void)
{
	loaded_count = 0;
	dl_iterate_phdr(collect_loaded, nullptr);
	for (unsigned i = 0; i < loaded_count; i++)
		dlopen(loaded[i], RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
}

/* Without a GPU, as in CI, the devices are not there */
TEST(gbm_device)
{
	const dev_t devices[] = { makedev(226, 0), makedev(226, 128) };
	static const uint64_t implicit = DRM_FORMAT_MOD_INVALID;
	struct poor_wl_swapchain_buffer buffer;
	struct poor_wl_gbm gbm;
	struct poor_loop loop;

	loop_init(&loop);
	assert(poor_wl_gbm_open(&gbm, &loop, makedev(1, 3), gbm_ready, gbm_done) == -1 && errno == ENODEV);
	for (unsigned i = 0; i < ARRAY_SIZE(devices); i++) {
		gbm_ready_called = gbm_done_called = false;
		assert(!poor_wl_gbm_open(&gbm, &loop, devices[i], gbm_ready, gbm_done));
		run_until(&loop, gbm_ready_called);
		assert(!gbm.device != !gbm.error);
		if (gbm.device) {
			keep_loaded();
			buffer.width = buffer.height = 64;
			assert(!poor_wl_gbm_allocator.allocate(&gbm, &buffer, DRM_FORMAT_XRGB8888, &implicit, 1, false));
			assert(buffer.modifier == DRM_FORMAT_MOD_INVALID && buffer.plane_count >= 1 && buffer.planes[0].stride >= 64 * 4);
			for (unsigned j = 0; j < buffer.plane_count; j++)
				poor_loop_close_fd_or_submit(&loop, buffer.planes[j].fd, 0);
			poor_wl_gbm_allocator.free(&gbm, &buffer);
		}
		poor_wl_gbm_close(&gbm);
		run_until(&loop, gbm_done_called);
	}
	loop_fini(&loop);
}
