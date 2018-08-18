#include "Volcano.h"

#include <vulkan/vulkan.h>

#if defined(_WIN32)
#define NOMINMAX
#include <wtypes.h>
#endif

#if defined(VOLCANO_USE_GLFW)
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#elif defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#elif defined(__APPLE__)
#include <vulkan/vulkan_macos.h>
#elif defined(__linux__)
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <X11/Xutil.h>
#include <vulkan/vulkan_xcb.h>
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xutil.h>
#include <vulkan/vulkan_xlib.h>
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include <linux/input.h>
#include <vulkan/vulkan_wayland.h>
#endif
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <imgui.h>

#include <examples/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <assert.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

constexpr void CHECK_VK(VkResult err)
{
	assert(err == VK_SUCCESS);
}

template <typename T>
constexpr auto sizeof_array(const T& array)
{
	return (sizeof(array) / sizeof(array[0]));
}

template <typename T, typename U, typename V>
inline T clamp(T x, U lowerlimit, V upperlimit)
{
	return (x < lowerlimit ? lowerlimit : (x > upperlimit ? upperlimit : x));
}

template <typename T, typename U, typename V>
inline T ramp(T x, U edge0, V edge1)
{
	return (x - edge0) / (edge1 - edge0);
}

template <typename T>
inline T smoothstep(T x)
{
	return x * x * (3 - 2 * x);
}

template <typename T>
inline T smootherstep(T x)
{
	return x * x * x * (x * (x * 6 - 15) + 10);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugReportFlagsEXT flags,
													VkDebugReportObjectTypeEXT objType,
													uint64_t obj, size_t location, int32_t code,
													const char* layerPrefix, const char* msg,
													void* userData)
{
	std::cerr << layerPrefix << ": " << msg << std::endl;

	return VK_FALSE;
}

static std::vector<char> readSPIRVFile(const std::string& filename)
{
	std::ifstream file(filename, std::ios::ate | std::ios::binary);
	if (!file.is_open())
		throw std::runtime_error("failed to open file!");

	size_t fileSize = (size_t)file.tellg();
	std::vector<char> buffer(fileSize);

	file.seekg(0);
	file.read(buffer.data(), fileSize);
	file.close();

	return buffer;
}

struct Vertex
{
	float pos[2];
	float color[3];
	float texCoord[2];

	static VkVertexInputBindingDescription getBindingDescription()
	{
		VkVertexInputBindingDescription bindingDescription = {};
		bindingDescription.binding = 0;
		bindingDescription.stride = sizeof(Vertex);
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		return bindingDescription;
	}

	static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions()
	{
		std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions = {};
		attributeDescriptions[0].binding = 0;
		attributeDescriptions[0].location = 0;
		attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
		attributeDescriptions[0].offset = offsetof(Vertex, pos);
		attributeDescriptions[1].binding = 0;
		attributeDescriptions[1].location = 1;
		attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[1].offset = offsetof(Vertex, color);
		attributeDescriptions[2].binding = 0;
		attributeDescriptions[2].location = 2;
		attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
		attributeDescriptions[2].offset = offsetof(Vertex, texCoord);
		return attributeDescriptions;
	}
};

struct Vec4
{
	union
	{
		struct
		{
			float x;
			float y;
			float z;
			float w;
		};
		float data[4];
	};
};

struct UniformBufferObject
{
	Vec4 model[4];
	Vec4 view[4];
	Vec4 proj[4];
};

class VulkanApplication
{
  public:
	VulkanApplication(void* view, int width, int height, const char* resourcePath)
		: myResourcePath(resourcePath)
	{
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		initVulkan(view, surface);
		initIMGUI(height, width, surface);

		// upload geometry
		createDeviceLocalBuffer(ourVertices, static_cast<uint32_t>(sizeof_array(ourVertices)),
								VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, myVertexBuffer,
								myVertexBufferMemory);
		createDeviceLocalBuffer(ourIndices, static_cast<uint32_t>(sizeof_array(ourIndices)),
								VK_BUFFER_USAGE_INDEX_BUFFER_BIT, myIndexBuffer,
								myIndexBufferMemory);

		// upload textures
		{
			int x, y, n;
			unsigned char* data =
				stbi_load((myResourcePath + std::string("images/Vulkan_500px_Dec16.png")).c_str(),
						  &x, &y, &n, STBI_rgb_alpha);

			assert(data != nullptr);

			createDeviceLocalImage2D(data, x, y, 4, VK_IMAGE_USAGE_SAMPLED_BIT, myImage,
									 myImageMemory);

			stbi_image_free(data);

			myImageView = createImageView2D(myImage, VK_FORMAT_R8G8B8A8_UNORM);
		}

		// create uniform buffer
		createBuffer(sizeof(UniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
					 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					 myUniformBuffer, myUniformBufferMemory);

		// create pipeline and descriptors
		createTextureSampler();
		createDescriptorSetLayout();
		createDescriptorSet();
		createGraphicsPipeline();
	}

	~VulkanApplication()
	{
		cleanup();
	}

	void draw()
	{
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();

		/*
		{
			ImGui::Begin("Image");
			ImGui::Image(ImTextureID user_texture_id, const ImVec2 &size)
			ImGui::End();
		}
		 */

		ImGui::ShowDemoWindow();

		{
			ImGui::Begin("Render Options");
			ImGui::Checkbox("Clear Enable", &myWindowData->ClearEnable);
			ImGui::ColorEdit3("Clear Color", &myWindowData->ClearValue.color.float32[0]);
			ImGui::End();
		}

		{
			ImGui::Begin("GUI Options");
			static int styleIndex = 0;
			ImGui::ShowStyleSelector("Styles", &styleIndex);
			ImGui::ShowFontSelector("Fonts");
			if (ImGui::Button("Show User Guide"))
			{
				ImGui::SetNextWindowPosCenter();
				ImGui::OpenPopup("UserGuide");
			}
			if (ImGui::BeginPopup("UserGuide", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::ShowUserGuide();
				ImGui::EndPopup();
			}
			ImGui::End();
		}

		{
			ImGui::ShowMetricsWindow();
		}

		ImGui::Render();

		updateUniformBuffer();
		submitFrame();
		presentFrame();
	}

  private:
	void createInstance()
	{
		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "No Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_1;

		uint32_t instanceLayerCount;
		CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
		std::cout << instanceLayerCount << " layers found!\n";
		if (instanceLayerCount > 0)
		{
			std::unique_ptr<VkLayerProperties[]> instanceLayers(
				new VkLayerProperties[instanceLayerCount]);
			CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.get()));
			for (uint32_t i = 0; i < instanceLayerCount; ++i)
				std::cout << instanceLayers[i].layerName << "\n";
		}

		const char* enabledLayerNames[] = {"VK_LAYER_LUNARG_standard_validation"};

		uint32_t instanceExtensionCount;
		vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

		std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
		vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount,
											   availableInstanceExtensions.data());

