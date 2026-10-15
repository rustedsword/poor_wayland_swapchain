/* SPDX-License-Identifier: MIT */
#ifndef EXAMPLE_LOG_H
#define EXAMPLE_LOG_H

#include <poor_loop.h>
#include <stdarg.h>

/* Log lines go to stderr through the loop, prefixed with the name of the example */
void log_init(struct poor_loop *loop, const char *name);
[[gnu::format(printf, 1, 0)]] void log_vprintf(const char *fmt, va_list ap);
[[gnu::format(printf, 1, 2)]] void log_printf(const char *fmt, ...);
bool log_pending(void);
/* Without a working ring there is no asynchronous way left to report anything */
struct io_uring_sqe *need_sqe(struct io_uring_sqe *sqe);

#endif
