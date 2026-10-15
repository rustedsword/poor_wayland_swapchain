/* SPDX-License-Identifier: MIT */
#ifndef TEST_LOOP_H
#define TEST_LOOP_H

#include <assert.h>
#include <drm/drm.h>
#include <fcntl.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include "test-ring.h"

constexpr uint64_t TEST_TIMEOUT_NS = 5'000'000'000;

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
	poor_loop_timer_arm(loop, &watchdog, poor_loop_now() + TEST_TIMEOUT_NS);
}

static void loop_fini(struct poor_loop *loop)
{
	poor_loop_timer_disarm(&watchdog);
	assert(!poor_loop_drain(loop, poor_loop_now() + TEST_TIMEOUT_NS));
	poor_loop_exit(loop);
}

static bool node_opened;
static int node_fd;

static void node_open_done(struct poor_loop *, struct poor_loop_op *, const struct io_uring_cqe *cqe)
{
	node_fd = cqe->res;
	node_opened = true;
}

/* The first render node, if there is a GPU with timeline syncobjs */
[[maybe_unused]] static int render_node_open(struct poor_loop *loop)
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

#endif