		std::vector<const char*> instanceExtensions(instanceExtensionCount);
		for (uint32_t i = 0; i < instanceExtensionCount; i++)
			instanceExtensions[i] = availableInstanceExtensions[i].extensionName;

		std::sort(instanceExtensions.begin(), instanceExtensions.end(),
				  [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

		std::vector<const char*> requiredExtensions = {
			// must be sorted lexicographically for std::includes to work!
			"VK_KHR_surface",
#if defined(_WIN32)
			"VK_KHR_win32_surface",
#elif defined(__APPLE__)
			"VK_MVK_macos_surface",
#elif defined(__linux__)
#if defined(VK_USE_PLATFORM_XCB_KHR)
			"VK_KHR_xcb_surface",
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
			"VK_KHR_xlib_surface",
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
			"VK_KHR_wayland_surface",
#endif
#endif
		};

		assert(
			std::includes(instanceExtensions.begin(), instanceExtensions.end(),
						  requiredExtensions.begin(), requiredExtensions.end(),
						  [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

		// if (std::find(instanceExtensions.begin(), instanceExtensions.end(), "VK_KHR_display") ==
		// instanceExtensions.end()) 	instanceExtensions.push_back("VK_KHR_display");

		VkInstanceCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		info.pApplicationInfo = &appInfo;
		info.enabledLayerCount = 1;
		info.ppEnabledLayerNames = enabledLayerNames;
		info.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
		info.ppEnabledExtensionNames = instanceExtensions.data();

		CHECK_VK(vkCreateInstance(&info, NULL, &myInstance));
	}

	void createDebugCallback()
	{
		VkDebugReportCallbackCreateInfoEXT debugCallbackInfo = {};
		debugCallbackInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
		debugCallbackInfo.flags =
			/* VK_DEBUG_REPORT_INFORMATION_BIT_EXT |*/ VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
			VK_DEBUG_REPORT_DEBUG_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT |
			VK_DEBUG_REPORT_WARNING_BIT_EXT;
		debugCallbackInfo.pfnCallback = debugCallback;

		auto vkCreateDebugReportCallbackEXT =
			(PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(
				myInstance, "vkCreateDebugReportCallbackEXT");
		assert(vkCreateDebugReportCallbackEXT != nullptr);
		CHECK_VK(vkCreateDebugReportCallbackEXT(myInstance, &debugCallbackInfo, nullptr,
												&myDebugCallback));
	}

	void createSurface(void* view, VkSurfaceKHR& outSurface)
	{
#if defined(VOLCANO_USE_GLFW)
		CHECK_VK(glfwCreateWindowSurface(myInstance, reinterpret_cast<GLFWwindow*>(view), nullptr,
										 &outSurface));
#elif defined(_WIN32)
		VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.hwnd = (HWND)view;
		surfaceCreateInfo.hinstance = GetModuleHandle(nullptr);
		auto vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(
			myInstance, "vkCreateWin32SurfaceKHR");
		assert(vkCreateWin32SurfaceKHR != nullptr);
		CHECK_VK(vkCreateWin32SurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &outSurface));
#elif defined(__APPLE__)
		VkMacOSSurfaceCreateInfoMVK surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.pView = view;
		auto vkCreateMacOSSurfaceMVK = (PFN_vkCreateMacOSSurfaceMVK)vkGetInstanceProcAddr(
			myInstance, "vkCreateMacOSSurfaceMVK");
		assert(vkCreateMacOSSurfaceMVK != nullptr);
		CHECK_VK(vkCreateMacOSSurfaceMVK(myInstance, &surfaceCreateInfo, nullptr, &outSurface));
#elif defined(__linux__)
#if defined(VK_USE_PLATFORM_XCB_KHR)
		VkXcbSurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.connection = nullptr; //?;
		surfaceCreateInfo.window = nullptr; //?;
		auto vkCreateXcbSurfaceKHR =
			(PFN_vkCreateXcbSurfaceKHR)vkGetInstanceProcAddr(myInstance, "vkCreateXcbSurfaceKHR");
		assert(vkCreateXcbSurfaceKHR != nullptr);
		CHECK_VK(vkCreateXcbSurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &outSurface));
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
		VkXlibSurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.dpy = nullptr; //?;
		surfaceCreateInfo.window = nullptr; //?;
		auto vkCreateXlibSurfaceKHR =
			(PFN_vkCreateXlibSurfaceKHR)vkGetInstanceProcAddr(myInstance, "vkCreateXlibSurfaceKHR");
		assert(vkCreateXlibSurfaceKHR != nullptr);
		CHECK_VK(vkCreateXlibSurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &outSurface));
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
		VkWaylandSurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.wl_display = nullptr; //?;
		surfaceCreateInfo.wl_surface = nullptr; //?;
		auto vkCreateWaylandSurfaceKHR = (PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr(
			myInstance, "vkCreateWaylandSurfaceKHR");
		assert(vkCreateWaylandSurfaceKHR != nullptr);
		CHECK_VK(vkCreateWaylandSurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &outSurface));
#endif
#endif
	}

	void createDevice(VkSurfaceKHR surface)
	{
		uint32_t deviceCount = 0;
		CHECK_VK(vkEnumeratePhysicalDevices(myInstance, &deviceCount, nullptr));
		if (deviceCount == 0)
			throw std::runtime_error("failed to find GPUs with Vulkan support!");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		CHECK_VK(vkEnumeratePhysicalDevices(myInstance, &deviceCount, devices.data()));

		for (const auto& device : devices)
		{
			myQueueFamilyIndex = isDeviceSuitable(myInstance, surface, device);
			if (myQueueFamilyIndex >= 0)
			{
				myPhysicalDevice = device;
				break;
			}
		}

		if (myPhysicalDevice == VK_NULL_HANDLE)
			throw std::runtime_error("failed to find a suitable GPU!");

		const float graphicsQueuePriority = 1.0f;

		VkDeviceQueueCreateInfo queueCreateInfo = {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = myQueueFamilyIndex;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &graphicsQueuePriority;

		VkPhysicalDeviceFeatures deviceFeatures = {};
		deviceFeatures.samplerAnisotropy = VK_TRUE;

		uint32_t deviceExtensionCount;
		vkEnumerateDeviceExtensionProperties(myPhysicalDevice, nullptr, &deviceExtensionCount,
											 nullptr);

		std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
		vkEnumerateDeviceExtensionProperties(myPhysicalDevice, nullptr, &deviceExtensionCount,
											 availableDeviceExtensions.data());

		assert(std::find_if(availableDeviceExtensions.begin(), availableDeviceExtensions.end(),
							[](const VkExtensionProperties& extension) {
								return strcmp(extension.extensionName, "VK_KHR_swapchain") == 0;
							}) != availableDeviceExtensions.end());

		std::vector<const char*> deviceExtensions;
		for (const auto& extension : availableDeviceExtensions)
		{
#if defined(__APPLE__)
			if (strcmp(extension.extensionName, "VK_MVK_moltenvk") == 0 ||
				strcmp(extension.extensionName, "VK_KHR_surface") == 0 ||
				strcmp(extension.extensionName, "VK_MVK_macos_surface") == 0)
				continue;
#endif

			deviceExtensions.push_back(extension.extensionName);
		}

		std::sort(deviceExtensions.begin(), deviceExtensions.end(),
				  [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

		VkDeviceCreateInfo deviceCreateInfo = {};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
		deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
		deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

		CHECK_VK(vkCreateDevice(myPhysicalDevice, &deviceCreateInfo, nullptr, &myDevice));

		vkGetDeviceQueue(myDevice, myQueueFamilyIndex, 0, &myQueue);
	}

	void createAllocator()
	{
		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = myPhysicalDevice;
		allocatorInfo.device = myDevice;
		vmaCreateAllocator(&allocatorInfo, &myAllocator);
	}

	void createDescriptorPool()
	{
		constexpr uint32_t maxDescriptorCount = 1000;
		VkDescriptorPoolSize poolSizes[] = {
			{VK_DESCRIPTOR_TYPE_SAMPLER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, maxDescriptorCount},
			{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxDescriptorCount}};

		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = (uint32_t)sizeof_array(poolSizes);
		poolInfo.pPoolSizes = poolSizes;
		poolInfo.maxSets = maxDescriptorCount * static_cast<uint32_t>(sizeof_array(poolSizes));
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

		CHECK_VK(vkCreateDescriptorPool(myDevice, &poolInfo, nullptr, &myDescriptorPool));
	}

	void createDescriptorSetLayout()
	{
		VkDescriptorSetLayoutBinding uboLayoutBinding = {};
		uboLayoutBinding.binding = 0;
		uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		uboLayoutBinding.descriptorCount = 1;
		uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		uboLayoutBinding.pImmutableSamplers = nullptr;

		VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
		samplerLayoutBinding.binding = 1;
		samplerLayoutBinding.descriptorCount = 1;
		samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		samplerLayoutBinding.pImmutableSamplers = &mySampler;

		std::array<VkDescriptorSetLayoutBinding, 2> bindings = {uboLayoutBinding,
																samplerLayoutBinding};

		VkDescriptorSetLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
		layoutInfo.pBindings = bindings.data();

		CHECK_VK(
			vkCreateDescriptorSetLayout(myDevice, &layoutInfo, nullptr, &myDescriptorSetLayout));
	}

	void createDescriptorSet()
	{
		VkDescriptorSetLayout layouts[] = {myDescriptorSetLayout};
		VkDescriptorSetAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = myDescriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = layouts;

		CHECK_VK(vkAllocateDescriptorSets(myDevice, &allocInfo, &myDescriptorSet));

		VkDescriptorBufferInfo bufferInfo = {};
		bufferInfo.buffer = myUniformBuffer;
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(UniformBufferObject);

		VkDescriptorImageInfo imageInfo = {};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = myImageView;
		imageInfo.sampler = mySampler;

		std::array<VkWriteDescriptorSet, 2> descriptorWrites = {};
		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].dstSet = myDescriptorSet;
		descriptorWrites[0].dstBinding = 0;
		descriptorWrites[0].dstArrayElement = 0;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].pBufferInfo = &bufferInfo;
		descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[1].dstSet = myDescriptorSet;
		descriptorWrites[1].dstBinding = 1;
		descriptorWrites[1].dstArrayElement = 0;
		descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrites[1].descriptorCount = 1;
		descriptorWrites[1].pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(myDevice, static_cast<uint32_t>(descriptorWrites.size()),
							   descriptorWrites.data(), 0, nullptr);
	}

	void createGraphicsPipeline()
	{
		auto vsCode =
			readSPIRVFile((myResourcePath + std::string("shaders/spir-v/vert.spv")).c_str());

		VkShaderModuleCreateInfo vsCreateInfo = {};
		vsCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		vsCreateInfo.codeSize = vsCode.size();
		vsCreateInfo.pCode = reinterpret_cast<const uint32_t*>(vsCode.data());

		VkShaderModule vsModule;
		CHECK_VK(vkCreateShaderModule(myDevice, &vsCreateInfo, nullptr, &vsModule));

		VkPipelineShaderStageCreateInfo vsStageInfo = {};
		vsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		vsStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
		vsStageInfo.module = vsModule;
		vsStageInfo.pName = "main";

		auto fsCode =
			readSPIRVFile((myResourcePath + std::string("shaders/spir-v/frag.spv")).c_str());

		VkShaderModuleCreateInfo fsCreateInfo = {};
		fsCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		fsCreateInfo.codeSize = fsCode.size();
		fsCreateInfo.pCode = reinterpret_cast<const uint32_t*>(fsCode.data());

		VkShaderModule fsModule;
		CHECK_VK(vkCreateShaderModule(myDevice, &fsCreateInfo, nullptr, &fsModule));

		VkPipelineShaderStageCreateInfo fsStageInfo = {};
		fsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		fsStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		fsStageInfo.module = fsModule;
		fsStageInfo.pName = "main";

		VkPipelineShaderStageCreateInfo shaderStages[] = {vsStageInfo, fsStageInfo};

		auto bindingDescription = Vertex::getBindingDescription();
		auto attributeDescriptions = Vertex::getAttributeDescriptions();

		VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.vertexBindingDescriptionCount = 1;
		vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
		vertexInputInfo.vertexAttributeDescriptionCount =
			static_cast<uint32_t>(attributeDescriptions.size());
		vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		VkViewport viewport = {};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = (float)(myWindowData->Width);
		viewport.height = (float)(myWindowData->Height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor = {};
		scissor.offset = {0, 0};
		scissor.extent = {static_cast<uint32_t>(myWindowData->Width),
						  static_cast<uint32_t>(myWindowData->Height)};

		VkPipelineViewportStateCreateInfo viewportState = {};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.pViewports = &viewport;
		viewportState.scissorCount = 1;
		viewportState.pScissors = &scissor;

		VkPipelineRasterizationStateCreateInfo rasterizer = {};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.depthClampEnable = VK_FALSE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.lineWidth = 1.0f;
		rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
		rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
		rasterizer.depthBiasEnable = VK_FALSE;
		rasterizer.depthBiasConstantFactor = 0.0f;
		rasterizer.depthBiasClamp = 0.0f;
		rasterizer.depthBiasSlopeFactor = 0.0f;

		VkPipelineMultisampleStateCreateInfo multisampling = {};
		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.sampleShadingEnable = VK_FALSE;
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		multisampling.minSampleShading = 1.0f;
		multisampling.pSampleMask = nullptr;
		multisampling.alphaToCoverageEnable = VK_FALSE;
		multisampling.alphaToOneEnable = VK_FALSE;

		VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
											  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_FALSE;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

		VkPipelineColorBlendStateCreateInfo colorBlending = {};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.logicOp = VK_LOGIC_OP_COPY;
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;
		colorBlending.blendConstants[0] = 0.0f;
		colorBlending.blendConstants[1] = 0.0f;
		colorBlending.blendConstants[2] = 0.0f;
		colorBlending.blendConstants[3] = 0.0f;

		// VkPipelineDynamicStateCreateInfo dynamicState = {};
		// dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		// dynamicState.dynamicStateCount = 0;
		// dynamicState.pDynamicStates = nullptr;

		VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &myDescriptorSetLayout;
		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = nullptr;

		CHECK_VK(vkCreatePipelineLayout(myDevice, &pipelineLayoutInfo, nullptr, &myPipelineLayout));

		VkGraphicsPipelineCreateInfo pipelineInfo = {};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = shaderStages;
		pipelineInfo.pVertexInputState = &vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pDepthStencilState = nullptr;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = nullptr;
		pipelineInfo.layout = myPipelineLayout;
		pipelineInfo.renderPass = myWindowData->RenderPass;
		pipelineInfo.subpass = 0;
		pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
		pipelineInfo.basePipelineIndex = -1;

		CHECK_VK(vkCreateGraphicsPipelines(myDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
										   &myGraphicsPipeline));

		vkDestroyShaderModule(myDevice, vsModule, nullptr);
		vkDestroyShaderModule(myDevice, fsModule, nullptr);
	}

	VkCommandBuffer beginSingleTimeCommands()
	{
		VkCommandPool commandPool = myWindowData->Frames[myWindowData->FrameIndex].CommandPool;
		VkCommandBuffer commandBuffer =
			myWindowData->Frames[myWindowData->FrameIndex].CommandBuffer;

		CHECK_VK(vkResetCommandPool(myDevice, commandPool, 0));

		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		CHECK_VK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

		return commandBuffer;
	}

	void endSingleTimeCommands(VkCommandBuffer commandBuffer)
	{
		VkSubmitInfo endInfo = {};
		endInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		endInfo.commandBufferCount = 1;
		endInfo.pCommandBuffers = &commandBuffer;
		CHECK_VK(vkEndCommandBuffer(commandBuffer));
		CHECK_VK(vkQueueSubmit(myQueue, 1, &endInfo, VK_NULL_HANDLE));
		CHECK_VK(vkDeviceWaitIdle(myDevice));
	}

	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
	{
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkBufferCopy copyRegion = {};
		copyRegion.srcOffset = 0;
		copyRegion.dstOffset = 0;
		copyRegion.size = size;
		vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

		endSingleTimeCommands(commandBuffer);
	}

	void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
					  VkBuffer& outBuffer, VmaAllocation& outBufferMemory)
	{
		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? VMA_MEMORY_USAGE_GPU_ONLY
																		: VMA_MEMORY_USAGE_UNKNOWN;
		allocInfo.requiredFlags = flags;
		allocInfo.memoryTypeBits = 0; // memRequirements.memoryTypeBits;

		CHECK_VK(vmaCreateBuffer(myAllocator, &bufferInfo, &allocInfo, &outBuffer, &outBufferMemory,
								 nullptr));
	}

	template <typename T>
	void createDeviceLocalBuffer(const T* bufferData, uint32_t bufferElementCount,
								 VkBufferUsageFlags usage, VkBuffer& outBuffer,
								 VmaAllocation& outBufferMemory)
	{
		assert(bufferData != nullptr);
		assert(bufferElementCount > 0);
		size_t bufferSize = sizeof(T) * bufferElementCount;

		// todo: use staging buffer pool, or use scratchpad memory
		VkBuffer stagingBuffer;
		VmaAllocation stagingBufferMemory;
		createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					 stagingBuffer, stagingBufferMemory);

		void* data;
		CHECK_VK(vmaMapMemory(myAllocator, stagingBufferMemory, &data));
		memcpy(data, bufferData, bufferSize);
		vmaUnmapMemory(myAllocator, stagingBufferMemory);

		createBuffer(bufferSize, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer, outBufferMemory);

		copyBuffer(stagingBuffer, outBuffer, bufferSize);

		vmaDestroyBuffer(myAllocator, stagingBuffer, stagingBufferMemory);
	}

	void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout,
							   VkImageLayout newLayout)
	{
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		VkPipelineStageFlags sourceStage;
		VkPipelineStageFlags destinationStage;
		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
				 newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}
		else
		{
			assert(false); // not implemented yet
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = 0;
		}

		vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0,
							 nullptr, 1, &barrier);

