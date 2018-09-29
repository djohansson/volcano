#include "Volcano.h"
#include "Core.h"
#include "Math.h"
#include "VkUtil.h"

#include <volk.h>

#if defined(VOLCANO_USE_GLFW)
#	define GLFW_INCLUDE_NONE
#	include <GLFW/glfw3.h>
#endif

#if defined(_WIN32)
#	define NOMINMAX
#	include <wtypes.h>
#	include <vulkan/vulkan_win32.h>
#elif defined(__APPLE__)
#	include <vulkan/vulkan_macos.h>
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
#		include <X11/Xutil.h>
#		include <vulkan/vulkan_xcb.h>
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
#		include <X11/Xutil.h>
#		include <vulkan/vulkan_xlib.h>
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
#		include <linux/input.h>
#		include <vulkan/vulkan_wayland.h>
#	endif
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

//#define GLM_FORCE_MESSAGES
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_interpolation.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <imgui.h>
#include <examples/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#ifndef __APPLE__
#include <execution>
#endif
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>


class VulkanApplication
{
public:
	VulkanApplication(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath, bool /*verbose*/)
		: myResourcePath(resourcePath)
		, myCommandBufferCount(std::thread::hardware_concurrency())
	{
		createInstance();
		createDebugCallback();
		
		createSurface(view);
		createDevice();

		createAllocator();

		createTextureSampler();

		createDescriptorPool();
		createDescriptorSetLayout();

		createFrameResources(framebufferWidth, framebufferHeight);

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

			createDeviceLocalImage2D(data, x, y, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, myImage,
				myImageMemory);

			stbi_image_free(data);

			myImageView = createImageView2D(myImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
		}

		// create uniform buffer
		createBuffer(
			NX * NY * sizeof(UniformBufferObject),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			myUniformBuffer,
			myUniformBufferMemory);
		
		createDescriptorSet();

		float dpiScaleX = static_cast<float>(framebufferWidth) / windowWidth;
		float dpiScaleY = static_cast<float>(framebufferHeight) / windowHeight;

		initIMGUI(dpiScaleX, dpiScaleY);
	}

	~VulkanApplication()
	{
		CHECK_VK(myDeviceTable.vkDeviceWaitIdle(myDevice));

		cleanup();
	}

	void draw()
	{
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();

		ImGui::ShowDemoWindow();

		{
			ImGui::Begin("Render Options");
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

		updateUniformBuffers();
		submitFrame();
		presentFrame();
	}

	void resize(int width, int height)
	{
		CHECK_VK(myDeviceTable.vkDeviceWaitIdle(myDevice));
		
		cleanupFrameResources();

		createFrameResources(width, height);
	}

private:
	void createInstance()
	{
		CHECK_VK(volkInitialize());

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

		const char* enabledLayerNames[] = { "VK_LAYER_LUNARG_standard_validation" };

		uint32_t instanceExtensionCount;
		vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

		std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
		vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount,
			availableInstanceExtensions.data());

		std::vector<const char*> instanceExtensions(instanceExtensionCount);
		for (uint32_t i = 0; i < instanceExtensionCount; i++)
		{
			instanceExtensions[i] = availableInstanceExtensions[i].extensionName;
			std::cout << instanceExtensions[i] << "\n";
		}

		std::sort(instanceExtensions.begin(), instanceExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

		std::vector<const char*> requiredExtensions = {
			// must be sorted lexicographically for std::includes to work!
			"VK_EXT_debug_report", 
			"VK_KHR_surface",
#if defined(_WIN32)
			"VK_KHR_win32_surface",
#elif defined(__APPLE__)
			"VK_MVK_macos_surface",
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
			"VK_KHR_xcb_surface",
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
			"VK_KHR_xlib_surface",
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
			"VK_KHR_wayland_surface",
#	else // default to xcb
			"VK_KHR_xcb_surface",
#	endif
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
		info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
		info.ppEnabledExtensionNames = requiredExtensions.data();

		CHECK_VK(vkCreateInstance(&info, NULL, &myInstance));

		volkLoadInstance(myInstance);
	}

	void createDebugCallback()
	{
		VkDebugReportCallbackCreateInfoEXT debugCallbackInfo = {};
		debugCallbackInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
		debugCallbackInfo.flags =
			/* VK_DEBUG_REPORT_INFORMATION_BIT_EXT |*/ VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
			VK_DEBUG_REPORT_DEBUG_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT |
			VK_DEBUG_REPORT_WARNING_BIT_EXT;

		debugCallbackInfo.pfnCallback = static_cast<PFN_vkDebugReportCallbackEXT>(
			[](VkDebugReportFlagsEXT /*flags*/, VkDebugReportObjectTypeEXT /*objectType*/, uint64_t /*object*/,
				size_t /*location*/, int32_t /*messageCode*/, const char* layerPrefix, const char* message,
				void* /*userData*/) -> VkBool32 {
			std::cout << layerPrefix << ": " << message << std::endl;
			return VK_FALSE;
		});

		CHECK_VK(vkCreateDebugReportCallbackEXT(myInstance, &debugCallbackInfo, nullptr,
			&myDebugCallback));
	}

	void createSurface(void* view)
	{
#if defined(VOLCANO_USE_GLFW)
		CHECK_VK(glfwCreateWindowSurface(myInstance, reinterpret_cast<GLFWwindow*>(view), nullptr,
			&mySurface));
#elif defined(_WIN32)
		VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.hwnd = (HWND)view;
		surfaceCreateInfo.hinstance = GetModuleHandle(nullptr);
		auto vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(
			myInstance, "vkCreateWin32SurfaceKHR");
		assert(vkCreateWin32SurfaceKHR != nullptr);
		CHECK_VK(vkCreateWin32SurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &mySurface));
#elif defined(__APPLE__)
		VkMacOSSurfaceCreateInfoMVK surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.pView = view;
		auto vkCreateMacOSSurfaceMVK = (PFN_vkCreateMacOSSurfaceMVK)vkGetInstanceProcAddr(
			myInstance, "vkCreateMacOSSurfaceMVK");
		assert(vkCreateMacOSSurfaceMVK != nullptr);
		CHECK_VK(vkCreateMacOSSurfaceMVK(myInstance, &surfaceCreateInfo, nullptr, &mySurface));
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
		VkXcbSurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.connection = nullptr; //?;
		surfaceCreateInfo.window = nullptr;		//?;
		auto vkCreateXcbSurfaceKHR =
			(PFN_vkCreateXcbSurfaceKHR)vkGetInstanceProcAddr(myInstance, "vkCreateXcbSurfaceKHR");
		assert(vkCreateXcbSurfaceKHR != nullptr);
		CHECK_VK(vkCreateXcbSurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &mySurface));
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
		VkXlibSurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.dpy = nullptr;	//?;
		surfaceCreateInfo.window = nullptr; //?;
		auto vkCreateXlibSurfaceKHR =
			(PFN_vkCreateXlibSurfaceKHR)vkGetInstanceProcAddr(myInstance, "vkCreateXlibSurfaceKHR");
		assert(vkCreateXlibSurfaceKHR != nullptr);
		CHECK_VK(vkCreateXlibSurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &mySurface));
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
		VkWaylandSurfaceCreateInfoKHR surfaceCreateInfo = {};
		surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
		surfaceCreateInfo.flags = 0;
		surfaceCreateInfo.wl_display = nullptr; //?;
		surfaceCreateInfo.wl_surface = nullptr; //?;
		auto vkCreateWaylandSurfaceKHR = (PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr(
			myInstance, "vkCreateWaylandSurfaceKHR");
		assert(vkCreateWaylandSurfaceKHR != nullptr);
		CHECK_VK(vkCreateWaylandSurfaceKHR(myInstance, &surfaceCreateInfo, nullptr, &mySurface));
#	endif
#endif
	}

