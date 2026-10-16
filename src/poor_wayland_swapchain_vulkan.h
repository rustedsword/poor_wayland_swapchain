/* SPDX-License-Identifier: MIT */
#ifndef POOR_WAYLAND_SWAPCHAIN_VULKAN_H
#define POOR_WAYLAND_SWAPCHAIN_VULKAN_H

#include <vulkan/vulkan.h>

#include "poor_wayland_swapchain.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * Vulkan images for the swapchain. The functions come from the vkGetInstanceProcAddr of the caller, so the library
 * doesn't link the Vulkan loader. Vulkan has no implicit synchronization: present them with explicit
 * synchronization, with the fences of their drawing, after releasing them to VK_QUEUE_FAMILY_FOREIGN_EXT. Their
 * contents, which the age of buffers is about, stay when they are acquired back from VK_QUEUE_FAMILY_FOREIGN_EXT in the
 * layout of the release, not from VK_IMAGE_LAYOUT_UNDEFINED.
 */

constexpr uint32_t POOR_WL_VULKAN_DEVICE_EXTENSIONS = 6;

/* The device needs these extensions, also for the fences and the release */
extern const char *const poor_wl_vulkan_device_extensions[POOR_WL_VULKAN_DEVICE_EXTENSIONS];

struct poor_wl_vulkan_image {
	VkImage image;
	VkDeviceMemory memory;
	VkFormat format;
};

struct poor_wl_vulkan {
	struct poor_loop *loop;
	VkPhysicalDevice physical;
	VkDevice device;
	VkImageUsageFlags usage;
	struct poor_wl_vulkan_image images[POOR_WL_SWAPCHAIN_BUFFERS];
	PFN_vkGetPhysicalDeviceFormatProperties2 get_format_properties;
	PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format_properties;
	PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
	PFN_vkCreateImage create_image;
	PFN_vkDestroyImage destroy_image;
	PFN_vkGetImageMemoryRequirements get_memory_requirements;
	PFN_vkAllocateMemory allocate_memory;
	PFN_vkFreeMemory free_memory;
	PFN_vkBindImageMemory bind_memory;
	PFN_vkGetImageSubresourceLayout get_subresource_layout;
	PFN_vkGetMemoryFdKHR get_memory_fd;
	PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_modifier;
};

/*
 * The physical device with poor_wl_vulkan_device_extensions of 'device', the primary or the render node that the
 * dmabuf feedback names. Its render node, for explicit synchronization, has 'render_minor'. Returns VK_NULL_HANDLE
 * if there is none.
 */
VkPhysicalDevice
poor_wl_vulkan_find_device(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, dev_t device, uint32_t *render_minor);

/* Images for 'usage' on 'device', which has poor_wl_vulkan_device_extensions and Vulkan 1.1 */
void
poor_wl_vulkan_init(struct poor_wl_vulkan *vulkan, struct poor_loop *loop, PFN_vkGetInstanceProcAddr get_proc,
		    VkInstance instance, VkPhysicalDevice physical, VkDevice device, VkImageUsageFlags usage);

/* The Vulkan format of images of the DRM 'format', VK_FORMAT_UNDEFINED if there is none */
VkFormat
poor_wl_vulkan_format(uint32_t format);

/*
 * Allocates images with one of the explicit modifiers that the device can share, 'data' is the struct
 * poor_wl_vulkan and buffers' 'data' a struct poor_wl_vulkan_image, which has the Vulkan format of the buffer. Free
 * them once their drawing is done.
 */
extern const struct poor_wl_swapchain_allocator poor_wl_vulkan_allocator;

#ifdef  __cplusplus
}
#endif

#endif