		endSingleTimeCommands(commandBuffer);
	}

	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
	{
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkBufferImageCopy region = {};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = {0, 0, 0};
		region.imageExtent = {width, height, 1};

		vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   1, &region);

		endSingleTimeCommands(commandBuffer);
	}

	void createImage2D(uint32_t width, uint32_t height, uint32_t pixelSizeBytes,
					   VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags,
					   VkImage& outImage, VmaAllocation& outImageMemory)
	{
		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = usage;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.flags = 0;

		VmaAllocationCreateInfo allocInfo = {};
		allocInfo.usage = (memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
							  ? VMA_MEMORY_USAGE_GPU_ONLY
							  : VMA_MEMORY_USAGE_UNKNOWN;
		allocInfo.requiredFlags = memoryFlags;
		allocInfo.memoryTypeBits = 0; // memRequirements.memoryTypeBits;

		VmaAllocationInfo outAllocInfo = {};
		CHECK_VK(vmaCreateImage(myAllocator, &imageInfo, &allocInfo, &outImage, &outImageMemory,
								&outAllocInfo));
	}

	template <typename T>
	void createDeviceLocalImage2D(const T* imageData, uint32_t width, uint32_t height,
								  uint32_t pixelSizeBytes, VkImageUsageFlags usage,
								  VkImage& outImage, VmaAllocation& outImageMemory)
	{
		VkDeviceSize imageSize = width * height * pixelSizeBytes;

		VkBuffer stagingBuffer;
		VmaAllocation stagingBufferMemory;
		createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					 stagingBuffer, stagingBufferMemory);

		void* data;
		CHECK_VK(vmaMapMemory(myAllocator, stagingBufferMemory, &data));
		memcpy(data, imageData, imageSize);
		vmaUnmapMemory(myAllocator, stagingBufferMemory);

		createImage2D(width, height, pixelSizeBytes, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
					  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outImage, outImageMemory);

		transitionImageLayout(outImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
							  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(stagingBuffer, outImage, width, height);
		transitionImageLayout(outImage, VK_FORMAT_R8G8B8A8_UNORM,
							  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(myAllocator, stagingBuffer, stagingBufferMemory);
	}

	VkImageView createImageView2D(VkImage image, VkFormat format)
	{
		VkImageView imageView;
		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;
		viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

		CHECK_VK(vkCreateImageView(myDevice, &viewInfo, nullptr, &imageView));

		return imageView;
	}

	void createTextureSampler()
	{
		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.anisotropyEnable = VK_TRUE;
		samplerInfo.maxAnisotropy = 16;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;

		CHECK_VK(vkCreateSampler(myDevice, &samplerInfo, nullptr, &mySampler));
	}

	void initVulkan(void* window, VkSurfaceKHR& outSurface)
	{
		createInstance();
		createDebugCallback();
		createSurface(window, outSurface);
		createDevice(outSurface);
		createAllocator();
		createDescriptorPool();
	}

	void initIMGUI(int height, int width, VkSurfaceKHR surface)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		// io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		// io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

		ImFontConfig config;
		config.OversampleH = 2;
		config.OversampleV = 2;
		config.PixelSnapH = false;

		io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
		myFonts.push_back(io.Fonts->AddFontFromFileTTF(
			(myResourcePath + std::string("fonts/Cousine-Regular.ttf")).c_str(), 16.0f, &config));
		myFonts.push_back(io.Fonts->AddFontFromFileTTF(
			(myResourcePath + std::string("fonts/DroidSans.ttf")).c_str(), 16.0f, &config));
		myFonts.push_back(io.Fonts->AddFontFromFileTTF(
			(myResourcePath + std::string("fonts/Karla-Regular.ttf")).c_str(), 16.0f, &config));
		myFonts.push_back(io.Fonts->AddFontFromFileTTF(
			(myResourcePath + std::string("fonts/ProggyClean.ttf")).c_str(), 16.0f, &config));
		myFonts.push_back(io.Fonts->AddFontFromFileTTF(
			(myResourcePath + std::string("fonts/ProggyTiny.ttf")).c_str(), 16.0f, &config));
		myFonts.push_back(io.Fonts->AddFontFromFileTTF(
			(myResourcePath + std::string("fonts/Roboto-Medium.ttf")).c_str(), 16.0f, &config));

		// Setup style
		ImGui::StyleColorsClassic();
		io.FontDefault = myFonts.back();

		const VkFormat requestSurfaceImageFormat[] = {
			VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
			VK_FORMAT_R8G8B8_UNORM };
		const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
		VkSurfaceFormatKHR surfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
			myPhysicalDevice, surface, requestSurfaceImageFormat,
			static_cast<uint32_t>(sizeof_array(requestSurfaceImageFormat)),
			requestSurfaceColorSpace);

		VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
		presentMode = ImGui_ImplVulkanH_SelectPresentMode(
			myPhysicalDevice, surface, &presentMode, 1);

		myWindowData = std::make_unique<ImGui_ImplVulkanH_WindowData>(presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? 3 : 2);

		myWindowData->SurfaceFormat = surfaceFormat;
		myWindowData->Surface = surface;
		myWindowData->PresentMode = presentMode;

		myWindowData->ClearValue.color.float32[0] = 0.4f;
		myWindowData->ClearValue.color.float32[1] = 0.4f;
		myWindowData->ClearValue.color.float32[2] = 0.5f;
		myWindowData->ClearValue.color.float32[3] = 1.0f;

		// Create SwapChain, RenderPass, Framebuffer, etc.
		ImGui_ImplVulkanH_CreateWindowDataSwapChainAndFramebuffer(
			myPhysicalDevice, myDevice, myWindowData.get(), myAllocator->GetAllocationCallbacks(),
			width, height, true);
		ImGui_ImplVulkanH_CreateWindowDataCommandBuffers(myPhysicalDevice, myDevice,
			myQueueFamilyIndex, myWindowData.get(),
			myAllocator->GetAllocationCallbacks());

		// Setup Vulkan binding
		ImGui_ImplVulkan_InitInfo initInfo = {};
		initInfo.Instance = myInstance;
		initInfo.PhysicalDevice = myPhysicalDevice;
		initInfo.Device = myDevice;
		initInfo.QueueFamily = myQueueFamilyIndex;
		initInfo.Queue = myQueue;
		initInfo.PipelineCache = VK_NULL_HANDLE;
		initInfo.DescriptorPool = myDescriptorPool;
		initInfo.Allocator = myAllocator->GetAllocationCallbacks();
		initInfo.CheckVkResultFn = CHECK_VK;
		ImGui_ImplVulkan_Init(&initInfo, myWindowData->RenderPass);

		// Upload Fonts
		{
			VkCommandBuffer commandBuffer = beginSingleTimeCommands();
			ImGui_ImplVulkan_CreateFontsTexture(commandBuffer);
			endSingleTimeCommands(commandBuffer);
			ImGui_ImplVulkan_InvalidateFontUploadObjects();
		}

		// vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
		myWindowData->FrameIndex = myWindowData->FrameCount - 1;
	}

	void checkFlipOrPresentResult(VkResult result)
	{
		if (result == VK_SUBOPTIMAL_KHR)
			return; // not much we can do
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
			ImGui_ImplVulkanH_CreateWindowDataSwapChainAndFramebuffer(
				myPhysicalDevice, myDevice, myWindowData.get(),
				myAllocator->GetAllocationCallbacks(), myWindowData->Width, myWindowData->Height);
		else if (result != VK_SUCCESS)
			throw std::runtime_error("failed to flip swap chain image!");
	}

	void updateUniformBuffer()
	{
		static auto start = std::chrono::high_resolution_clock::now();
		auto now = std::chrono::high_resolution_clock::now();
		constexpr float period = 5.0f;
		float t = fmod(std::chrono::duration<float>(now - start).count(), period);
		float s = smootherstep(
			smoothstep(clamp(ramp(t < (0.5f * period) ? t : period - t, 0, 0.5f * period), 0, 1)));

		UniformBufferObject ubo = {};
		ubo.model[0] = {1 * s, 0, 0, 0};
		ubo.model[1] = {0, 1 * s, 0, 0};
		ubo.model[2] = {0, 0, 1 * s, 0};
		ubo.model[3] = {0, 0, 0, 1};
		ubo.view[0] = {1, 0, 0, 0};
		ubo.view[1] = {0, 1, 0, 0};
		ubo.view[2] = {0, 0, 1, 0};
		ubo.view[3] = {0, 0, 0, 1};
		ubo.proj[0] = {1, 0, 0, 0};
		ubo.proj[1] = {0, 1, 0, 0};
		ubo.proj[2] = {0, 0, 1, 0};
		ubo.proj[3] = {0, 0, 0, 1};

		void* data;
		CHECK_VK(vmaMapMemory(myAllocator, myUniformBufferMemory, &data));
		memcpy(data, &ubo, sizeof(ubo));
		vmaUnmapMemory(myAllocator, myUniformBufferMemory);
	}

	void submitFrame()
	{
		ImGui_ImplVulkanH_FrameData* oldFrame = &myWindowData->Frames[myWindowData->FrameIndex];
		VkSemaphore& imageAquiredSemaphore = oldFrame->ImageAcquiredSemaphore;

		checkFlipOrPresentResult(vkAcquireNextImageKHR(myDevice, myWindowData->Swapchain,
													   UINT64_MAX, imageAquiredSemaphore,
													   VK_NULL_HANDLE, &myWindowData->FrameIndex));
		/*
		{
			VkAcquireNextImageInfoKHR nextImageInfo = {};
			nextImageInfo.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR;
			nextImageInfo.swapchain = myWindowData->Swapchain;
			nextImageInfo.timeout = UINT64_MAX;
			nextImageInfo.semaphore = imageAquiredSemaphore;
			nextImageInfo.fence = VK_NULL_HANDLE;
			nextImageInfo.deviceMask = ?;

			checkFlipOrPresentResult(vkAcquireNextImage2KHR(myDevice, &nextImageInfo,
		&myWindowData->FrameIndex));
		}
		 */

		ImGui_ImplVulkanH_FrameData* newFrame = &myWindowData->Frames[myWindowData->FrameIndex];

		// wait for previous command buffer to be submitted
		{
			CHECK_VK(vkWaitForFences(myDevice, 1, &newFrame->Fence, VK_TRUE, UINT64_MAX));
			CHECK_VK(vkResetFences(myDevice, 1, &newFrame->Fence));
		}

		// begin command buffer
		{
			CHECK_VK(vkResetCommandPool(myDevice, newFrame->CommandPool, 0));
			VkCommandBufferBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			CHECK_VK(vkBeginCommandBuffer(newFrame->CommandBuffer, &info));
		}

		// begin render pass
		{
			VkRenderPassBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			beginInfo.renderPass = myWindowData->RenderPass;
			beginInfo.framebuffer = myWindowData->Framebuffer[myWindowData->FrameIndex];
			beginInfo.renderArea.extent.width = myWindowData->Width;
			beginInfo.renderArea.extent.height = myWindowData->Height;
			beginInfo.clearValueCount = 1;
			beginInfo.pClearValues = &myWindowData->ClearValue;
			vkCmdBeginRenderPass(newFrame->CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
		}

		// draw animated image
		{
			VkBuffer vertexBuffers[] = {myVertexBuffer};
			VkDeviceSize offsets[] = {0};

			vkCmdBindDescriptorSets(newFrame->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
									myPipelineLayout, 0, 1, &myDescriptorSet, 0, nullptr);
			vkCmdBindPipeline(newFrame->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
							  myGraphicsPipeline);
			vkCmdBindVertexBuffers(newFrame->CommandBuffer, 0, 1, vertexBuffers, offsets);
			vkCmdBindIndexBuffer(newFrame->CommandBuffer, myIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
			vkCmdDrawIndexed(newFrame->CommandBuffer,
							 static_cast<uint32_t>(sizeof_array(ourIndices)), 1, 0, 0, 0);
		}

		// Record Imgui Draw Data and draw funcs into command buffer
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), newFrame->CommandBuffer);

		// end render pass
		vkCmdEndRenderPass(newFrame->CommandBuffer);

		// Submit command buffer
		{
			VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			VkSubmitInfo submitInfo = {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = &imageAquiredSemaphore;
			submitInfo.pWaitDstStageMask = &waitStage;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &newFrame->CommandBuffer;
			submitInfo.signalSemaphoreCount = 1;
			submitInfo.pSignalSemaphores = &newFrame->RenderCompleteSemaphore;

			CHECK_VK(vkEndCommandBuffer(newFrame->CommandBuffer));
			CHECK_VK(vkQueueSubmit(myQueue, 1, &submitInfo, newFrame->Fence));
		}
	}

	void presentFrame()
	{
		ImGui_ImplVulkanH_FrameData* fd = &myWindowData->Frames[myWindowData->FrameIndex];
		VkPresentInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &fd->RenderCompleteSemaphore;
		info.swapchainCount = 1;
		info.pSwapchains = &myWindowData->Swapchain;
		info.pImageIndices = &myWindowData->FrameIndex;
		checkFlipOrPresentResult(vkQueuePresentKHR(myQueue, &info));
	}

	void cleanup()
	{
		CHECK_VK(vkDeviceWaitIdle(myDevice));

		ImGui_ImplVulkanH_DestroyWindowData(myInstance, myDevice, myWindowData.get(),
											myAllocator->GetAllocationCallbacks());
		ImGui_ImplVulkan_Shutdown();

		ImGui::DestroyContext();

		vmaDestroyBuffer(myAllocator, myUniformBuffer, myUniformBufferMemory);
		vmaDestroyBuffer(myAllocator, myVertexBuffer, myVertexBufferMemory);
		vmaDestroyBuffer(myAllocator, myIndexBuffer, myIndexBufferMemory);
		vmaDestroyImage(myAllocator, myImage, myImageMemory);
		vkDestroyImageView(myDevice, myImageView, nullptr);
		vkDestroySampler(myDevice, mySampler, nullptr);

		vkDestroyDescriptorSetLayout(myDevice, myDescriptorSetLayout, nullptr);
		vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);

		vmaDestroyAllocator(myAllocator);

		vkDestroyDevice(myDevice, nullptr);

		auto vkDestroyDebugReportCallbackEXT =
			(PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
				myInstance, "vkDestroyDebugReportCallbackEXT");
		assert(vkDestroyDebugReportCallbackEXT != nullptr);
		vkDestroyDebugReportCallbackEXT(myInstance, myDebugCallback, nullptr);

		vkDestroyInstance(myInstance, nullptr);
	}

	static uint32_t findMemoryType(VkPhysicalDevice device, uint32_t typeFilter,
								   VkMemoryPropertyFlags properties)
	{
		VkPhysicalDeviceMemoryProperties memProperties;
		vkGetPhysicalDeviceMemoryProperties(device, &memProperties);

		for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
			if ((typeFilter & (1 << i)) &&
				(memProperties.memoryTypes[i].propertyFlags & properties))
				return i;

		return 0;
	}

	static int isDeviceSuitable(VkInstance instance, VkSurfaceKHR surface, VkPhysicalDevice device)
	{
		VkPhysicalDeviceProperties deviceProperties;
		vkGetPhysicalDeviceProperties(device, &deviceProperties);

		VkPhysicalDeviceFeatures deviceFeatures;
		vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

		struct SwapChainInfo
		{
			VkSurfaceCapabilitiesKHR capabilities;
			std::vector<VkSurfaceFormatKHR> formats;
			std::vector<VkPresentModeKHR> presentModes;
		} swapChainInfo;

		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &swapChainInfo.capabilities);

		uint32_t formatCount;
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
		if (formatCount != 0)
		{
			swapChainInfo.formats.resize(formatCount);
			vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
												 swapChainInfo.formats.data());
		}

		assert(!swapChainInfo.formats.empty());

		uint32_t presentModeCount;
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
		if (presentModeCount != 0)
		{
			swapChainInfo.presentModes.resize(presentModeCount);
			vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
													  swapChainInfo.presentModes.data());
		}

		assert(!swapChainInfo.presentModes.empty());

		// if (deviceProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && // lets run on
		// the integrated for now to save battery
		//    deviceFeatures.samplerAnisotropy)
		{
			uint32_t queueFamilyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

			std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
													 queueFamilies.data());

			for (int i = 0; i < queueFamilies.size(); i++)
			{
				const auto& queueFamily = queueFamilies[i];

				VkBool32 presentSupport = false;
				vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

				if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT &&
					presentSupport)
				{
					return i;
				}
			}
		}

		return -1;
	}

	VkInstance myInstance = VK_NULL_HANDLE;
	VkDebugReportCallbackEXT myDebugCallback = VK_NULL_HANDLE;
	VkPhysicalDevice myPhysicalDevice = VK_NULL_HANDLE;
	VkDevice myDevice = VK_NULL_HANDLE;
	VmaAllocator myAllocator = VK_NULL_HANDLE;
	int myQueueFamilyIndex = -1;
	VkQueue myQueue = VK_NULL_HANDLE;
	VkDescriptorPool myDescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSetLayout myDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet myDescriptorSet = VK_NULL_HANDLE;
	VkPipelineLayout myPipelineLayout = VK_NULL_HANDLE;
	VkPipeline myGraphicsPipeline = VK_NULL_HANDLE;
	VkBuffer myVertexBuffer = VK_NULL_HANDLE;
	VmaAllocation myVertexBufferMemory = VK_NULL_HANDLE;
	VkBuffer myIndexBuffer = VK_NULL_HANDLE;
	VmaAllocation myIndexBufferMemory = VK_NULL_HANDLE;
	VkImage myImage = VK_NULL_HANDLE;
	VmaAllocation myImageMemory = VK_NULL_HANDLE;
	VkImageView myImageView = VK_NULL_HANDLE;
	VkSampler mySampler = VK_NULL_HANDLE;
	VkBuffer myUniformBuffer = VK_NULL_HANDLE;
	VmaAllocation myUniformBufferMemory = VK_NULL_HANDLE;

	std::unique_ptr<ImGui_ImplVulkanH_WindowData> myWindowData;
	std::vector<ImFont*> myFonts;

	const std::string myResourcePath;

	static const Vertex ourVertices[4];
	static const uint16_t ourIndices[6];
};

