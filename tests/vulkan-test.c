/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <vk_init.h>

#include "keep-loaded.h"
#include "poor_wayland_client_display.h"
#include "poor_wayland_swapchain_vulkan.h"
#include "swapchain-server.h"
#include "test-loop.h"
#include "test-runner.h"


static const uint32_t formats[] = { DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB8888 };

/* How the image of a buffer is drawn */
struct target {
	VkImageView view;
	VkCommandBuffer commands;
	VkFence done;
};

struct client {
	struct poor_wl_display display;
	struct poor_loop *loop;
	struct poor_wl_registry registry;
	struct poor_wl_callback callback;
	struct poor_wl_compositor compositor;
	struct poor_zwp_linux_dmabuf_v1 dmabuf;
	struct poor_wp_linux_drm_syncobj_manager_v1 syncobj;
	struct poor_wl_surface surface;
	struct poor_wl_swapchain swapchain;
	struct poor_wl_vulkan vulkan;
	struct target targets[POOR_WL_SWAPCHAIN_BUFFERS];
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkCommandPool pool;
	VkSemaphore rendered;
	PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
	uint32_t family;
	unsigned frames, max_frames, allocations, frees, partial_frames;
	bool ten_bit;
	bool synced, done, error_called, swapchain_done;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

static struct client *client_of(struct poor_wl_swapchain *swapchain)
{
	return container_of(swapchain, struct client, swapchain);
}

/* A device of the render node, if Vulkan has one with what the swapchain needs */
static bool vulkan_start(struct client *c, dev_t device)
{
	static const float priority = 1.0f;
	auto application = xVkApplicationInfo(.pApplicationName = "vulkan-test", .apiVersion = VK_API_VERSION_1_4);
	VkQueueFamilyProperties2 families[16];
	uint32_t count = ARRAY_SIZE(families), render_minor;

	assert(vkCreateInstance(&xVkInstanceCreateInfo(.pApplicationInfo = &application), nullptr, &c->instance) == VK_SUCCESS);
	keep_loaded();
	c->physical = poor_wl_vulkan_find_device(vkGetInstanceProcAddr, c->instance, device, &render_minor);
	if (!c->physical) {
		vkDestroyInstance(c->instance, nullptr);
		return false;
	}
	assert(render_minor == minor(device));
	foreach_array_ref(families, properties)
		*properties = xVkQueueFamilyProperties2();
	vkGetPhysicalDeviceQueueFamilyProperties2(c->physical, &count, families);
	c->family = count;
	foreach_array_index(arrview_first(count, families), i) {
		if (families[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			c->family = i;
			break;
		}
	}
	assert(c->family < count);
	auto device_info = xVkDeviceCreateInfo(
		.pNext = &xVkPhysicalDeviceVulkan13Features(.synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE),
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &xVkDeviceQueueCreateInfo(
			.queueFamilyIndex = c->family,
			.queueCount = 1,
			.pQueuePriorities = &priority,
		),
		.enabledExtensionCount = ARRAY_SIZE(poor_wl_vulkan_device_extensions),
		.ppEnabledExtensionNames = poor_wl_vulkan_device_extensions,
	);
	auto pool_info = xVkCommandPoolCreateInfo(
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = c->family,
	);
	auto semaphore_info = xVkSemaphoreCreateInfo(
		.pNext = &xVkExportSemaphoreCreateInfo(.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT),
	);

	assert(vkCreateDevice(c->physical, &device_info, nullptr, &c->device) == VK_SUCCESS);
	keep_loaded();
	vkGetDeviceQueue2(c->device, &xVkDeviceQueueInfo2(.queueFamilyIndex = c->family), &c->queue);
	c->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(c->device, "vkGetSemaphoreFdKHR");
	assert(vkCreateCommandPool(c->device, &pool_info, nullptr, &c->pool) == VK_SUCCESS);
	assert(vkCreateSemaphore(c->device, &semaphore_info, nullptr, &c->rendered) == VK_SUCCESS);
	poor_wl_vulkan_init(&c->vulkan, c->loop, vkGetInstanceProcAddr, c->instance, c->physical, c->device,
			    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	return true;
}

static void vulkan_stop(struct client *c)
{
	vkDestroySemaphore(c->device, c->rendered, nullptr);
	vkDestroyCommandPool(c->device, c->pool, nullptr);
	vkDestroyDevice(c->device, nullptr);
	vkDestroyInstance(c->instance, nullptr);
}

/* Up to half of the format table gets the modifiers that the driver draws 'format' into, then linear */
static unsigned add_format(struct client *c, uint32_t format, unsigned n)
{
	VkDrmFormatModifierProperties2EXT driver[64];
	auto list = xVkDrmFormatModifierPropertiesList2EXT(
		.drmFormatModifierCount = ARRAY_SIZE(driver),
		.pDrmFormatModifierProperties = driver,
	);
	unsigned end = n + ARRAY_SIZE(swapchain_server.table) / 2 - 1;

	vkGetPhysicalDeviceFormatProperties2(c->physical, poor_wl_vulkan_format(format), &xVkFormatProperties2(.pNext = &list));
	foreach_array_const_ref(arrview_first(list.drmFormatModifierCount, driver), listed) {
		if (!(listed->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT))
			continue;
		c->ten_bit |= format == DRM_FORMAT_XRGB2101010;
		if (n == end || listed->drmFormatModifier == DRM_FORMAT_MOD_LINEAR)
			continue;
		swapchain_server.table[n].format = format;
		swapchain_server.table[n].padding = 0;
		swapchain_server.table[n++].modifier = listed->drmFormatModifier;
	}
	swapchain_server.table[n].format = format;
	swapchain_server.table[n].padding = 0;
	swapchain_server.table[n++].modifier = DRM_FORMAT_MOD_LINEAR;
	return n;
}

/* The feedback has the 10 and the 8 bit formats that the driver draws into */
static void server_config(struct client *c, dev_t device, int syncobj_fd)
{
	unsigned n;

	c->ten_bit = false;
	n = add_format(c, DRM_FORMAT_XRGB8888, add_format(c, DRM_FORMAT_XRGB2101010, 0));
	swapchain_server.dmabuf_version = 4;
	swapchain_server.main_device = device;
	swapchain_server.entries = n;
	swapchain_server.tranche_count = 1;
	swapchain_server.tranches[0].device = device;
	swapchain_server.tranches[0].flags = 0;
	swapchain_server.tranches[0].count = n;
	foreach_array_index(arrview_first(n, swapchain_server.tranches[0].indices), i)
		swapchain_server.tranches[0].indices[i] = i;
	swapchain_server.syncobj_fd = syncobj_fd;
}

static struct target *target_of(struct client *c, struct poor_wl_swapchain_buffer *buffer)
{
	return &c->targets[array_ref_index(c->swapchain.buffers, buffer)];
}

static int allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, unsigned count,
		    const uint64_t (*modifiers)[count], bool scanout)
{
	struct client *c = data;
	struct target *target = target_of(c, buffer);
	auto commands_info = xVkCommandBufferAllocateInfo(
		.commandPool = c->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	);
	auto fence_info = xVkFenceCreateInfo(.flags = VK_FENCE_CREATE_SIGNALED_BIT);
	int ret = poor_wl_vulkan_allocator.allocate(&c->vulkan, buffer, format, count, modifiers, scanout);

	if (ret < 0)
		return ret;
	const struct poor_wl_vulkan_image *image = buffer->data;
	auto view_info = xVkImageViewCreateInfo(
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = image->format,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	assert(vkCreateImageView(c->device, &view_info, nullptr, &target->view) == VK_SUCCESS);
	assert(vkAllocateCommandBuffers(c->device, &commands_info, &target->commands) == VK_SUCCESS);
	assert(vkCreateFence(c->device, &fence_info, nullptr, &target->done) == VK_SUCCESS);
	c->allocations++;
	return 0;
}

static void free_buffer(void *data, struct poor_wl_swapchain_buffer *buffer)
{
	struct client *c = data;
	struct target *target = target_of(c, buffer);

	assert(vkGetFenceStatus(c->device, target->done) == VK_SUCCESS);
	vkFreeCommandBuffers(c->device, c->pool, 1, &target->commands);
	vkDestroyFence(c->device, target->done, nullptr);
	vkDestroyImageView(c->device, target->view, nullptr);
	poor_wl_vulkan_allocator.free(&c->vulkan, buffer);
	c->frees++;
}

static const struct poor_wl_swapchain_allocator allocator = {
	.allocate = allocate,
	.free = free_buffer,
};

static void swapchain_device(struct poor_wl_swapchain *, dev_t)
{
}

static const struct poor_wl_swapchain_rect dot[] = { { 1, 1, 8, 8 } };

static void image_barrier(VkCommandBuffer commands, const VkImageMemoryBarrier2 *barrier)
{
	vkCmdPipelineBarrier2(commands, &xVkDependencyInfo(.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = barrier));
}

/*
 * A buffer without an age is cleared, one with an age comes back from the queue family of the compositor with its
 * contents and only gets a dot redrawn, as in simple-vulkan. The image goes to the compositor with the fence of the
 * batch. A buffer comes back after its last drawing, which Vulkan learns from the fence of that batch.
 */
static void swapchain_frame(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer, uint32_t)
{
	struct client *c = client_of(swapchain);
	struct target *target = target_of(c, buffer);
	VkImage image = ((struct poor_wl_vulkan_image *)buffer->data)->image;
	bool partial = buffer->age;
	VkClearRect clear = { .rect = { { dot[0].x, dot[0].y }, { (uint32_t)dot[0].width, (uint32_t)dot[0].height } },
			      .layerCount = 1 };
	auto begin = xVkCommandBufferBeginInfo(.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	auto draw = xVkImageMemoryBarrier2(
		.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
		.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = partial ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = partial ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = partial ? c->family : VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	auto release = xVkImageMemoryBarrier2(
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = c->family,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = image,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
	);
	auto attachment = xVkRenderingAttachmentInfo(
		.imageView = target->view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = partial ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .color = { .float32 = { 0.25f * (c->frames % 4), 0.5f, 1.0f, 1.0f } } },
	);
	VkClearAttachment fill = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue = attachment.clearValue };
	auto rendering = xVkRenderingInfo(
		.renderArea = { .extent = { (uint32_t)buffer->width, (uint32_t)buffer->height } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &attachment,
	);
	auto submit = xVkSubmitInfo2(
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &xVkCommandBufferSubmitInfo(.commandBuffer = target->commands),
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &xVkSemaphoreSubmitInfo(
			.semaphore = c->rendered,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		),
	);
	auto get = xVkSemaphoreGetFdInfoKHR(
		.semaphore = c->rendered,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	);
	int fence;

	assert(buffer->width == swapchain->width && buffer->height == swapchain->height);
	assert(vkGetFenceStatus(c->device, target->done) == VK_SUCCESS);
	assert(vkResetFences(c->device, 1, &target->done) == VK_SUCCESS);
	assert(vkBeginCommandBuffer(target->commands, &begin) == VK_SUCCESS);
	image_barrier(target->commands, &draw);
	vkCmdBeginRendering(target->commands, &rendering);
	if (partial)
		vkCmdClearAttachments(target->commands, 1, &fill, 1, &clear);
	vkCmdEndRendering(target->commands);
	image_barrier(target->commands, &release);
	assert(vkEndCommandBuffer(target->commands) == VK_SUCCESS);
	assert(vkQueueSubmit2(c->queue, 1, &submit, target->done) == VK_SUCCESS);
	assert(c->get_semaphore_fd(c->device, &get, &fence) == VK_SUCCESS);
	c->frames++;
	c->partial_frames += partial;
	if (partial)
		poor_wl_swapchain_present_damage(swapchain, buffer, fence, dot);
	else
		poor_wl_swapchain_present(swapchain, buffer, fence);
	if (c->frames < c->max_frames)
		poor_wl_swapchain_request_frame(swapchain);
}

static void swapchain_error(struct poor_wl_swapchain *, int error)
{
	fprintf(stderr, "swapchain: %s\n", strerror(error));
	abort();
}

static void swapchain_done(struct poor_wl_swapchain *swapchain)
{
	client_of(swapchain)->swapchain_done = true;
}

static void registry_global(void *data, struct poor_wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct client *c = data;

	if (!strcmp(interface, poor_wl_compositor_interface.name))
		poor_wl_registry_bind(registry, name, &poor_wl_compositor_interface, 4, &c->compositor);
	else if (!strcmp(interface, poor_zwp_linux_dmabuf_v1_interface.name))
		poor_wl_registry_bind(registry, name, &poor_zwp_linux_dmabuf_v1_interface, version, &c->dmabuf);
	else if (!strcmp(interface, poor_wp_linux_drm_syncobj_manager_v1_interface.name))
		poor_wl_registry_bind(registry, name, &poor_wp_linux_drm_syncobj_manager_v1_interface, 1, &c->syncobj);
}

static void registry_global_remove(void *, struct poor_wl_registry *, uint32_t)
{
}

static const struct poor_wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void sync_done(void *data, struct poor_wl_callback *callback, uint32_t)
{
	((struct client *)data)->synced = true;
	poor_wl_callback_destroy(callback);
}

static const struct poor_wl_callback_listener sync_listener = {
	.done = sync_done,
};

static void client_roundtrip(struct client *c)
{
	c->synced = false;
	poor_wl_display_sync(&c->display, &c->callback);
	poor_wl_callback_add_listener(&c->callback, &sync_listener, c);
	run_until(c->loop, c->synced || c->error_called);
	assert(!c->error_called);
}

static void client_error(struct poor_wl_display *display)
{
	container_of(display, struct client, display)->error_called = true;
}

static void client_done(struct poor_wl_display *display)
{
	container_of(display, struct client, display)->done = true;
}

static void client_start(struct client *c, int syncobj_fd)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);

	c->frames = c->max_frames = c->allocations = c->frees = c->partial_frames = 0;
	c->synced = c->done = c->error_called = c->swapchain_done = false;
	poor_wl_display_connect_to_fd(&c->display, c->loop, &memory, swapchain_server_connect(), client_error, client_done);
	poor_wl_display_get_registry(&c->display, &c->registry);
	poor_wl_registry_add_listener(&c->registry, &registry_listener, c);
	client_roundtrip(c);
	poor_wl_compositor_create_surface(&c->compositor, &c->surface);
	poor_wl_swapchain_init(&c->swapchain, c->loop, &c->dmabuf, &c->surface, formats, swapchain_device,
			       swapchain_frame, swapchain_error);
	assert(!poor_wl_swapchain_set_syncobj(&c->swapchain, &c->syncobj, syncobj_fd));
	poor_wl_swapchain_set_allocator(&c->swapchain, &allocator, c);
	client_roundtrip(c);
}

/* The buffers go once the device is done with them */
static void client_stop(struct client *c)
{
	assert(vkDeviceWaitIdle(c->device) == VK_SUCCESS);
	poor_wl_swapchain_destroy(&c->swapchain, swapchain_done);
	poor_wl_surface_destroy(&c->surface);
	client_roundtrip(c);
	run_until(c->loop, c->swapchain_done);
	assert(c->frees == c->allocations && swapchain_server.buffers_destroyed == swapchain_server.buffers_created);
	poor_wl_display_disconnect(&c->display);
	run_until(c->loop, c->done);
}

static void client_present(struct client *c, int32_t width, int32_t height, unsigned frames)
{
	unsigned commits = swapchain_server.commits;

	c->max_frames = c->frames + frames;
	poor_wl_swapchain_resize(&c->swapchain, width, height);
	poor_wl_swapchain_request_frame(&c->swapchain);
	run_until(c->loop, swapchain_server.commits == commits + frames);
	assert(c->frames == c->max_frames && swapchain_server.width == width && swapchain_server.height == height);
}

/*
 * The compositor gets the fences of the drawing on the acquire points, and the buffers come back through the
 * release points. Without a GPU, as in CI, there is nothing to draw with. The validation layer keeps its log open,
 * so the fds are counted from the start of the swapchain to the end of the loop, which closes the render node and
 * the ring.
 */
TEST(vulkan_frames)
{
	static struct client c;
	struct poor_loop loop;
	dev_t device = makedev(226, 128);
	int fd, fds;

	loop_init(&loop);
	c.loop = &loop;
	fd = render_node_open(&loop);
	if (fd < 0 || !vulkan_start(&c, device)) {
		if (fd >= 0)
			poor_loop_close_fd_or_submit(&loop, fd, 0);
		loop_fini(&loop);
		return;
	}
	DISABLE_LEAK_CHECKS;
	fds = count_open_fds();
	server_config(&c, device, fd);
	swapchain_server_start(&loop);
	client_start(&c, fd);
	client_present(&c, 64, 32, 10);
	client_present(&c, 96, 64, 10);
	assert(swapchain_server.acquires == 20 && swapchain_server.planes >= 1 && c.allocations >= 2 && c.partial_frames);
	assert(swapchain_server.format == (c.ten_bit ? DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888));
	client_stop(&c);
	swapchain_server_stop();
	run_until(&loop, swapchain_server.display_done);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	loop_fini(&loop);
	assert(count_open_fds() == fds - 2);
	vulkan_stop(&c);
}
