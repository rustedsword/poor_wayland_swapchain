/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "keep-loaded.h"

/*
 * Vulkan tests need a working validation layer, since the loader leaves out layers that it doesn't find. With
 * --report-error the layer reports an error, for the test of the runner.
 */
int main(int argc, char **argv)
{
	const bool report_error = argc == 2 && !strcmp(argv[1], "--report-error");
	const char *layer = "VK_LAYER_KHRONOS_validation";
	const VkApplicationInfo application = {
		.sType = report_error ? VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO : VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vulkan-validation-probe",
		.apiVersion = VK_API_VERSION_1_3,
	};
	const VkInstanceCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &application,
		.enabledLayerCount = 1,
		.ppEnabledLayerNames = &layer,
	};
	VkInstance instance;
	VkResult result;

	if (argc != 1 && !report_error)
		return 1;
	result = vkCreateInstance(&info, nullptr, &instance);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "Vulkan tests need a working %s, vkCreateInstance returned %d\n", layer, result);
		return 1;
	}
	keep_loaded();
	vkDestroyInstance(instance, nullptr);
	return 0;
}
