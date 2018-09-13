#pragma once

#include <volk.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

class VkDeviceTable
{
	VkDevice myDevice = VK_NULL_HANDLE;
	VolkDeviceTable myDeviceTable;

public:

	VkDeviceTable(VkDevice device, VolkDeviceTable deviceTable)
		: myDevice(device)
		, myDeviceTable(deviceTable)
	{}

	inline VkResult vkAllocateCommandBuffers(const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers) const
	{
		return myDeviceTable.vkAllocateCommandBuffers(myDevice, pAllocateInfo, pCommandBuffers);
	}

	inline void vkGetDeviceQueue(uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) const
	{
		myDeviceTable.vkGetDeviceQueue(myDevice, queueFamilyIndex, queueIndex, pQueue);
	}

	// todo: add all vulkan functions here that uses the device ... *sigh*
	// ...
};

static inline void CHECK_VK(VkResult err)
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