const Vertex VulkanApplication::ourVertices[] = {{{-1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
												 {{1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
												 {{1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
												 {{-1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}}};

const uint16_t VulkanApplication::ourIndices[] = {0, 1, 2, 2, 3, 0};

VulkanApplication* theApp = nullptr;

int vkapp_create(void* view, int width, int height, const char* resourcePath)
{
	assert(view != nullptr);
	assert(width > 0);
	assert(height > 0);
	assert(resourcePath != nullptr);
	assert(theApp == nullptr);

	try
	{
		static const char* VK_LOADER_DEBUG_STR = "VK_LOADER_DEBUG";
		if (char* vkLoaderDebug = getenv(VK_LOADER_DEBUG_STR))
			std::cout << VK_LOADER_DEBUG_STR << "=" << vkLoaderDebug << std::endl;

		static const char* VK_LAYER_PATH_STR = "VK_LAYER_PATH";
		if (char* vkLayerPath = getenv(VK_LAYER_PATH_STR))
			std::cout << VK_LAYER_PATH_STR << "=" << vkLayerPath << std::endl;

		static const char* VK_ICD_FILENAMES_STR = "VK_ICD_FILENAMES";
		if (char* vkIcdFilenames = getenv(VK_ICD_FILENAMES_STR))
			std::cout << VK_ICD_FILENAMES_STR << "=" << vkIcdFilenames << std::endl;

		theApp = new VulkanApplication(view, width, height, resourcePath);
	}
	catch (const std::runtime_error& e)
	{
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

void vkapp_destroy()
{
	assert(theApp != nullptr);

	delete theApp;
}

void vkapp_draw()
{
	assert(theApp != nullptr);

	theApp->draw();
}
