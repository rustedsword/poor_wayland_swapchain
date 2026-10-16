/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vk_init.h>

#include "example-log.h"
#include "poor_wayland_client_display.h"
#include "poor_wayland_swapchain_vulkan.h"
#include "xdg-shell-client-protocol.h"

/*
 * A square that goes through all hues and bounces over a still background, cleared with Vulkan on the GPU that the
 * compositor names in its dmabuf feedback and presented through the swapchain, with 10 bits per channel if the
 * compositor and the GPU have them. A frame damages only where the square was and is, and a buffer redraws only what
 * changed since the frame that it holds, which its age tells. Vulkan has no implicit synchronization, so the
 * compositor needs explicit synchronization. It runs until its close button is pressed or until SIGINT or SIGTERM.
 * WAYLAND_DEBUG=client shows the protocol.
 */
constexpr int32_t DEFAULT_WIDTH = 640;
constexpr int32_t DEFAULT_HEIGHT = 480;
constexpr int32_t SQUARE = 96;
constexpr unsigned HISTORY = 4;
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

static const uint32_t formats[] = { DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB8888 };
static const VkClearColorValue background = { .float32 = { 0.1f, 0.1f, 0.12f, 1.0f } };

poor_loop_fd_declare(signal_file);

/* What changed in a frame since the frame before it */
struct damage {
	struct poor_wl_swapchain_rect rects[2];
};

/* How the image of a buffer of the swapchain is drawn */
struct target {
	VkImageView view;
	VkCommandBuffer commands;
	VkFence done;
};

struct app {
	struct poor_loop loop;
	struct poor_wl_display display;
	struct poor_wl_registry registry;
	struct poor_wl_callback sync;
	struct poor_wl_compositor compositor;
	struct poor_zwp_linux_dmabuf_v1 dmabuf;
	struct poor_wp_linux_drm_syncobj_manager_v1 syncobj;
	struct poor_xdg_wm_base wm_base;
	struct poor_wl_surface surface;
	struct poor_xdg_surface shell_surface;
	struct poor_xdg_toplevel toplevel;
	struct poor_wl_swapchain swapchain;
	struct poor_wl_vulkan vulkan;
	struct target targets[POOR_WL_SWAPCHAIN_BUFFERS];
	struct damage history[HISTORY];
	struct poor_wl_swapchain_rect square;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkCommandPool pool;
	VkSemaphore rendered, idle;
	VkFence drained;
	PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
	uint32_t family;
	struct poor_loop_op open_op, idle_op;
	int drm_fd, idle_fd;
	signal_file signals;
	struct poor_loop_op signal_op;
	struct signalfd_siginfo siginfo;
	sigset_t sigset;
	int status;
	int32_t width, height, presented_width, presented_height;
	uint32_t format;
	bool have_compositor, have_dmabuf, have_syncobj, have_wm_base;
	bool swapchain_started, swapchain_closed, closing, signals_closed;
	char path[32];
	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

poor_loop_group_attach(signal_file, struct app, signals, signal_op, on_signal);

static bool check(VkResult result, const char *what)
{
	if (result >= 0)
		return true;
	log_printf("simple-vulkan: %s: %d\n", what, result);
	return false;
}

static struct app *app_of_swapchain(struct poor_wl_swapchain *swapchain)
{
	return container_of(swapchain, struct app, swapchain);
}

static void swapchain_done(struct poor_wl_swapchain *swapchain)
{
	app_of_swapchain(swapchain)->swapchain_closed = true;
}

/* The buffers go before the device that they are on */
static void app_teardown(struct app *app)
{
	if (app->swapchain_started)
		poor_wl_swapchain_destroy(&app->swapchain, swapchain_done);
	if (app->device) {
		vkDestroyFence(app->device, app->drained, nullptr);
		vkDestroySemaphore(app->device, app->idle, nullptr);
		vkDestroySemaphore(app->device, app->rendered, nullptr);
		vkDestroyCommandPool(app->device, app->pool, nullptr);
		vkDestroyDevice(app->device, nullptr);
	}
	if (app->instance)
		vkDestroyInstance(app->instance, nullptr);
	if (app->drm_fd >= 0)
		poor_loop_close_fd_or_submit(&app->loop, app->drm_fd, 0);
	poor_wl_display_disconnect(&app->display);
	if (app->signal_op.pending)
		io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(&app->loop)), &app->signal_op, 0);
}

