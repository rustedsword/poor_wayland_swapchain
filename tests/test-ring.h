/* SPDX-License-Identifier: MIT */
#ifndef TEST_RING_H
#define TEST_RING_H

#include <liburing.h>
#include <stdlib.h>
#include <string.h>

/* The ring setup comes from TEST_RING: default, coop, defer or sqpoll */
static inline void test_ring_params(struct io_uring_params *params)
{
	const char *mode = getenv("TEST_RING");

	if (!mode || !strcmp(mode, "default"))
		return;
	if (!strcmp(mode, "coop"))
		params->flags |= IORING_SETUP_COOP_TASKRUN;
	else if (!strcmp(mode, "defer"))
		params->flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
	else if (!strcmp(mode, "sqpoll"))
		params->flags |= IORING_SETUP_SQPOLL;
	else
		abort();
}

#endif
