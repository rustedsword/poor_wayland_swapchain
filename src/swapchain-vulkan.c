/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <poor_loop_fd.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <vk_init.h>

#include "poor_wayland_swapchain_vulkan.h"

constexpr uint32_t DEVICE_EXTENSIONS = 512;
constexpr uint32_t DRIVER_MODIFIERS = 128;

POOR_WL_EXPORT const char *const poor_wl_vulkan_device_extensions[POOR_WL_VULKAN_DEVICE_EXTENSIONS] = {
	VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
	VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

static bool has_extensions(PFN_vkEnumerateDeviceExtensionProperties enumerate, VkPhysicalDevice physical)
{
	VkExtensionProperties properties[DEVICE_EXTENSIONS];
	uint32_t count = ARRAY_SIZE(properties), found = 0;

	if (enumerate(physical, nullptr, &count, properties) < 0)
		return false;
	foreach_array_const_ref(poor_wl_vulkan_device_extensions, extension) {
		foreach_array_const_ref(arrview_first(count, properties), listed) {
			if (!strcmp(*extension, listed->extensionName)) {
				found++;
				break;
			}
		}
	}
	return found == ARRAY_SIZE(poor_wl_vulkan_device_extensions);
}

POOR_WL_EXPORT VkPhysicalDevice poor_wl_vulkan_find_device(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance,
							   dev_t device, uint32_t *render_minor)
{
	PFN_vkEnumeratePhysicalDevices enumerate_devices =
		(PFN_vkEnumeratePhysicalDevices)get_proc(instance, "vkEnumeratePhysicalDevices");
	PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions =
		(PFN_vkEnumerateDeviceExtensionProperties)get_proc(instance, "vkEnumerateDeviceExtensionProperties");
	PFN_vkGetPhysicalDeviceProperties2 get_properties =
		(PFN_vkGetPhysicalDeviceProperties2)get_proc(instance, "vkGetPhysicalDeviceProperties2");
	VkPhysicalDevice physicals[16];
	uint32_t count = ARRAY_SIZE(physicals);

	if (enumerate_devices(instance, &count, physicals) < 0)
		return VK_NULL_HANDLE;
	foreach_array_const_ref(arrview_first(count, physicals), physical) {
		auto drm = xVkPhysicalDeviceDrmPropertiesEXT();
		auto properties = xVkPhysicalDeviceProperties2(.pNext = &drm);

		if (!has_extensions(enumerate_extensions, *physical))
			continue;
		get_properties(*physical, &properties);
		if (properties.properties.apiVersion < VK_API_VERSION_1_4 || !drm.hasRender)
			continue;
		if ((drm.hasPrimary && major(device) == drm.primaryMajor && minor(device) == drm.primaryMinor) ||
		    (major(device) == drm.renderMajor && minor(device) == drm.renderMinor)) {
			*render_minor = drm.renderMinor;
			return *physical;
		}
	}
	return VK_NULL_HANDLE;
}

POOR_WL_EXPORT void poor_wl_vulkan_init(struct poor_wl_vulkan *vulkan, struct poor_loop *loop, PFN_vkGetInstanceProcAddr get_proc,
					VkInstance instance, VkPhysicalDevice physical, VkDevice device, VkImageUsageFlags usage)
{
	PFN_vkGetDeviceProcAddr get_device_proc = (PFN_vkGetDeviceProcAddr)get_proc(instance, "vkGetDeviceProcAddr");

	vulkan->loop = loop;
	vulkan->physical = physical;
	vulkan->device = device;
	vulkan->usage = usage;
	vulkan->get_format_properties =
		(PFN_vkGetPhysicalDeviceFormatProperties2)get_proc(instance, "vkGetPhysicalDeviceFormatProperties2");
	vulkan->get_image_format_properties =
		(PFN_vkGetPhysicalDeviceImageFormatProperties2)get_proc(instance, "vkGetPhysicalDeviceImageFormatProperties2");
	vulkan->get_memory_properties =
		(PFN_vkGetPhysicalDeviceMemoryProperties2)get_proc(instance, "vkGetPhysicalDeviceMemoryProperties2");
	vulkan->create_image = (PFN_vkCreateImage)get_device_proc(device, "vkCreateImage");
	vulkan->destroy_image = (PFN_vkDestroyImage)get_device_proc(device, "vkDestroyImage");
	vulkan->get_memory_requirements =
		(PFN_vkGetImageMemoryRequirements2)get_device_proc(device, "vkGetImageMemoryRequirements2");
	vulkan->allocate_memory = (PFN_vkAllocateMemory)get_device_proc(device, "vkAllocateMemory");
	vulkan->free_memory = (PFN_vkFreeMemory)get_device_proc(device, "vkFreeMemory");
	vulkan->bind_memory = (PFN_vkBindImageMemory2)get_device_proc(device, "vkBindImageMemory2");
	vulkan->get_subresource_layout =
		(PFN_vkGetImageSubresourceLayout2)get_device_proc(device, "vkGetImageSubresourceLayout2");
	vulkan->get_memory_fd = (PFN_vkGetMemoryFdKHR)get_device_proc(device, "vkGetMemoryFdKHR");
	vulkan->get_image_modifier =
		(PFN_vkGetImageDrmFormatModifierPropertiesEXT)get_device_proc(device, "vkGetImageDrmFormatModifierPropertiesEXT");
	assert(vulkan->get_subresource_layout && vulkan->get_memory_fd && vulkan->get_image_modifier);
}

/*
 * DRM formats are little endian: the bytes of XRGB8888 are blue, green, red and the unused one. Packed Vulkan formats
 * are in the order of the host, so they only match on little endian hosts.
 */
static const struct {
	uint32_t drm;
	VkFormat vulkan;
} formats[] = {
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM },
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM },
	{ DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM },
	{ DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM },
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	{ DRM_FORMAT_XRGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32 },
	{ DRM_FORMAT_ARGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32 },
	{ DRM_FORMAT_XBGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
	{ DRM_FORMAT_ABGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
	{ DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16 },
	{ DRM_FORMAT_XBGR16161616, VK_FORMAT_R16G16B16A16_UNORM },
	{ DRM_FORMAT_ABGR16161616, VK_FORMAT_R16G16B16A16_UNORM },
	{ DRM_FORMAT_XBGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT },
	{ DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT },
#endif
};

POOR_WL_EXPORT VkFormat poor_wl_vulkan_format(uint32_t format)
{
	foreach_array_const_ref(formats, pair)
		if (pair->drm == format)
			return pair->vulkan;
	return VK_FORMAT_UNDEFINED;
}

static bool exportable(struct poor_wl_vulkan *vulkan, VkFormat format, uint64_t modifier,
		       const struct poor_wl_swapchain_buffer *buffer)
{
	auto info = xVkPhysicalDeviceImageFormatInfo2(
		.pNext = &xVkPhysicalDeviceExternalImageFormatInfo(
			.pNext = &xVkPhysicalDeviceImageDrmFormatModifierInfoEXT(
				.drmFormatModifier = modifier,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			),
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		),
		.format = format,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = vulkan->usage,
	);
	auto external = xVkExternalImageFormatProperties();
	auto properties = xVkImageFormatProperties2(.pNext = &external);

	return vulkan->get_image_format_properties(vulkan->physical, &info, &properties) == VK_SUCCESS &&
	       external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT &&
	       (uint32_t)buffer->width <= properties.imageFormatProperties.maxExtent.width &&
	       (uint32_t)buffer->height <= properties.imageFormatProperties.maxExtent.height;
}

/* The format features that the usage needs, for the modifiers that the driver lists */
static VkFormatFeatureFlags2 usage_features(VkImageUsageFlags usage)
{
	return (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ? VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT : 0) |
	       (usage & VK_IMAGE_USAGE_STORAGE_BIT ? VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT : 0) |
	       (usage & VK_IMAGE_USAGE_SAMPLED_BIT ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT : 0) |
	       (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT ? VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT : 0) |
	       (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT ? VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT : 0);
}

/* The modifiers of the tranche that the driver has for the usage and shares, with their numbers of memory planes */
static unsigned usable_modifiers(struct poor_wl_vulkan *vulkan, const struct poor_wl_swapchain_buffer *buffer, VkFormat format,
				 unsigned count, const uint64_t (*modifiers)[count], uint64_t (*usable)[count],
				 uint32_t (*planes)[count])
{
	VkDrmFormatModifierProperties2EXT driver[DRIVER_MODIFIERS];
	auto list = xVkDrmFormatModifierPropertiesList2EXT(
		.drmFormatModifierCount = ARRAY_SIZE(driver),
		.pDrmFormatModifierProperties = driver,
	);
	VkFormatFeatureFlags2 features = usage_features(vulkan->usage);
	unsigned n = 0;

	vulkan->get_format_properties(vulkan->physical, format, &xVkFormatProperties2(.pNext = &list));
	foreach_array_const_ref(modifiers, modifier) {
		foreach_array_const_ref(arrview_first(list.drmFormatModifierCount, driver), listed) {
			if (listed->drmFormatModifier != *modifier ||
			    (listed->drmFormatModifierTilingFeatures & features) != features ||
			    listed->drmFormatModifierPlaneCount > ARRAY_SIZE(buffer->planes) ||
			    !exportable(vulkan, format, *modifier, buffer))
				continue;
			(*usable)[n] = *modifier;
			(*planes)[n++] = listed->drmFormatModifierPlaneCount;
			break;
		}
	}
	return n;
}

/* Device local memory if there is, the image has the memory for itself */
static VkResult image_init(struct poor_wl_vulkan *vulkan, struct poor_wl_vulkan_image *image, const VkImageCreateInfo *info)
{
	auto properties = xVkPhysicalDeviceMemoryProperties2();
	const VkPhysicalDeviceMemoryProperties *memory = &properties.memoryProperties;
	auto requirements = xVkMemoryRequirements2();
	uint32_t type = UINT32_MAX;
	VkResult result;

	image->memory = VK_NULL_HANDLE;
	result = vulkan->create_image(vulkan->device, info, nullptr, &image->image);
	if (result != VK_SUCCESS) {
		image->image = VK_NULL_HANDLE;
		return result;
	}
	vulkan->get_memory_requirements(vulkan->device, &xVkImageMemoryRequirementsInfo2(.image = image->image), &requirements);
	vulkan->get_memory_properties(vulkan->physical, &properties);
	foreach_array_index(arrview_first(memory->memoryTypeCount, memory->memoryTypes), i) {
		if (!(requirements.memoryRequirements.memoryTypeBits & 1u << i))
			continue;
		if (type == UINT32_MAX || memory->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			type = i;
		if (memory->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			break;
	}
	if (type == UINT32_MAX)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	auto allocate = xVkMemoryAllocateInfo(
		.pNext = &xVkExportMemoryAllocateInfo(
			.pNext = &xVkMemoryDedicatedAllocateInfo(.image = image->image),
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		),
		.allocationSize = requirements.memoryRequirements.size,
		.memoryTypeIndex = type,
	);
	result = vulkan->allocate_memory(vulkan->device, &allocate, nullptr, &image->memory);
	if (result != VK_SUCCESS) {
		image->memory = VK_NULL_HANDLE;
		return result;
	}
	return vulkan->bind_memory(vulkan->device, 1, &xVkBindImageMemoryInfo(.image = image->image, .memory = image->memory));
}

static void image_fini(struct poor_wl_vulkan *vulkan, struct poor_wl_vulkan_image *image)
{
	vulkan->destroy_image(vulkan->device, image->image, nullptr);
	vulkan->free_memory(vulkan->device, image->memory, nullptr);
}

/* The memory planes share the memory, each with an fd of its own */
static VkResult image_export(struct poor_wl_vulkan *vulkan, struct poor_wl_vulkan_image *image,
			     struct poor_wl_swapchain_buffer *buffer, uint32_t planes)
{
	auto get = xVkMemoryGetFdInfoKHR(.memory = image->memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
	auto layout = xVkSubresourceLayout2();
	VkResult result;

	foreach_array_index(arrview_first(planes, buffer->planes), i) {
		auto subresource = xVkImageSubresource2(
			.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i },
		);

		result = vulkan->get_memory_fd(vulkan->device, &get, &buffer->planes[i].fd);
		if (result != VK_SUCCESS) {
			foreach_array_const_ref(arrview_first(i, buffer->planes), plane)
				poor_loop_close_fd_or_submit(vulkan->loop, plane->fd, 0);
			return result;
		}
		vulkan->get_subresource_layout(vulkan->device, image->image, &subresource, &layout);
		buffer->planes[i].offset = layout.subresourceLayout.offset;
		buffer->planes[i].stride = layout.subresourceLayout.rowPitch;
	}
	buffer->plane_count = planes;
	return VK_SUCCESS;
}

/* Vulkan picks one of the modifiers, each memory plane of the image becomes a plane of the buffer */
static int vulkan_allocate(void *data, struct poor_wl_swapchain_buffer *buffer, uint32_t format, unsigned count,
			   const uint64_t (*modifiers)[count], bool)
{
	struct poor_wl_vulkan *vulkan = data;
	struct poor_wl_vulkan_image *image = &vulkan->images[array_ref_index(buffer->swapchain->buffers, buffer)];
	VkFormat vulkan_format = poor_wl_vulkan_format(format);
	uint64_t usable[POOR_WL_SWAPCHAIN_MODIFIERS];
	uint32_t planes[POOR_WL_SWAPCHAIN_MODIFIERS];
	unsigned n;

	assert(count <= ARRAY_SIZE(usable));
	if (vulkan_format == VK_FORMAT_UNDEFINED)
		return -ENOTSUP;
	n = usable_modifiers(vulkan, buffer, vulkan_format, count, modifiers, arrview_first(count, usable),
			     arrview_first(count, planes));
	auto info = xVkImageCreateInfo(
		.pNext = &xVkExternalMemoryImageCreateInfo(
			.pNext = &xVkImageDrmFormatModifierListCreateInfoEXT(
				.drmFormatModifierCount = n,
				.pDrmFormatModifiers = usable,
			),
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		),
		.imageType = VK_IMAGE_TYPE_2D,
		.format = vulkan_format,
		.extent = { (uint32_t)buffer->width, (uint32_t)buffer->height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = vulkan->usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	);
	auto chosen = xVkImageDrmFormatModifierPropertiesEXT();

	if (!n)
		return -ENOTSUP;
	if (image_init(vulkan, image, &info) != VK_SUCCESS ||
	    vulkan->get_image_modifier(vulkan->device, image->image, &chosen) != VK_SUCCESS) {
		image_fini(vulkan, image);
		return -ENOMEM;
	}
	foreach_array_index(arrview_first(n, usable), i) {
		if (usable[i] != chosen.drmFormatModifier)
			continue;
		if (image_export(vulkan, image, buffer, planes[i]) != VK_SUCCESS)
			break;
		image->format = vulkan_format;
		buffer->modifier = chosen.drmFormatModifier;
		buffer->data = image;
		return 0;
	}
	image_fini(vulkan, image);
	return -ENOMEM;
}

static void vulkan_free(void *data, struct poor_wl_swapchain_buffer *buffer)
{
	image_fini(data, buffer->data);
}

POOR_WL_EXPORT const struct poor_wl_swapchain_allocator poor_wl_vulkan_allocator = {
	.allocate = vulkan_allocate,
	.free = vulkan_free,
};
