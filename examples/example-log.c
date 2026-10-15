/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "example-log.h"

/* One buffer fills while the other one is written */
static struct {
	struct poor_loop *loop;
	struct poor_loop_op op;
	const char *name;
	size_t len[2], written;
	unsigned fill;
	char data[2][16384];
} logger;

struct io_uring_sqe *need_sqe(struct io_uring_sqe *sqe)
{
	if (!sqe) {
		dprintf(STDERR_FILENO, "%s: io_uring: %s\n", logger.name, strerror(errno));
		exit(1);
	}
	return sqe;
}

static void log_write(void)
{
	unsigned busy = logger.fill ^ 1;

	io_uring_prep_write(need_sqe(poor_loop_get_sqe_or_submit(logger.loop, &logger.op)), STDERR_FILENO,
			    logger.data[busy] + logger.written, logger.len[busy] - logger.written, -1);
}

static void log_flush(void)
{
	if (logger.op.pending || !logger.len[logger.fill])
		return;
	logger.fill ^= 1;
	logger.written = 0;
	log_write();
}

static void on_log_written(struct poor_loop *, struct poor_loop_op *, const struct io_uring_cqe *cqe)
{
	unsigned busy = logger.fill ^ 1;

	if (cqe->res > 0 && (logger.written += cqe->res) < logger.len[busy]) {
		log_write();
		return;
	}
	logger.len[busy] = 0;
	log_flush();
}

/* A line that doesn't fit the buffer is dropped */
void log_vprintf(const char *fmt, va_list ap)
{
	size_t room = sizeof(logger.data[0]) - logger.len[logger.fill];
	int n = vsnprintf(logger.data[logger.fill] + logger.len[logger.fill], room, fmt, ap);

	if (n > 0 && (size_t)n < room)
		logger.len[logger.fill] += n;
	log_flush();
}

void log_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_vprintf(fmt, ap);
	va_end(ap);
}

bool log_pending(void)
{
	return logger.op.pending || logger.len[logger.fill];
}

void log_init(struct poor_loop *loop, const char *name)
{
	logger.loop = loop;
	logger.name = name;
	poor_loop_op_init(&logger.op, on_log_written);
	logger.len[0] = logger.len[1] = 0;
	logger.fill = 0;
}
