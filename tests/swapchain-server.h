/* SPDX-License-Identifier: MIT */
#ifndef SWAPCHAIN_SERVER_H
#define SWAPCHAIN_SERVER_H

#include <poor_loop.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "poor_wayland_server_core.h"

/*
 * A compositor for the swapchain tests: it shows buffers of wl_surfaces, releases the previous one and fires the
 * frame callbacks on each commit, and imports any dmabuf. The feedback, 'syncobj_fd' and 'hold_releases' are set
 * before it starts. Explicit synchronization needs 'syncobj_fd', a DRM device with timeline syncobjs, 'hold_releases'
 * keeps the release points from signaling.
 */
struct swapchain_server_rect {
	int32_t x, y, width, height;
};

struct swapchain_server_tranche {
	dev_t device;
	uint32_t flags;
	unsigned count;
	uint16_t indices[16];
};

struct swapchain_server_client {
	struct poor_wl_client client;
	bool used;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

/* The server's struct poor_wl_display, which the tests can't see since the client's display has the same name */
struct swapchain_server_display {
	alignas(max_align_t) unsigned char storage[512];
};

struct swapchain_server {
	uint32_t dmabuf_version;
	dev_t main_device;
	unsigned entries;
	struct {
		uint32_t format;
		uint32_t padding;
		uint64_t modifier;
	} table[16];
	unsigned tranche_count;
	struct swapchain_server_tranche tranches[4];
	unsigned buffers_created, buffers_destroyed, commits, feedbacks, clients_done;
	int32_t width, height;
	uint32_t format, stride;
	uint64_t modifier;
	unsigned planes;
	dev_t sampling_device;
	struct swapchain_server_rect damage[8];
	unsigned damage_count;
	int syncobj_fd;
	bool hold_releases;
	unsigned acquires;
	uint64_t acquire_point, release_point;
	bool display_done;
	struct poor_loop *loop;
	struct swapchain_server_display display;
	struct poor_wl_global compositor, dmabuf, syncobj;
	struct poor_wl_list feedback_list;
	struct swapchain_server_client clients[4];
	struct {
		uint32_t syncobj;
		uint64_t point;
	} held[8];
	unsigned held_count;
};

void swapchain_server_start(struct swapchain_server *server, struct poor_loop *loop);
/* Returns the fd of the client end of a new connection */
int swapchain_server_connect(struct swapchain_server *server);
/* Send the feedback again to all feedback objects */
void swapchain_server_send_feedback(struct swapchain_server *server);
/* Signal the release points that were held */
void swapchain_server_release_held(struct swapchain_server *server);
void swapchain_server_stop(struct swapchain_server *server);

#endif
