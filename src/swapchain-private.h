/* SPDX-License-Identifier: MIT */
#ifndef SWAPCHAIN_PRIVATE_H
#define SWAPCHAIN_PRIVATE_H

#include <errno.h>
#include <poor_loop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A ring that fails to submit can't run the swapchain anymore */
static inline struct io_uring_sqe *need_sqe(struct io_uring_sqe *sqe)
{
	if (!sqe) {
		fprintf(stderr, "io_uring: %s\n", strerror(errno));
		abort();
	}
	return sqe;
}

static inline void cancel_op(struct poor_loop *loop, struct poor_loop_op *op)
{
	if (op->pending)
		io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(loop)), op, 0);
}

#endif
