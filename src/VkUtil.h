#pragma once

#include <vulkan/vulkan.h>

#include <cassert>
#include <string>
#include <vector>

static inline constexpr void CHECK_VK(VkResult err)
{
	assert(err == VK_SUCCESS);
}

std::vector<char> readSPIRVFile(const std::string& filename);