	void createDevice()
	{
		uint32_t deviceCount = 0;
		CHECK_VK(vkEnumeratePhysicalDevices(myInstance, &deviceCount, nullptr));
		if (deviceCount == 0)
			throw std::runtime_error("failed to find GPUs with Vulkan support!");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		CHECK_VK(vkEnumeratePhysicalDevices(myInstance, &deviceCount, devices.data()));

		for (const auto& device : devices)
		{
			SwapChainInfo swapChain;
			myQueueFamilyIndex = isDeviceSuitable(mySurface, device, swapChain);
			if (myQueueFamilyIndex >= 0)
			{
				myPhysicalDevice = device;

				const VkFormat requestSurfaceImageFormat[] =
				{
					VK_FORMAT_B8G8R8A8_UNORM,
					VK_FORMAT_R8G8B8A8_UNORM,
					VK_FORMAT_B8G8R8_UNORM,
					VK_FORMAT_R8G8B8_UNORM
				};
				const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
				const VkPresentModeKHR requestPresentMode[] =
				{
					VK_PRESENT_MODE_MAILBOX_KHR,
					VK_PRESENT_MODE_FIFO_RELAXED_KHR,
					VK_PRESENT_MODE_FIFO_KHR,
					VK_PRESENT_MODE_IMMEDIATE_KHR
				};

				// Request several formats, the first found will be used 
				// If none of the requested image formats could be found, use the first available
				mySurfaceFormat = swapChain.formats[0];
				for (uint32_t request_i = 0; request_i < sizeof_array(requestSurfaceImageFormat); request_i++)
				{
					VkSurfaceFormatKHR requestedFormat = { requestSurfaceImageFormat[request_i], requestSurfaceColorSpace };
					auto formatIt = std::find(swapChain.formats.begin(), swapChain.formats.end(), requestedFormat);
					if (formatIt != swapChain.formats.end())
					{
						mySurfaceFormat = *formatIt;
						break;
					}
				}

				// Request a certain mode and confirm that it is available. If not use VK_PRESENT_MODE_FIFO_KHR which is mandatory
				myPresentMode = VK_PRESENT_MODE_FIFO_KHR;
				myBufferCount = 2;
				for (uint32_t request_i = 0; request_i < sizeof_array(requestPresentMode); request_i++)
				{
					auto modeIt = std::find(swapChain.presentModes.begin(), swapChain.presentModes.end(), requestPresentMode[request_i]);
					if (modeIt != swapChain.presentModes.end())
					{
						myPresentMode = *modeIt;
						
						if (myPresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
							myBufferCount = 3;

						break;
					}
				}

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

		std::vector<const char*> deviceExtensions;
        deviceExtensions.reserve(deviceExtensionCount);
		for (uint32_t i = 0; i < deviceExtensionCount; i++)
		{
#if defined(__APPLE__)
			if (strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_moltenvk") == 0 ||
				strcmp(availableDeviceExtensions[i].extensionName, "VK_KHR_surface") == 0 ||
				strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_macos_surface") == 0)
				continue;
#endif
			deviceExtensions.push_back(availableDeviceExtensions[i].extensionName);
			std::cout << deviceExtensions.back() << "\n";
		}

		std::sort(deviceExtensions.begin(), deviceExtensions.end(),
			[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

		std::vector<const char*> requiredDeviceExtensions = {
			// must be sorted lexicographically for std::includes to work!
			"VK_KHR_swapchain"
		};

		assert(
			std::includes(deviceExtensions.begin(), deviceExtensions.end(),
				requiredDeviceExtensions.begin(), requiredDeviceExtensions.end(),
				[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

		VkDeviceCreateInfo deviceCreateInfo = {};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
		deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtensions.size());
		deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

		CHECK_VK(vkCreateDevice(myPhysicalDevice, &deviceCreateInfo, nullptr, &myDevice));

		volkLoadDeviceTable(&myDeviceTable, myDevice);
		VkDeviceTable vk(myDevice, myDeviceTable);
		vk.vkGetDeviceQueue(myQueueFamilyIndex, 0, &myQueue);
	}

	void createAllocator()
	{
		VmaVulkanFunctions functions = {};
		functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
		functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
		functions.vkAllocateMemory = myDeviceTable.vkAllocateMemory;
		functions.vkFreeMemory = myDeviceTable.vkFreeMemory;
		functions.vkMapMemory = myDeviceTable.vkMapMemory;
		functions.vkUnmapMemory = myDeviceTable.vkUnmapMemory;
		functions.vkFlushMappedMemoryRanges = myDeviceTable.vkFlushMappedMemoryRanges;
		functions.vkInvalidateMappedMemoryRanges = myDeviceTable.vkInvalidateMappedMemoryRanges;
		functions.vkBindBufferMemory = myDeviceTable.vkBindBufferMemory;
		functions.vkBindImageMemory = myDeviceTable.vkBindImageMemory;
		functions.vkGetBufferMemoryRequirements = myDeviceTable.vkGetBufferMemoryRequirements;
		functions.vkGetImageMemoryRequirements = myDeviceTable.vkGetImageMemoryRequirements;
		functions.vkCreateBuffer = myDeviceTable.vkCreateBuffer;
		functions.vkDestroyBuffer = myDeviceTable.vkDestroyBuffer;
		functions.vkCreateImage = myDeviceTable.vkCreateImage;
		functions.vkDestroyImage = myDeviceTable.vkDestroyImage;
		functions.vkGetBufferMemoryRequirements2KHR = myDeviceTable.vkGetBufferMemoryRequirements2KHR;
		functions.vkGetImageMemoryRequirements2KHR = myDeviceTable.vkGetImageMemoryRequirements2KHR;
		
		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = myPhysicalDevice;
		allocatorInfo.device = myDevice;
		allocatorInfo.pVulkanFunctions = &functions;
		vmaCreateAllocator(&allocatorInfo, &myAllocator);
	}

	void createDescriptorPool()
	{
		constexpr uint32_t maxDescriptorCount = 1000;
		VkDescriptorPoolSize poolSizes[] =
		{
			{ VK_DESCRIPTOR_TYPE_SAMPLER, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, maxDescriptorCount },
			{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, maxDescriptorCount }
		};

		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = static_cast<uint32_t>(sizeof_array(poolSizes));
		poolInfo.pPoolSizes = poolSizes;
		poolInfo.maxSets = maxDescriptorCount * static_cast<uint32_t>(sizeof_array(poolSizes));
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

		CHECK_VK(myDeviceTable.vkCreateDescriptorPool(myDevice, &poolInfo, nullptr, &myDescriptorPool));
	}

	void createDescriptorSetLayout()
	{
		VkDescriptorSetLayoutBinding uboLayoutBinding = {};
		uboLayoutBinding.binding = 0;
		uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		uboLayoutBinding.descriptorCount = 1;
		uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		uboLayoutBinding.pImmutableSamplers = nullptr;

		VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
		samplerLayoutBinding.binding = 1;
		samplerLayoutBinding.descriptorCount = 1;
		samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		samplerLayoutBinding.pImmutableSamplers = &mySampler;

		std::array<VkDescriptorSetLayoutBinding, 2> bindings = { uboLayoutBinding,
																samplerLayoutBinding };

		VkDescriptorSetLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
		layoutInfo.pBindings = bindings.data();

		CHECK_VK(
			myDeviceTable.vkCreateDescriptorSetLayout(myDevice, &layoutInfo, nullptr, &myDescriptorSetLayout));
	}

	void createDescriptorSet()
	{
		VkDescriptorSetLayout layouts[] = { myDescriptorSetLayout };
		VkDescriptorSetAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = myDescriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = layouts;

		CHECK_VK(myDeviceTable.vkAllocateDescriptorSets(myDevice, &allocInfo, &myDescriptorSet));

		VkDescriptorBufferInfo bufferInfo = {};
		bufferInfo.buffer = myUniformBuffer;
		bufferInfo.offset = 0;
		bufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorImageInfo imageInfo = {};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = myImageView;
		imageInfo.sampler = mySampler;

		std::array<VkWriteDescriptorSet, 2> descriptorWrites = {};
		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].dstSet = myDescriptorSet;
		descriptorWrites[0].dstBinding = 0;
		descriptorWrites[0].dstArrayElement = 0;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].pBufferInfo = &bufferInfo;
		descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[1].dstSet = myDescriptorSet;
		descriptorWrites[1].dstBinding = 1;
		descriptorWrites[1].dstArrayElement = 0;
		descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrites[1].descriptorCount = 1;
		descriptorWrites[1].pImageInfo = &imageInfo;

		myDeviceTable.vkUpdateDescriptorSets(myDevice, static_cast<uint32_t>(descriptorWrites.size()),
			descriptorWrites.data(), 0, nullptr);
	}

	void createRenderPass()
	{
		VkAttachmentDescription colorAttachment = {};
		colorAttachment.format = myWindowData->SurfaceFormat.format;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentDescription depthAttachment = {};
		depthAttachment.format = myDepthFormat;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorAttachmentRef = {};
		colorAttachmentRef.attachment = 0;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthAttachmentRef = {};
		depthAttachmentRef.attachment = 1;
		depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;
		subpass.pDepthStencilAttachment = &depthAttachmentRef;

		VkSubpassDependency dependency = {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		CHECK_VK(myDeviceTable.vkCreateRenderPass(myDevice, &renderPassInfo, nullptr, &myRenderPass));
	}

	void createGraphicsPipeline()
	{
		std::vector<char> vsCode;
		readSPIRVFile((myResourcePath + std::string("shaders/spir-v/vert.spv")).c_str(), vsCode);

		VkShaderModuleCreateInfo vsCreateInfo = {};
		vsCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		vsCreateInfo.codeSize = vsCode.size();
		vsCreateInfo.pCode = reinterpret_cast<const uint32_t*>(vsCode.data());

		VkShaderModule vsModule;
		CHECK_VK(myDeviceTable.vkCreateShaderModule(myDevice, &vsCreateInfo, nullptr, &vsModule));

		VkPipelineShaderStageCreateInfo vsStageInfo = {};
		vsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		vsStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
		vsStageInfo.module = vsModule;
		vsStageInfo.pName = "main";

		std::vector<char> fsCode;
		readSPIRVFile((myResourcePath + std::string("shaders/spir-v/frag.spv")).c_str(), fsCode);

		VkShaderModuleCreateInfo fsCreateInfo = {};
		fsCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		fsCreateInfo.codeSize = fsCode.size();
		fsCreateInfo.pCode = reinterpret_cast<const uint32_t*>(fsCode.data());

		VkShaderModule fsModule;
		CHECK_VK(myDeviceTable.vkCreateShaderModule(myDevice, &fsCreateInfo, nullptr, &fsModule));

		VkPipelineShaderStageCreateInfo fsStageInfo = {};
		fsStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		fsStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		fsStageInfo.module = fsModule;
		fsStageInfo.pName = "main";

		VkPipelineShaderStageCreateInfo shaderStages[] = { vsStageInfo, fsStageInfo };

		auto bindingDescription = SimpleVertex2D::getBindingDescription();
		auto attributeDescriptions = SimpleVertex2D::getAttributeDescriptions();

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
		viewport.width = static_cast<float>(myWindowData->Width);
		viewport.height = static_cast<float>(myWindowData->Height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor = {};
		scissor.offset = { 0, 0 };
		scissor.extent = { static_cast<uint32_t>(myWindowData->Width),
						  static_cast<uint32_t>(myWindowData->Height) };

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
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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

		VkPipelineDepthStencilStateCreateInfo depthStencil = {};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
		depthStencil.depthBoundsTestEnable = VK_FALSE;
		depthStencil.minDepthBounds = 0.0f;
		depthStencil.maxDepthBounds = 1.0f;
		depthStencil.stencilTestEnable = VK_FALSE;
		depthStencil.front = {};
		depthStencil.back = {};

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

		std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = {};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &myDescriptorSetLayout;
		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = nullptr;

		CHECK_VK(myDeviceTable.vkCreatePipelineLayout(myDevice, &pipelineLayoutInfo, nullptr, &myPipelineLayout));

		VkGraphicsPipelineCreateInfo pipelineInfo = {};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = shaderStages;
		pipelineInfo.pVertexInputState = &vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = myPipelineLayout;
		pipelineInfo.renderPass = myWindowData->RenderPass;
		pipelineInfo.subpass = 0;
		pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
		pipelineInfo.basePipelineIndex = -1;

		CHECK_VK(myDeviceTable.vkCreateGraphicsPipelines(myDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
			&myGraphicsPipeline));

		myDeviceTable.vkDestroyShaderModule(myDevice, vsModule, nullptr);
		myDeviceTable.vkDestroyShaderModule(myDevice, fsModule, nullptr);
	}

	VkCommandBuffer beginSingleTimeCommands()
	{
		VkCommandPool commandPool = myWindowData->Frames[myWindowData->FrameIndex].CommandPool;
		VkCommandBuffer commandBuffer =
			myWindowData->Frames[myWindowData->FrameIndex].CommandBuffer;

		CHECK_VK(myDeviceTable.vkResetCommandPool(myDevice, commandPool, 0));

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
		CHECK_VK(myDeviceTable.vkDeviceWaitIdle(myDevice));
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

		if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			if (hasStencilComponent(format))
				barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
		else
		{
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}

		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		VkPipelineStageFlags sourceStage = 0;
		VkPipelineStageFlags destinationStage = 0;

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
			destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; // wrong of accessed by another shader type
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
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
		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &region);

		endSingleTimeCommands(commandBuffer);
	}

	void createImage2D(uint32_t width, uint32_t height, VkFormat format,
		VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags memoryFlags,
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
		imageInfo.format = format;
		imageInfo.tiling = tiling;
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
		VkFormat format, VkImageUsageFlags usage,
		VkImage& outImage, VmaAllocation& outImageMemory)
	{
		uint32_t pixelSizeBytes = getFormatSize(format); // todo
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

		createImage2D(width, height, format, VK_IMAGE_TILING_OPTIMAL, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outImage, outImageMemory);

		transitionImageLayout(outImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(stagingBuffer, outImage, width, height);
		transitionImageLayout(outImage, VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vmaDestroyBuffer(myAllocator, stagingBuffer, stagingBufferMemory);
	}

	VkImageView createImageView2D(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags)
	{
		VkImageView imageView;
		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = aspectFlags;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;
		viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

		CHECK_VK(myDeviceTable.vkCreateImageView(myDevice, &viewInfo, nullptr, &imageView));

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

		CHECK_VK(myDeviceTable.vkCreateSampler(myDevice, &samplerInfo, nullptr, &mySampler));
	}

	void initIMGUI(float dpiScaleX, float dpiScaleY)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		// io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		// io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

		io.DisplayFramebufferScale = ImVec2(dpiScaleX, dpiScaleY);

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

		// Setup Vulkan binding
		ImGui_ImplVulkan_InitInfo initInfo = {};
		initInfo.Instance = myInstance;
		initInfo.PhysicalDevice = myPhysicalDevice;
		initInfo.Device = myDevice;
		initInfo.QueueFamily = myQueueFamilyIndex;
		initInfo.Queue = myQueue;
		initInfo.PipelineCache = VK_NULL_HANDLE;
		initInfo.DescriptorPool = myDescriptorPool;
		initInfo.Allocator = myAllocator;
		initInfo.HostAllocationCallbacks = nullptr;
		initInfo.CheckVkResultFn = CHECK_VK;
		ImGui_ImplVulkan_Init(&initInfo, myWindowData->RenderPass);

		// Upload Fonts
		{
			VkCommandBuffer commandBuffer = beginSingleTimeCommands();
			ImGui_ImplVulkan_CreateFontsTexture(commandBuffer);
			endSingleTimeCommands(commandBuffer);
			ImGui_ImplVulkan_InvalidateFontUploadObjects();
		}
	}

	void createFrameResources(int width, int height)
	{
		myWindowData = std::make_unique<ImGui_ImplVulkanH_WindowData>(myBufferCount);

		// vkAcquireNextImageKHR uses semaphore from last frame -> cant use index 0 for first frame
		myWindowData->FrameIndex = myWindowData->FrameCount - 1;

		myWindowData->SurfaceFormat = mySurfaceFormat;
		myWindowData->Surface = mySurface;
		myWindowData->PresentMode = myPresentMode;

		myWindowData->ClearEnable = false; // we will clear before IMGUI, using myWindowData->ClearValue
		myWindowData->ClearValue.color.float32[0] = 0.4f;
		myWindowData->ClearValue.color.float32[1] = 0.4f;
		myWindowData->ClearValue.color.float32[2] = 0.5f;
		myWindowData->ClearValue.color.float32[3] = 1.0f;

		myDepthFormat = findSupportedFormat(
			myPhysicalDevice,
			{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
			VK_IMAGE_TILING_OPTIMAL,
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

		createImage2D(
			width,
			height,
			myDepthFormat,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			myDepthImage,
			myDepthImageMemory);

		myDepthImageView = createImageView2D(myDepthImage, myDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

		myCommandPools.resize(myCommandBufferCount);
		myCommandBuffers.resize(myCommandBufferCount * myBufferCount);

		// Create SwapChain, RenderPass, Framebuffer, etc.
		ImGui_ImplVulkanH_CreateWindowDataSwapChainAndFramebuffer(
			myPhysicalDevice, myDevice, myWindowData.get(), nullptr, width, height, true, myDepthImageView, myDepthFormat);

		std::vector<VkCommandBuffer> threadCommandBuffers(myBufferCount);
		for (uint32_t cmdIt = 0; cmdIt < myCommandBufferCount; cmdIt++)
		{
			VkCommandPoolCreateInfo cmdPoolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			cmdPoolInfo.queueFamilyIndex = myQueueFamilyIndex;
			cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
			CHECK_VK(vkCreateCommandPool(myDevice, &cmdPoolInfo, nullptr, &myCommandPools[cmdIt]));
			assert(myCommandPools[cmdIt] != VK_NULL_HANDLE);

			VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			cmdInfo.commandPool = myCommandPools[cmdIt];
			cmdInfo.level = cmdIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
			cmdInfo.commandBufferCount = myBufferCount;
			CHECK_VK(vkAllocateCommandBuffers(myDevice, &cmdInfo, threadCommandBuffers.data()));

			for (uint32_t bufferIt = 0; bufferIt < myBufferCount; bufferIt++)
				myCommandBuffers[myCommandBufferCount * bufferIt + cmdIt] = threadCommandBuffers[bufferIt];
		}

		myFrameFences.resize(myBufferCount);
		myImageAcquiredSemaphores.resize(myBufferCount);
		myRenderCompleteSemaphores.resize(myBufferCount);

		for (uint32_t bufferIt = 0; bufferIt < myBufferCount; bufferIt++)
		{
			VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			CHECK_VK(vkCreateFence(myDevice, &fenceInfo, nullptr, &myFrameFences[bufferIt]));

			VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			CHECK_VK(vkCreateSemaphore(myDevice, &semaphoreInfo, nullptr, &myImageAcquiredSemaphores[bufferIt]));
			CHECK_VK(vkCreateSemaphore(myDevice, &semaphoreInfo, nullptr, &myRenderCompleteSemaphores[bufferIt]));

			// IMGUI uses primary command buffer only
			ImGui_ImplVulkanH_FrameData* fd = &myWindowData->Frames[bufferIt];
			fd->CommandPool = myCommandPools[0];
			fd->CommandBuffer = myCommandBuffers[myCommandBufferCount * bufferIt];
			fd->Fence = myFrameFences[bufferIt];
			fd->ImageAcquiredSemaphore = myImageAcquiredSemaphores[bufferIt];
			fd->RenderCompleteSemaphore = myRenderCompleteSemaphores[bufferIt];
		}
		//ImGui_ImplVulkanH_CreateWindowDataCommandBuffers(myDevice, myQueueFamilyIndex, myWindowData.get(), nullptr);

		transitionImageLayout(
			myDepthImage,
			myDepthFormat,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

		createRenderPass();
		createGraphicsPipeline();
	}

	void checkFlipOrPresentResult(VkResult result)
	{
		if (result == VK_SUBOPTIMAL_KHR)
			return; // not much we can do
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
			ImGui_ImplVulkanH_CreateWindowDataSwapChainAndFramebuffer(
				myPhysicalDevice, myDevice, myWindowData.get(), nullptr, myWindowData->Width,
				myWindowData->Height);
		else if (result != VK_SUCCESS)
			throw std::runtime_error("failed to flip swap chain image!");
	}

	void updateUniformBuffers()
	{
		UniformBufferObject* data;
		CHECK_VK(vmaMapMemory(myAllocator, myUniformBufferMemory, (void**)&data));

		static auto start = std::chrono::high_resolution_clock::now();
		auto now = std::chrono::high_resolution_clock::now();
		constexpr float period = 10.0;
		float t = std::chrono::duration<float>(now - start).count();

		for (uint32_t n = 0; n < (NX * NY); n++)
		{
			UniformBufferObject& ubo = data[n];				

			float tp = fmod((0.003f * n) + t, period);
			float s = smootherstep(
				smoothstep(clamp(ramp(tp < (0.5f * period) ? tp : period - tp, 0, 0.5f * period), 0, 1)));

			glm::mat4 model = glm::rotate(
				glm::translate(
					glm::mat4(1),
					glm::vec3(0, 0, -0.01f - std::numeric_limits<float>::epsilon())),
				s * glm::radians(360.0f),
				glm::vec3(0.0, 0.0, 1.0));

			glm::mat4 view0 = glm::mat4(1);
			glm::mat4 proj0 = glm::frustum(-1.0, 1.0, -1.0, 1.0, 0.01, 10.0);

			glm::mat4 view1 = 
				glm::lookAt(
					glm::vec3(1.5f, 1.5f, 1.0f),
					glm::vec3(0.0f, 0.0f, -0.5f),
					glm::vec3(0.0f, 0.0f, -1.0f));
			glm::mat4 proj1 = 
				glm::perspective(
					glm::radians(75.0f),
					myWindowData->Width / static_cast<float>(myWindowData->Height),
					0.01f,
					10.0f);

			ubo.model = model;
			ubo.view = glm::mat4(
				lerp(view0[0], view1[0], s),
				lerp(view0[1], view1[1], s),
				lerp(view0[2], view1[2], s),
				lerp(view0[3], view1[3], s));
			ubo.proj = glm::mat4(
				lerp(proj0[0], proj1[0], s),
				lerp(proj0[1], proj1[1], s),
				lerp(proj0[2], proj1[2], s),
				lerp(proj0[3], proj1[3], s));

			vmaFlushAllocation(
				myAllocator,
				myUniformBufferMemory,
				n * sizeof(UniformBufferObject),
				sizeof(UniformBufferObject));
		}
		
		vmaUnmapMemory(myAllocator, myUniformBufferMemory);
	}

	void submitFrame()
	{
		ImGui_ImplVulkanH_FrameData* oldFrame = &myWindowData->Frames[myWindowData->FrameIndex];
		VkSemaphore& imageAquiredSemaphore = oldFrame->ImageAcquiredSemaphore;

		checkFlipOrPresentResult(myDeviceTable.vkAcquireNextImageKHR(myDevice, myWindowData->Swapchain,
			UINT64_MAX, imageAquiredSemaphore,
			VK_NULL_HANDLE, &myWindowData->FrameIndex));
		/* MGPU method from vk 1.1 spec
		{
			VkAcquireNextImageInfoKHR nextImageInfo = {};
			nextImageInfo.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR;
			nextImageInfo.swapchain = myWindowData->Swapchain;
			nextImageInfo.timeout = UINT64_MAX;
			nextImageInfo.semaphore = imageAquiredSemaphore;
			nextImageInfo.fence = VK_NULL_HANDLE;
			nextImageInfo.deviceMask = ?;

			checkFlipOrPresentResult(myDeviceTable.vkAcquireNextImage2KHR(myDevice, &nextImageInfo,
		&myWindowData->FrameIndex));
		}
		 */

		ImGui_ImplVulkanH_FrameData* newFrame = &myWindowData->Frames[myWindowData->FrameIndex];

		std::array<VkClearValue, 2> clearValues = {};
		clearValues[0] = myWindowData->ClearValue;
		clearValues[1].depthStencil = { 1.0f, 0 };

		// wait for previous command buffer to be submitted
		{
			CHECK_VK(myDeviceTable.vkWaitForFences(myDevice, 1, &newFrame->Fence, VK_TRUE, UINT64_MAX));
			CHECK_VK(myDeviceTable.vkResetFences(myDevice, 1, &newFrame->Fence));
		}

		// setup draw parameters
		uint32_t dx = myWindowData->Width / NX;
		uint32_t dy = myWindowData->Height / NY;

		constexpr uint32_t drawCount = NX * NY;
		uint32_t segmentCount = std::max(myCommandBufferCount - 1u, 1u);
		uint32_t segmentDrawCount = drawCount / segmentCount;

		if (drawCount % segmentCount)
			segmentDrawCount += 1;

		// begin secondary command buffers
		for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
		{
			VkCommandBuffer& cmd =
				myCommandBuffers[myWindowData->FrameIndex * myCommandBufferCount + (segmentIt + 1)];

			VkCommandBufferInheritanceInfo inherit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
			inherit.renderPass = myRenderPass;
			inherit.framebuffer = myWindowData->Framebuffer[myWindowData->FrameIndex];

			CHECK_VK(vkResetCommandBuffer(cmd, 0));
			VkCommandBufferBeginInfo secBeginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			secBeginInfo.flags =
				VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
				VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
				VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
			secBeginInfo.pInheritanceInfo = &inherit;
			CHECK_VK(vkBeginCommandBuffer(cmd, &secBeginInfo));

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, myGraphicsPipeline);
			
			VkBuffer vertexBuffers[] = { myVertexBuffer };
			VkDeviceSize vertexOffsets[] = { 0 };

			vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, vertexOffsets);
			vkCmdBindIndexBuffer(cmd, myIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
		}

		// draw spinning logos using secondary command buffers
		{
			std::array<uint32_t, 128> seq;
			std::iota(seq.begin(), seq.begin() + segmentCount, 0);
			std::for_each_n(
			#ifndef __APPLE__
				std::execution::par,
			#endif
				seq.begin(),
				segmentCount,
				[this, &dx, &dy, &segmentDrawCount](uint32_t segmentIt)
			{
				VkCommandBuffer& cmd = myCommandBuffers[myWindowData->FrameIndex * myCommandBufferCount + (segmentIt + 1)];

				for (uint32_t drawIt = 0; drawIt < segmentDrawCount; drawIt++)
				{
					uint32_t n = segmentIt * segmentDrawCount + drawIt;

					if (n >= drawCount)
						break;

					uint32_t i = n % NX;
					uint32_t j = n / NX;

					auto drawSpinningLogo = [this, &n](VkCommandBuffer cmd, int32_t x, int32_t y, int32_t width, int32_t height)
					{
						VkViewport viewport = {};
						viewport.x = static_cast<float>(x);
						viewport.y = static_cast<float>(y);
						viewport.width = static_cast<float>(width);
						viewport.height = static_cast<float>(height);
						viewport.minDepth = 0.0f;
						viewport.maxDepth = 1.0f;

						VkRect2D scissor = {};
						scissor.offset = { x, y };
						scissor.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };

						uint32_t uniformBufferOffset = n * sizeof(UniformBufferObject);
						vkCmdBindDescriptorSets(
							cmd,
							VK_PIPELINE_BIND_POINT_GRAPHICS,
							myPipelineLayout,
							0,
							1,
							&myDescriptorSet,
							1,
							&uniformBufferOffset);

						vkCmdSetViewport(cmd, 0, 1, &viewport);
						vkCmdSetScissor(cmd, 0, 1, &scissor);
						vkCmdDrawIndexed(cmd, static_cast<uint32_t>(sizeof_array(ourIndices)), 1, 0, 0, 0);
					};

					drawSpinningLogo(cmd, i * dx, j * dy, dx, dy);
				}
			});
		}

		for (uint32_t segmentIt = 0; segmentIt < segmentCount; segmentIt++)
		{
			VkCommandBuffer& cmd =
				myCommandBuffers[myWindowData->FrameIndex * myCommandBufferCount + (segmentIt + 1)];

			CHECK_VK(vkEndCommandBuffer(cmd));
		}

		// begin primary command buffer
		{
			CHECK_VK(vkResetCommandBuffer(newFrame->CommandBuffer, 0));
			VkCommandBufferBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			CHECK_VK(vkBeginCommandBuffer(newFrame->CommandBuffer, &info));
		}

		// call secondary command buffers
		{
			VkRenderPassBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			beginInfo.renderPass = myRenderPass;
			beginInfo.framebuffer = myWindowData->Framebuffer[myWindowData->FrameIndex];
			beginInfo.renderArea.offset = { 0, 0 };
			beginInfo.renderArea.extent = { static_cast<uint32_t>(myWindowData->Width), static_cast<uint32_t>(myWindowData->Height) };
			beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			beginInfo.pClearValues = clearValues.data();
			vkCmdBeginRenderPass(newFrame->CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

			vkCmdExecuteCommands(newFrame->CommandBuffer,
				(myCommandBufferCount - 1),
				&myCommandBuffers[(myWindowData->FrameIndex * myCommandBufferCount) + 1]);

			vkCmdEndRenderPass(newFrame->CommandBuffer);
		}

		// Record Imgui Draw Data and draw funcs into command buffer
		{
			VkRenderPassBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			beginInfo.renderPass = myWindowData->RenderPass;
			beginInfo.framebuffer = myWindowData->Framebuffer[myWindowData->FrameIndex];
			beginInfo.renderArea.extent.width = myWindowData->Width;
			beginInfo.renderArea.extent.height = myWindowData->Height;
			beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
			beginInfo.pClearValues = clearValues.data();
			vkCmdBeginRenderPass(newFrame->CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), newFrame->CommandBuffer);

			vkCmdEndRenderPass(newFrame->CommandBuffer);
		}

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

	void cleanupFrameResources()
	{
		//ImGui_ImplVulkanH_DestroyWindowDataCommandBuffers(myDevice, myWindowData.get(), nullptr);
		{
			for (uint32_t bufferIt = 0; bufferIt < myBufferCount; bufferIt++)
			{
				vkDestroyFence(myDevice, myFrameFences[bufferIt], nullptr);
				vkDestroySemaphore(myDevice, myImageAcquiredSemaphores[bufferIt], nullptr);
				vkDestroySemaphore(myDevice, myRenderCompleteSemaphores[bufferIt], nullptr);
			}

			std::vector<VkCommandBuffer> threadCommandBuffers(myBufferCount);
			for (uint32_t cmdIt = 0; cmdIt < myCommandBufferCount; cmdIt++)
			{
				for (uint32_t bufferIt = 0; bufferIt < myBufferCount; bufferIt++)
					threadCommandBuffers[bufferIt] = myCommandBuffers[myCommandBufferCount * bufferIt + cmdIt];

				vkFreeCommandBuffers(myDevice, myCommandPools[cmdIt], myBufferCount, threadCommandBuffers.data());
				vkDestroyCommandPool(myDevice, myCommandPools[cmdIt], nullptr);
			}
		}

		ImGui_ImplVulkanH_DestroyWindowDataSwapChainAndFramebuffer(myDevice, myWindowData.get(), nullptr);

		vmaDestroyImage(myAllocator, myDepthImage, myDepthImageMemory);
		myDeviceTable.vkDestroyImageView(myDevice, myDepthImageView, nullptr);

		myDeviceTable.vkDestroyPipeline(myDevice, myGraphicsPipeline, nullptr);
		myDeviceTable.vkDestroyPipelineLayout(myDevice, myPipelineLayout, nullptr);
		myDeviceTable.vkDestroyRenderPass(myDevice, myRenderPass, nullptr);
	}

	void cleanup()
	{
		cleanupFrameResources();

		ImGui_ImplVulkan_Shutdown();

		ImGui::DestroyContext();

		vmaDestroyBuffer(myAllocator, myUniformBuffer, myUniformBufferMemory);
		vmaDestroyBuffer(myAllocator, myVertexBuffer, myVertexBufferMemory);
		vmaDestroyBuffer(myAllocator, myIndexBuffer, myIndexBufferMemory);
		
		vmaDestroyImage(myAllocator, myImage, myImageMemory);
		myDeviceTable.vkDestroyImageView(myDevice, myImageView, nullptr);

		myDeviceTable.vkDestroySampler(myDevice, mySampler, nullptr);

		myDeviceTable.vkDestroyDescriptorSetLayout(myDevice, myDescriptorSetLayout, nullptr);
		myDeviceTable.vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);

		vmaDestroyAllocator(myAllocator);

		myDeviceTable.vkDestroyDevice(myDevice, nullptr);
		
		vkDestroySurfaceKHR(myInstance, mySurface, nullptr);

		vkDestroyDebugReportCallbackEXT(myInstance, myDebugCallback, nullptr);

		vkDestroyInstance(myInstance, nullptr);
	}

	struct UniformBufferObject
	{
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;
		glm::mat4 pad;
	};

	struct SimpleVertex2D
	{
		glm::vec3 pos;
		glm::vec3 color;
		glm::vec2 texCoord;

		static VkVertexInputBindingDescription getBindingDescription()
		{
			VkVertexInputBindingDescription bindingDescription = {};
			bindingDescription.binding = 0;
			bindingDescription.stride = sizeof(SimpleVertex2D);
			bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
			return bindingDescription;
		}

		static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions()
		{
			std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions = {};
			attributeDescriptions[0].binding = 0;
			attributeDescriptions[0].location = 0;
			attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
			attributeDescriptions[0].offset = offsetof(SimpleVertex2D, pos);
			attributeDescriptions[1].binding = 0;
			attributeDescriptions[1].location = 1;
			attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
			attributeDescriptions[1].offset = offsetof(SimpleVertex2D, color);
			attributeDescriptions[2].binding = 0;
			attributeDescriptions[2].location = 2;
			attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
			attributeDescriptions[2].offset = offsetof(SimpleVertex2D, texCoord);
			return attributeDescriptions;
		}
	};

	static const SimpleVertex2D ourVertices[8];
	static const uint16_t ourIndices[12];

	VkInstance myInstance = VK_NULL_HANDLE;
	VkDebugReportCallbackEXT myDebugCallback = VK_NULL_HANDLE;
	VkSurfaceKHR mySurface = VK_NULL_HANDLE; // todo: take ownership of this object from IMGUI
	VkSurfaceFormatKHR mySurfaceFormat = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
	VkPresentModeKHR myPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
	VkPhysicalDevice myPhysicalDevice = VK_NULL_HANDLE;
	VkDevice myDevice = VK_NULL_HANDLE;
	VolkDeviceTable myDeviceTable = {};
	VmaAllocator myAllocator = VK_NULL_HANDLE;
	int myQueueFamilyIndex = -1;
	VkQueue myQueue = VK_NULL_HANDLE;
	VkDescriptorPool myDescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSetLayout myDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet myDescriptorSet = VK_NULL_HANDLE;
	VkRenderPass myRenderPass = VK_NULL_HANDLE;
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
	VkFormat myDepthFormat = VK_FORMAT_UNDEFINED;
	VkImage myDepthImage = VK_NULL_HANDLE;
	VmaAllocation myDepthImageMemory = VK_NULL_HANDLE;
	VkImageView myDepthImageView = VK_NULL_HANDLE;

	std::vector<VkCommandPool> myCommandPools; // count = [threadCount]
	std::vector<VkCommandBuffer> myCommandBuffers; // count = [threadCount*bufferCount] [f0cb0 f0cb1 f1cb0 f1cb1 f2cb0 f1cb1 ...]
	std::vector<VkFence> myFrameFences; // count = [bufferCount]
	std::vector<VkSemaphore> myImageAcquiredSemaphores; // count = [bufferCount]
	std::vector<VkSemaphore> myRenderCompleteSemaphores; // count = [bufferCount]

	std::unique_ptr<ImGui_ImplVulkanH_WindowData> myWindowData;
	std::vector<ImFont*> myFonts;

	const std::string myResourcePath;

	uint32_t myBufferCount = 0;
	uint32_t myCommandBufferCount = 0;

	static constexpr uint32_t NX = 16;
	static constexpr uint32_t NY = 9;
};

const VulkanApplication::SimpleVertex2D VulkanApplication::ourVertices[] =
{
	{ { -1.0f, -1.0f, 0.0f },{ 1.0f, 0.0f, 0.0f },{ 0.0f, 0.0f } },
	{ { -1.0f, 1.0f, 0.0f },{ 0.0f, 1.0f, 0.0f },{ 0.0f, 1.0f } },
	{ { 1.0f, 1.0f, 0.0f },{ 0.0f, 0.0f, 1.0f },{ 1.0f, 1.0f } },
	{ { 1.0f, -1.0f, 0.0f },{ 1.0f, 1.0f, 1.0f },{ 1.0f, 0.0f } },

	{ { -1.0f, -1.0f, -1.0f },{ 1.0f, 0.0f, 0.0f },{ 0.0f, 0.0f } },
	{ { -1.0f, 1.0f, -1.0f },{ 0.0f, 1.0f, 0.0f },{ 0.0f, 1.0f } },
	{ { 1.0f, 1.0f, -1.0f },{ 0.0f, 0.0f, 1.0f },{ 1.0f, 1.0f } },
	{ { 1.0f, -1.0f, -1.0f },{ 1.0f, 1.0f, 1.0f },{ 1.0f, 0.0f } }
};

const uint16_t VulkanApplication::ourIndices[] =
{
	0, 1, 2, 2, 3, 0,
	4, 5, 6, 6, 7, 4
};

static VulkanApplication* theApp = nullptr;

int vkapp_create(void* view, int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight, const char* resourcePath, bool verbose)
{
	assert(view != nullptr);
	assert(theApp == nullptr);

#if defined(__WINDOWS__)
	static const char* DISABLE_VK_LAYER_VALVE_steam_overlay_1 = "DISABLE_VK_LAYER_VALVE_steam_overlay_1=1";
	_putenv((char*)DISABLE_VK_LAYER_VALVE_steam_overlay_1);
#endif

	if (verbose)
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
	}

	theApp = new VulkanApplication(view, windowWidth, windowHeight, framebufferWidth, framebufferHeight, resourcePath ? resourcePath : "./", verbose);

	return EXIT_SUCCESS;
}

bool vkapp_is_initialized()
{
	return theApp != nullptr;
}

void vkapp_draw()
{
	assert(theApp != nullptr);

	theApp->draw();
}

void vkapp_resize(int width, int height)
{
	assert(theApp != nullptr);

	theApp->resize(width, height);
}

void vkapp_destroy()
{
	assert(theApp != nullptr);

	delete theApp;
}
