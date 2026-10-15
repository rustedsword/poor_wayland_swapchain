/* SPDX-License-Identifier: MIT */
#ifndef SWAPCHAIN_SERVER_H
#define SWAPCHAIN_SERVER_H

#include <poor_loop.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * A compositor for the swapchain tests: it shows buffers of wl_surfaces, releases the previous one and fires the
 * frame callbacks on each commit, and imports any dmabuf. The feedback is set before it starts. Explicit
 * synchronization needs 'syncobj_fd', a DRM device with timeline syncobjs.
 */
struct swapchain_server_tranche {
	dev_t device;
	uint32_t flags;
	unsigned count;
	uint16_t indices[8];
};

struct swapchain_server_state {
	uint32_t dmabuf_version;
	dev_t main_device;
	unsigned entries;
	struct {
		uint32_t format;
		uint32_t padding;
		uint64_t modifier;
	} table[8];
	unsigned tranche_count;
	struct swapchain_server_tranche tranches[4];
	unsigned buffers_created, buffers_destroyed, commits, feedbacks, clients_done;
	int32_t width, height;
	uint32_t format, stride;
	uint64_t modifier;
	unsigned planes;
	dev_t sampling_device;
	int syncobj_fd;
	unsigned acquires;
	uint64_t acquire_point, release_point;
	bool display_done;
};

extern struct swapchain_server_state swapchain_server;

void swapchain_server_start(struct poor_loop *loop);
/* Returns the fd of the client end of a new connection */
int swapchain_server_connect(void);
/* Send the feedback again to all feedback objects */
void swapchain_server_send_feedback(void);
void swapchain_server_stop(void);

#endif
