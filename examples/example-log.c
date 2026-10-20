/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "example-log.h"
#include "poor_wayland_client.h"

/* The log handler of poor_wayland takes no data */
static struct example_log *wayland_log;

struct io_uring_sqe *need_sqe(struct io_uring_sqe *sqe)
{
	if (!sqe) {
		dprintf(STDERR_FILENO, "%s: io_uring: %s\n", program_invocation_short_name, strerror(errno));
		exit(1);
	}
	return sqe;
}

static void log_write(struct example_log *log)
{
	unsigned busy = log->fill ^ 1;
	auto unsent = arrview_cfront(log->written, arrview_first(log->len[busy], log->data[busy]));

	io_uring_prep_write_array(need_sqe(poor_loop_get_sqe_or_submit(log->loop, &log->op)), STDERR_FILENO, unsent, -1);
}

static void log_flush(struct example_log *log)
{
	if (log->op.pending || !log->len[log->fill])
		return;
	log->fill ^= 1;
	log->written = 0;
	log_write(log);
}

static void on_log_written(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct example_log *log = container_of(op, struct example_log, op);
	unsigned busy = log->fill ^ 1;

	if (cqe->res > 0 && (log->written += cqe->res) < log->len[busy]) {
		log_write(log);
		return;
	}
	log->len[busy] = 0;
	log_flush(log);
}

/* A line that doesn't fit the buffer is dropped */
void log_vprintf(struct example_log *log, const char *fmt, va_list ap)
{
	auto room = arrview_cfront(log->len[log->fill], log->data[log->fill]);
	int n = vsnprintf_array(room, fmt, ap);

	if (n > 0 && (size_t)n < ARRAY_SIZE(room))
		log->len[log->fill] += n;
	log_flush(log);
}

void log_printf(struct example_log *log, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_vprintf(log, fmt, ap);
	va_end(ap);
}

bool log_pending(const struct example_log *log)
{
	return log->op.pending || log->len[log->fill];
}

[[gnu::format(printf, 1, 0)]] static void log_wayland(const char *fmt, va_list ap)
{
	log_vprintf(wayland_log, fmt, ap);
}

void log_init(struct example_log *log, struct poor_loop *loop)
{
	log->loop = loop;
	poor_loop_op_init(&log->op, on_log_written);
	fill_array(log->len, 0);
	log->fill = 0;
	wayland_log = log;
	poor_wl_log_set_handler_client(log_wayland);
}

void log_fini(struct example_log *log)
{
	assert(wayland_log == log);
	poor_wl_log_set_handler_client(nullptr);
	wayland_log = nullptr;
}
