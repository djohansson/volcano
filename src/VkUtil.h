#pragma once

#include <vulkan/vulkan.h>

#include <cassert>
#include <string>
#include <vector>

static inline constexpr void CHECK_VK(VkResult err)
{
	(void)err;
	assert(err == VK_SUCCESS);
}

uint32_t getFormatSize(VkFormat format);

bool hasStencilComponent(VkFormat format);

uint32_t findMemoryType(
	VkPhysicalDevice device,
	uint32_t typeFilter,
	VkMemoryPropertyFlags properties);

VkFormat findSupportedFormat(
	VkPhysicalDevice device,
	const std::vector<VkFormat>& candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features);

int isDeviceSuitable(VkSurfaceKHR surface, VkPhysicalDevice device);

void readSPIRVFile(const std::string& filename, std::vector<char>& outData);