/* Vulkan learns from the fence of the empty batch that all drawing is done */
static void on_idle(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *)
{
	struct app *app = container_of(op, struct app, idle_op);

	poor_loop_close_fd_or_submit(&app->loop, app->idle_fd, 0);
	vkGetFenceStatus(app->device, app->drained);
	app_teardown(app);
}

/* An empty batch signals once all drawing before it is done, -1 means that it is or that the fence failed */
static int idle_fence(struct app *app)
{
	auto info = xVkSemaphoreCreateInfo(
		.pNext = &xVkExportSemaphoreCreateInfo(.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT),
	);
	int fd = -1;

	if (!check(vkCreateSemaphore(app->device, &info, nullptr, &app->idle), "vkCreateSemaphore") ||
	    !check(vkCreateFence(app->device, &xVkFenceCreateInfo(), nullptr, &app->drained), "vkCreateFence"))
		return -1;
	auto submit = xVkSubmitInfo2(
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &xVkSemaphoreSubmitInfo(
			.semaphore = app->idle,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		),
	);
	auto get = xVkSemaphoreGetFdInfoKHR(.semaphore = app->idle, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);

	if (!check(vkQueueSubmit2(app->queue, 1, &submit, app->drained), "vkQueueSubmit2"))
		return -1;
	check(app->get_semaphore_fd(app->device, &get, &fd), "vkGetSemaphoreFdKHR");
	return fd;
}

/* The drawing has to end before the buffers go, which is waited for through the loop */
static void app_quit(struct app *app, int status)
{
	if (app->closing)
		return;
	app->closing = true;
	app->status = status;
	if (!app->queue) {
		app_teardown(app);
		return;
	}
	app->idle_fd = idle_fence(app);
	if (app->idle_fd < 0) {
		vkDeviceWaitIdle(app->device);
		app_teardown(app);
		return;
	}
	io_uring_prep_poll_add(need_sqe(poor_loop_get_sqe_or_submit(&app->loop, &app->idle_op)), app->idle_fd, POLLIN);
}

static struct target *target_of(struct app *app, struct poor_wl_swapchain_buffer *buffer)
{
	return &app->targets[array_ref_index(app->swapchain.buffers, buffer)];
}

/* The last drawing is done by then, which Vulkan learns from the fence */
static void target_fini(struct app *app, struct target *target)
{
	if (target->done)
		vkGetFenceStatus(app->device, target->done);
	if (target->commands)
		vkFreeCommandBuffers(app->device, app->pool, 1, &target->commands);
	vkDestroyFence(app->device, target->done, nullptr);
	vkDestroyImageView(app->device, target->view, nullptr);
}

static bool target_init(struct app *app, struct target *target, const struct poor_wl_vulkan_image *image)
{
	auto view_info = xVkImageViewCreateInfo(
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = image->format,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	auto commands_info = xVkCommandBufferAllocateInfo(
		.commandPool = app->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	);
	auto fence_info = xVkFenceCreateInfo(.flags = VK_FENCE_CREATE_SIGNALED_BIT);

	target->view = VK_NULL_HANDLE;
	target->commands = VK_NULL_HANDLE;
	target->done = VK_NULL_HANDLE;
	return check(vkCreateImageView(app->device, &view_info, nullptr, &target->view), "vkCreateImageView") &&
	       check(vkAllocateCommandBuffers(app->device, &commands_info, &target->commands), "vkAllocateCommandBuffers") &&
	       check(vkCreateFence(app->device, &fence_info, nullptr, &target->done), "vkCreateFence");
}

/* poor_wl_vulkan allocates, the targets come and go with the buffers */
static int allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, unsigned count,
		    const uint64_t (*modifiers)[count], bool scanout)
{
	struct app *app = data;
	struct target *target = target_of(app, buffer);
	int ret = poor_wl_vulkan_allocator.allocate(&app->vulkan, buffer, format, count, modifiers, scanout);

	if (ret < 0)
		return ret;
	if (!target_init(app, target, buffer->data)) {
		target_fini(app, target);
		foreach_array_const_ref(arrview_first(buffer->plane_count, buffer->planes), plane)
			poor_loop_close_fd_or_submit(&app->loop, plane->fd, 0);
		poor_wl_vulkan_allocator.free(&app->vulkan, buffer);
		return -ENOMEM;
	}
	if (format != app->format)
		log_printf("simple-vulkan: buffers in %c%c%c%c\n", (char)format, (char)(format >> 8), (char)(format >> 16),
			   (char)(format >> 24));
	app->format = format;
	return 0;
}

