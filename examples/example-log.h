/* SPDX-License-Identifier: MIT */
#ifndef EXAMPLE_LOG_H
#define EXAMPLE_LOG_H

#include <poor_loop.h>
#include <stdarg.h>

/* Log lines go to stderr through the loop: one buffer fills while the other one is written */
struct example_log {
	struct poor_loop *loop;
	struct poor_loop_op op;
	size_t len[2], written;
	unsigned fill;
	char data[2][16384];
};

/* The lines of poor_wayland go to 'log' too, until log_fini() */
void log_init(struct example_log *log, struct poor_loop *loop);
void log_fini(struct example_log *log);
[[gnu::format(printf, 2, 0)]] void log_vprintf(struct example_log *log, const char *fmt, va_list ap);
[[gnu::format(printf, 2, 3)]] void log_printf(struct example_log *log, const char *fmt, ...);
bool log_pending(const struct example_log *log);
/* Without a working ring there is no asynchronous way left to report anything */
struct io_uring_sqe *need_sqe(struct io_uring_sqe *sqe);

#endif