static void free_buffer(void *data, struct poor_wl_swapchain_buffer *buffer)
{
	struct app *app = data;

	target_fini(app, target_of(app, buffer));
	poor_wl_vulkan_allocator.free(&app->vulkan, buffer);
}

static const struct poor_wl_swapchain_allocator allocator = {
	.allocate = allocate,
	.free = free_buffer,
};

static bool device_queue_family(VkPhysicalDevice physical, uint32_t *family)
{
	VkQueueFamilyProperties2 families[16];
	uint32_t count = ARRAY_SIZE(families);

	foreach_array_ref(families, properties)
		*properties = xVkQueueFamilyProperties2();
	vkGetPhysicalDeviceQueueFamilyProperties2(physical, &count, families);
	foreach_array_index(arrview_first(count, families), i) {
		if (families[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			*family = i;
			return true;
		}
	}
	return false;
}

static bool device_exports_fences(VkPhysicalDevice physical)
{
	auto info = xVkPhysicalDeviceExternalSemaphoreInfo(.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
	auto properties = xVkExternalSemaphoreProperties();

	vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &info, &properties);
	return properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
}

static bool vulkan_init(struct app *app, dev_t device, unsigned *render_minor)
{
	static const float priority = 1.0f;
	auto application = xVkApplicationInfo(.pApplicationName = "simple-vulkan", .apiVersion = VK_API_VERSION_1_4);
	auto properties = xVkPhysicalDeviceProperties2();

	/* Mesa's device-select layer would connect to the compositor with the system libwayland and block, to sort GPUs */
	setenv("NODEVICE_SELECT", "1", 1);
	if (!check(vkCreateInstance(&xVkInstanceCreateInfo(.pApplicationInfo = &application), nullptr, &app->instance),
		   "vkCreateInstance"))
		return false;
	app->physical = poor_wl_vulkan_find_device(vkGetInstanceProcAddr, app->instance, device, render_minor);
	if (app->physical) {
		vkGetPhysicalDeviceProperties2(app->physical, &properties);
		copy_array(app->device_name, properties.properties.deviceName);
	}
	if (!app->physical || !device_queue_family(app->physical, &app->family) || !device_exports_fences(app->physical)) {
		log_printf("simple-vulkan: no Vulkan 1.4 device with dmabufs, modifiers and fences for %u:%u\n", major(device),
			   minor(device));
		return false;
	}
	auto device_info = xVkDeviceCreateInfo(
		.pNext = &xVkPhysicalDeviceVulkan13Features(.synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE),
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &xVkDeviceQueueCreateInfo(
			.queueFamilyIndex = app->family,
			.queueCount = 1,
			.pQueuePriorities = &priority,
		),
		.enabledExtensionCount = ARRAY_SIZE(poor_wl_vulkan_device_extensions),
		.ppEnabledExtensionNames = poor_wl_vulkan_device_extensions,
	);
	auto pool_info = xVkCommandPoolCreateInfo(
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = app->family,
	);
	auto semaphore_info = xVkSemaphoreCreateInfo(
		.pNext = &xVkExportSemaphoreCreateInfo(.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT),
	);

	if (!check(vkCreateDevice(app->physical, &device_info, nullptr, &app->device), "vkCreateDevice"))
		return false;
	vkGetDeviceQueue2(app->device, &xVkDeviceQueueInfo2(.queueFamilyIndex = app->family), &app->queue);
	app->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(app->device, "vkGetSemaphoreFdKHR");
	poor_wl_vulkan_init(&app->vulkan, &app->loop, vkGetInstanceProcAddr, app->instance, app->physical, app->device,
			    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	return check(vkCreateCommandPool(app->device, &pool_info, nullptr, &app->pool), "vkCreateCommandPool") &&
	       check(vkCreateSemaphore(app->device, &semaphore_info, nullptr, &app->rendered), "vkCreateSemaphore");
}

/* The render node only makes the timelines of explicit synchronization */
static void on_open(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct app *app = container_of(op, struct app, open_op);
	int ret;

	if (app->closing) {
		if (cqe->res >= 0)
			poor_loop_close_fd_or_submit(&app->loop, cqe->res, 0);
		return;
	}
	if (cqe->res < 0) {
		log_printf("simple-vulkan: %s: %s\n", app->path, strerror(-cqe->res));
		app_quit(app, 1);
		return;
	}
	app->drm_fd = cqe->res;
	ret = poor_wl_swapchain_set_syncobj(&app->swapchain, &app->syncobj, app->drm_fd);
	if (ret < 0) {
		log_printf("simple-vulkan: no explicit synchronization: %s\n", strerror(-ret));
		app_quit(app, 1);
		return;
	}
	log_printf("simple-vulkan: %s\n", app->device_name);
	poor_wl_swapchain_set_allocator(&app->swapchain, &allocator, app);
}

static void swapchain_device(struct poor_wl_swapchain *swapchain, dev_t device)
{
	struct app *app = app_of_swapchain(swapchain);
	unsigned render_minor;

	if (app->instance) {
		log_printf("simple-vulkan: the compositor moved to another GPU\n");
		app_quit(app, 1);
		return;
	}
	if (!vulkan_init(app, device, &render_minor)) {
		app_quit(app, 1);
		return;
	}
	snprintf_array(app->path, "/dev/dri/renderD%u", render_minor);
	io_uring_prep_openat(need_sqe(poor_loop_get_sqe_or_submit(&app->loop, &app->open_op)), AT_FDCWD, app->path,
			     O_RDWR | O_CLOEXEC, 0);
}

/* All hues every six seconds */
static VkClearColorValue hue(uint32_t time)
{
	float rise = time % 1000 / 1000.0f, fall = 1.0f - rise;

	switch (time / 1000 % 6) {
	case 0:
		return (VkClearColorValue){ .float32 = { 1.0f, rise, 0.0f, 1.0f } };
	case 1:
		return (VkClearColorValue){ .float32 = { fall, 1.0f, 0.0f, 1.0f } };
	case 2:
		return (VkClearColorValue){ .float32 = { 0.0f, 1.0f, rise, 1.0f } };
	case 3:
		return (VkClearColorValue){ .float32 = { 0.0f, fall, 1.0f, 1.0f } };
	case 4:
		return (VkClearColorValue){ .float32 = { rise, 0.0f, 1.0f, 1.0f } };
	default:
		return (VkClearColorValue){ .float32 = { 1.0f, 0.0f, fall, 1.0f } };
	}
}

/* The square crosses the window back and forth at 'speed' pixels per second */
static int32_t bounce(uint32_t time, uint32_t speed, int32_t range)
{
	uint64_t at = range > 0 ? (uint64_t)time * speed / 1000 % (2 * (uint64_t)range) : 0;

	return at <= (uint64_t)range ? at : 2 * (uint64_t)range - at;
}

static struct poor_wl_swapchain_rect square_at(uint32_t time, int32_t width, int32_t height)
{
	int32_t side = width < SQUARE ? width : SQUARE;

	side = height < side ? height : side;
	return (struct poor_wl_swapchain_rect){ bounce(time, 300, width - side), bounce(time, 200, height - side), side, side };
}

/* The part of 'rect' in the image, if there is one */
static bool clear_rect(const struct poor_wl_swapchain_rect *rect, int32_t width, int32_t height, VkClearRect *clear)
{
	int64_t x0 = rect->x > 0 ? rect->x : 0, y0 = rect->y > 0 ? rect->y : 0;
	int64_t x1 = (int64_t)rect->x + rect->width, y1 = (int64_t)rect->y + rect->height;

	x1 = x1 < width ? x1 : width;
	y1 = y1 < height ? y1 : height;
	if (x0 >= x1 || y0 >= y1)
		return false;
	*clear = (VkClearRect){ .rect = { { (int32_t)x0, (int32_t)y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } },
				.layerCount = 1 };
	return true;
}

static void image_barrier(VkCommandBuffer commands, const VkImageMemoryBarrier2 *barrier)
{
	vkCmdPipelineBarrier2(commands, &xVkDependencyInfo(.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = barrier));
}

/*
 * A buffer without an age is cleared whole. One with an age keeps its contents: it comes back from the queue family of
 * the compositor, and what changed since its frame is cleared to the background before the square goes on top. The
 * image goes to the queue family of the compositor, which reads it once the fence of the batch signals. The
 * compositor was done with the buffer, and so was the last batch that drew it, which Vulkan learns from the fence of
 * that batch.
 */
static void swapchain_frame(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer, uint32_t time)
{
	struct app *app = app_of_swapchain(swapchain);
	struct target *target = target_of(app, buffer);
	VkImage image = ((struct poor_wl_vulkan_image *)buffer->data)->image;
	struct poor_wl_swapchain_rect square = square_at(time, buffer->width, buffer->height);
	bool whole = buffer->width != app->presented_width || buffer->height != app->presented_height;
	bool full = whole || !buffer->age || buffer->age > ARRAY_SIZE(app->history) + 1;
	struct damage damage = whole ? (struct damage){ { { 0, 0, INT32_MAX, INT32_MAX } } } :
				       (struct damage){ { app->square, square } };
	VkClearRect clears[(ARRAY_SIZE(app->history) + 1) * ARRAY_SIZE(damage.rects)], square_clear;
	VkClearAttachment fill = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue = { .color = background } };
	VkClearAttachment paint = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue = { .color = hue(time) } };
	uint32_t count = 0;
	auto begin = xVkCommandBufferBeginInfo(.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	auto draw = xVkImageMemoryBarrier2(
		.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
		.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = full ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = full ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex = full ? VK_QUEUE_FAMILY_IGNORED : app->family,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	auto release = xVkImageMemoryBarrier2(
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = app->family,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	auto rendering = xVkRenderingInfo(
		.renderArea = { .extent = { (uint32_t)buffer->width, (uint32_t)buffer->height } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &xVkRenderingAttachmentInfo(
			.imageView = target->view,
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = full ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = { .color = background },
		),
	);
	auto submit = xVkSubmitInfo2(
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &xVkCommandBufferSubmitInfo(.commandBuffer = target->commands),
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &xVkSemaphoreSubmitInfo(
			.semaphore = app->rendered,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		),
	);
	auto get = xVkSemaphoreGetFdInfoKHR(
		.semaphore = app->rendered,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	);
	int fence;

	if (app->closing)
		return;
	if (!full) {
		foreach_array_const_ref(damage.rects, rect)
			count += clear_rect(rect, buffer->width, buffer->height, &clears[count]);
		foreach_array_const_ref(arrview_first(buffer->age - 1, app->history), older)
			foreach_array_const_ref(older->rects, rect)
				count += clear_rect(rect, buffer->width, buffer->height, &clears[count]);
	}
	clear_rect(&square, buffer->width, buffer->height, &square_clear);
	if (vkGetFenceStatus(app->device, target->done) != VK_SUCCESS) {
		log_printf("simple-vulkan: a buffer came back before its drawing was done\n");
		app_quit(app, 1);
		return;
	}
	if (!check(vkResetFences(app->device, 1, &target->done), "vkResetFences") ||
	    !check(vkBeginCommandBuffer(target->commands, &begin), "vkBeginCommandBuffer")) {
		app_quit(app, 1);
		return;
	}
	image_barrier(target->commands, &draw);
	vkCmdBeginRendering(target->commands, &rendering);
	if (count)
		vkCmdClearAttachments(target->commands, 1, &fill, count, clears);
	vkCmdClearAttachments(target->commands, 1, &paint, 1, &square_clear);
	vkCmdEndRendering(target->commands);
	image_barrier(target->commands, &release);
	if (!check(vkEndCommandBuffer(target->commands), "vkEndCommandBuffer") ||
	    !check(vkQueueSubmit2(app->queue, 1, &submit, target->done), "vkQueueSubmit2") ||
	    !check(app->get_semaphore_fd(app->device, &get, &fence), "vkGetSemaphoreFdKHR")) {
		app_quit(app, 1);
		return;
	}
	if (whole)
		poor_wl_swapchain_present(swapchain, buffer, fence);
	else
		poor_wl_swapchain_present_damage(swapchain, buffer, fence, damage.rects);
	array_insert(app->history, 0, damage);
	app->square = square;
	app->presented_width = buffer->width;
	app->presented_height = buffer->height;
	poor_wl_swapchain_request_frame(swapchain);
}

static void swapchain_error(struct poor_wl_swapchain *swapchain, int error)
{
	log_printf("simple-vulkan: swapchain: %s\n", strerror(error));
	app_quit(app_of_swapchain(swapchain), 1);
}

static void shell_surface_configure(void *data, struct poor_xdg_surface *shell_surface, uint32_t serial)
{
	struct app *app = data;

	poor_xdg_surface_ack_configure(shell_surface, serial);
	poor_wl_swapchain_resize(&app->swapchain, app->width, app->height);
	poor_wl_swapchain_request_frame(&app->swapchain);
}

static const struct poor_xdg_surface_listener shell_surface_listener = {
	.configure = shell_surface_configure,
};

static void toplevel_configure(void *data, struct poor_xdg_toplevel *, int32_t width, int32_t height, struct poor_wl_array *)
{
	struct app *app = data;

	app->width = width > 0 ? width : DEFAULT_WIDTH;
	app->height = height > 0 ? height : DEFAULT_HEIGHT;
}

static void toplevel_close(void *data, struct poor_xdg_toplevel *)
{
	app_quit(data, 0);
}

static const struct poor_xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void wm_base_ping(void *, struct poor_xdg_wm_base *wm_base, uint32_t serial)
{
	poor_xdg_wm_base_pong(wm_base, serial);
}

static const struct poor_xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* The surface feedback needs version 4 of linux-dmabuf */
static void registry_global(void *data, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct app *app = data;

	if (!strcmp(interface, poor_wl_compositor_interface.name) && !app->have_compositor && version >= 4) {
		app->have_compositor = poor_wl_registry_bind(registry, name, &poor_wl_compositor_interface, 4, &app->compositor);
	} else if (!strcmp(interface, poor_zwp_linux_dmabuf_v1_interface.name) && !app->have_dmabuf && version >= 4) {
		app->have_dmabuf = poor_wl_registry_bind(registry, name, &poor_zwp_linux_dmabuf_v1_interface,
						     version < 6 ? version : 6, &app->dmabuf);
	} else if (!strcmp(interface, poor_wp_linux_drm_syncobj_manager_v1_interface.name) && !app->have_syncobj) {
		app->have_syncobj = poor_wl_registry_bind(registry, name, &poor_wp_linux_drm_syncobj_manager_v1_interface, 1,
							  &app->syncobj);
	} else if (!strcmp(interface, poor_xdg_wm_base_interface.name) && !app->have_wm_base) {
		app->have_wm_base = poor_wl_registry_bind(registry, name, &poor_xdg_wm_base_interface, 1, &app->wm_base);
		poor_xdg_wm_base_add_listener(&app->wm_base, &wm_base_listener, app);
	}
}

static void registry_global_remove(void *, struct poor_wl_registry *, uint32_t)
{
}

static const struct poor_wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* The sync callback comes after all globals were announced */
static void synced(void *data, struct poor_wl_callback *callback, uint32_t)
{
	struct app *app = data;

	poor_wl_callback_destroy(callback);
	if (!app->have_compositor || !app->have_dmabuf || !app->have_syncobj || !app->have_wm_base) {
		log_printf("simple-vulkan: the compositor has no wl_compositor 4, zwp_linux_dmabuf_v1 4, "
			   "wp_linux_drm_syncobj_manager_v1 or xdg_wm_base\n");
		app_quit(app, 1);
		return;
	}
	poor_wl_compositor_create_surface(&app->compositor, &app->surface);
	poor_xdg_wm_base_get_xdg_surface(&app->wm_base, &app->shell_surface, &app->surface);
	poor_xdg_surface_add_listener(&app->shell_surface, &shell_surface_listener, app);
	poor_xdg_surface_get_toplevel(&app->shell_surface, &app->toplevel);
	poor_xdg_toplevel_add_listener(&app->toplevel, &toplevel_listener, app);
	poor_xdg_toplevel_set_title(&app->toplevel, "poor_wayland");
	poor_xdg_toplevel_set_app_id(&app->toplevel, "poor_wayland.simple-vulkan");
	poor_wl_swapchain_init(&app->swapchain, &app->loop, &app->dmabuf, &app->surface, formats, swapchain_device,
			       swapchain_frame, swapchain_error);
	app->swapchain_started = true;
	poor_wl_surface_commit(&app->surface);
}

static const struct poor_wl_callback_listener sync_listener = {
	.done = synced,
};

static void display_error(struct poor_wl_display *display)
{
	struct app *app = container_of(display, struct app, display);
	int error = poor_wl_display_get_error(display);
	const struct poor_wl_interface *interface;
	uint32_t code, id;

	if (error == EPROTO) {
		code = poor_wl_display_get_protocol_error(display, &interface, &id);
		log_printf("simple-vulkan: protocol error %u on %s#%u\n", code, interface ? interface->name : "[unknown]", id);
	} else {
		log_printf("simple-vulkan: connection: %s\n", strerror(error));
	}
	app_quit(app, 1);
}

static void display_done(struct poor_wl_display *display)
{
	poor_loop_stop(&container_of(display, struct app, display)->loop);
}

/* Signals are unblocked afterwards, so a second one kills a client that hangs */
POOR_LOOP_GROUP_OP(on_signal, loop, file, op, cqe)
{
	struct app *app = container_of(file, struct app, signals);

	if (cqe->res == sizeof(app->siginfo))
		log_printf("simple-vulkan: %s\n", strsignal(app->siginfo.ssi_signo));
	sigprocmask(SIG_UNBLOCK, &app->sigset, nullptr);
	app_quit(app, 0);
}

POOR_LOOP_FD_DONE(signal_file, loop, file)
{
	container_of(file, struct app, signals)->signals_closed = true;
}

[[nodiscard]] static int app_init(struct app *app)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(app->in, app->out, app->objects, app->server_objects);
	int fd;

	app->have_compositor = app->have_dmabuf = app->have_syncobj = app->have_wm_base = false;
	app->swapchain_started = app->swapchain_closed = app->closing = false;
	app->signals_closed = true;
	app->instance = nullptr;
	app->physical = nullptr;
	app->device = nullptr;
	app->queue = nullptr;
	app->pool = VK_NULL_HANDLE;
	app->rendered = app->idle = VK_NULL_HANDLE;
	app->drained = VK_NULL_HANDLE;
	app->drm_fd = app->idle_fd = -1;
	poor_loop_op_init(&app->open_op, on_open);
	poor_loop_op_init(&app->idle_op, on_idle);
	app->status = 0;
	app->width = DEFAULT_WIDTH;
	app->height = DEFAULT_HEIGHT;
	app->presented_width = app->presented_height = 0;
	app->format = 0;
	if (poor_wl_display_connect(&app->display, &app->loop, &memory, nullptr, display_error, display_done) < 0) {
		log_printf("simple-vulkan: can't connect: %s\n", strerror(errno));
		return -1;
	}
	poor_wl_display_get_registry(&app->display, &app->registry);
	poor_wl_registry_add_listener(&app->registry, &registry_listener, app);
	poor_wl_display_sync(&app->display, &app->sync);
	poor_wl_callback_add_listener(&app->sync, &sync_listener, app);

	sigemptyset(&app->sigset);
	sigaddset(&app->sigset, SIGINT);
	sigaddset(&app->sigset, SIGTERM);
	sigprocmask(SIG_BLOCK, &app->sigset, nullptr);
	fd = signalfd(-1, &app->sigset, SFD_CLOEXEC);
	if (fd < 0) {
		log_printf("simple-vulkan: signalfd: %s\n", strerror(errno));
		app_quit(app, 1);
		return 0;
	}
	poor_loop_fd_init(&app->signals, fd);
	poor_loop_op_init(&app->signal_op, on_signal);
	app->signals_closed = false;
	io_uring_prep_read_array(need_sqe(poor_loop_group_get_sqe_or_submit(&app->loop, &app->signals, &app->signal_op)), fd,
				 array_ptr(&app->siginfo), -1);
	return 0;
}

/* Write out the log, then wait for the closes, which nobody tracks */
static int app_finish(struct app *app)
{
	int ret = 0;

	while (!ret && (!app->signals_closed || (app->swapchain_started && !app->swapchain_closed) || app->open_op.pending ||
			app->idle_op.pending || log_pending()))
		ret = poor_loop_run_once(&app->loop);
	if (!ret)
		ret = poor_loop_drain(&app->loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	poor_loop_exit(&app->loop);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-vulkan: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	return app->status;
}

int main(void)
{
	static struct app app;
	struct io_uring_params params = {};
	int ret;

	ret = poor_loop_init(&app.loop, 64, &params);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-vulkan: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	log_init(&app.loop, "simple-vulkan");
	poor_wl_log_set_handler_client(log_vprintf);
	if (app_init(&app) < 0) {
		app.status = 1;
		return app_finish(&app);
	}
	ret = poor_loop_run(&app.loop);
	if (ret) {
		dprintf(STDERR_FILENO, "simple-vulkan: io_uring: %s\n", strerror(-ret));
		return 1;
	}
	return app_finish(&app);
}
