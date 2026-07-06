#include "VulkanRenderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#include <objbase.h>
#include <wincodec.h>
#endif

namespace viewer {
namespace {

const std::vector<const char*> ValidationLayers = {
    "VK_LAYER_KHRONOS_validation",
};

const std::vector<const char*> DeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

constexpr std::uint32_t FinalColorAttachment = 0;
constexpr std::uint32_t DepthAttachment = 1;
constexpr std::uint32_t OpaqueColorAttachment = 2;
constexpr std::uint32_t OitAccumAttachment = 3;
constexpr std::uint32_t OitRevealAttachment = 4;
constexpr std::uint32_t OitAdditiveAttachment = 5;
constexpr std::uint32_t ImGuiSubpass = 3;

#ifdef NDEBUG
constexpr bool WantValidationLayers = false;
#else
constexpr bool WantValidationLayers = true;
#endif

std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    const std::ifstream::pos_type fileSize = file.tellg();
    if (fileSize <= 0) {
        throw std::runtime_error("file is empty: " + path.string());
    }

    std::vector<char> buffer(static_cast<std::size_t>(fileSize));
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return buffer;
}

void checkVk(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message);
    }
}

std::uint32_t textureDescriptorIndex(std::uint32_t materialIndex, ApexTextureKind kind) {
    return materialIndex * static_cast<std::uint32_t>(ApexTextureKindCount) + static_cast<std::uint32_t>(kind);
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::array<std::uint8_t, 4> defaultPixel(ApexTextureKind kind) {
    switch (kind) {
    case ApexTextureKind::Albedo:
        return {255, 255, 255, 255};
    case ApexTextureKind::Normal:
        return {128, 128, 255, 255};
    case ApexTextureKind::Gloss:
        return {128, 128, 128, 255};
    case ApexTextureKind::Specular:
        return {10, 10, 10, 255};
    case ApexTextureKind::AmbientOcclusion:
    case ApexTextureKind::Cavity:
    case ApexTextureKind::Opacity:
    case ApexTextureKind::EmissiveMultiply:
        return {255, 255, 255, 255};
    case ApexTextureKind::ScatterThickness:
    case ApexTextureKind::Emissive:
        return {0, 0, 0, 255};
    case ApexTextureKind::Anisotropy:
        return {128, 128, 255, 255};
    case ApexTextureKind::Count:
        break;
    }
    return {255, 255, 255, 255};
}

VkFormat textureFormatForKind(ApexTextureKind kind) {
    return apexTextureKindUsesSrgb(kind) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

#ifdef _WIN32
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() {
        reset();
    }

    T** put() {
        reset();
        return &ptr_;
    }

    T* get() const {
        return ptr_;
    }

    T* operator->() const {
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

private:
    void reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T* ptr_ = nullptr;
};

struct LoadedImagePixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

void checkHr(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(message);
    }
}

LoadedImagePixels loadImagePixelsWic(const std::filesystem::path& path) {
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initResult == S_OK || initResult == S_FALSE;
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("failed to initialize COM for WIC image loading");
    }

    try {
        ComPtr<IWICImagingFactory> factory;
        checkHr(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())),
            "failed to create WIC imaging factory");

        ComPtr<IWICBitmapDecoder> decoder;
        checkHr(
            factory->CreateDecoderFromFilename(
                path.wstring().c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                decoder.put()),
            "failed to create WIC decoder for texture");

        ComPtr<IWICBitmapFrameDecode> frame;
        checkHr(decoder->GetFrame(0, frame.put()), "failed to read WIC frame");

        UINT width = 0;
        UINT height = 0;
        checkHr(frame->GetSize(&width, &height), "failed to query WIC texture size");
        if (width == 0 || height == 0) {
            throw std::runtime_error("texture image has zero size");
        }

        ComPtr<IWICFormatConverter> converter;
        checkHr(factory->CreateFormatConverter(converter.put()), "failed to create WIC format converter");
        checkHr(
            converter->Initialize(
                frame.get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "failed to convert texture to RGBA8");

        LoadedImagePixels image;
        image.width = width;
        image.height = height;
        const UINT stride = width * 4;
        const UINT byteCount = stride * height;
        image.rgba.resize(byteCount);
        checkHr(converter->CopyPixels(nullptr, stride, byteCount, image.rgba.data()), "failed to copy WIC texture pixels");

        if (shouldUninitialize) {
            CoUninitialize();
        }
        return image;
    } catch (...) {
        if (shouldUninitialize) {
            CoUninitialize();
        }
        throw;
    }
}
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*) {
    std::cerr << "Vulkan validation: " << callbackData->pMessage << '\n';
    return VK_FALSE;
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
}

VkResult createDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDebugUtilsMessengerEXT* debugMessenger) {
    const auto function = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (function != nullptr) {
        return function(instance, createInfo, allocator, debugMessenger);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroyDebugUtilsMessengerEXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT debugMessenger,
    const VkAllocationCallbacks* allocator) {
    const auto function = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (function != nullptr) {
        function(instance, debugMessenger, allocator);
    }
}

std::vector<const char*> requiredInstanceExtensions(bool includeDebugUtils) {
    std::uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
        throw std::runtime_error("GLFW did not report required Vulkan instance extensions");
    }

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
    if (includeDebugUtils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

} // namespace

VulkanRenderer::~VulkanRenderer() {
    cleanup();
}

void VulkanRenderer::initialize(GLFWwindow* window, LoadedModel model, ApexMaterialSet apexMaterialSet) {
    window_ = window;
    model_ = std::move(model);
    apexMaterialSet_ = std::move(apexMaterialSet);

    createInstance();
    setupDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapChain();
    createImageViews();
    createRenderPass();
    createDescriptorSetLayout();
    createGraphicsPipeline();
    createCompositePipeline();
    createCommandPool();
    createDefaultApexTextures();
    rebuildApexMaterialResources();
    rebuildDrawRanges();
    createDepthResources();
    createTransparencyResources();
    createFramebuffers();
    createVertexBuffer();
    createIndexBuffer();
    createUniformBuffers();
    createDescriptorPool();
    createDescriptorSets();
    createCompositeDescriptorPool();
    createCompositeDescriptorSets();
    initializeImGui();
    createCommandBuffers();
    createSyncObjects();

    initialized_ = true;
}

void VulkanRenderer::replaceModel(LoadedModel model, ApexMaterialSet apexMaterialSet) {
    if (model.vertices.empty() || model.indices.empty()) {
        throw std::runtime_error("replacement model has no drawable geometry");
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    destroyMeshBuffers();
    model_ = std::move(model);
    apexMaterialSet_ = std::move(apexMaterialSet);
    createVertexBuffer();
    createIndexBuffer();
    rebuildApexMaterialResources();
    rebuildDrawRanges();
    updateApexTextureDescriptors();
}

void VulkanRenderer::replaceApexMaterialSet(ApexMaterialSet apexMaterialSet) {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    apexMaterialSet_ = std::move(apexMaterialSet);
    rebuildApexMaterialResources();
    rebuildDrawRanges();
    updateApexTextureDescriptors();
}

void VulkanRenderer::beginUiFrame() {
    if (!imguiInitialized_) {
        return;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    uiFrameActive_ = true;
}

void VulkanRenderer::drawFrame(const CameraState& camera) {
    if (!initialized_) {
        return;
    }

    if (uiFrameActive_) {
        ImGui::Render();
        uiFrameActive_ = false;
    }

    checkVk(vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
            "failed to wait for frame fence");

    std::uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(
        device_,
        swapChain_,
        std::numeric_limits<std::uint64_t>::max(),
        imageAvailableSemaphores_[currentFrame_],
        VK_NULL_HANDLE,
        &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain();
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("failed to acquire swapchain image");
    }

    checkVk(vkResetFences(device_, 1, &inFlightFences_[currentFrame_]), "failed to reset frame fence");
    checkVk(vkResetCommandBuffer(commandBuffers_[currentFrame_], 0), "failed to reset command buffer");

    updateUniformBuffer(static_cast<std::uint32_t>(currentFrame_), camera);
    recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex);

    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    checkVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]), "failed to submit draw command buffer");

    VkSwapchainKHR swapChains[] = {swapChain_};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue_, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized_) {
        framebufferResized_ = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swapchain image");
    }

    currentFrame_ = (currentFrame_ + 1) % MaxFramesInFlight;
}

void VulkanRenderer::waitIdle() const {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

void VulkanRenderer::notifyFramebufferResized() {
    framebufferResized_ = true;
}

const ApexMaterialSet& VulkanRenderer::apexMaterialSet() const {
    return apexMaterialSet_;
}

void VulkanRenderer::setApexMaterialParameters(const ApexMaterialParameters& parameters) {
    apexMaterialSet_.parameters = parameters;
}

void VulkanRenderer::cleanup() {
    if (!initialized_ && device_ == VK_NULL_HANDLE && instance_ == VK_NULL_HANDLE) {
        return;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    shutdownImGui();
    cleanupSwapChain();
    destroyApexMaterialResources();
    destroyDefaultApexTextures();

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }

    for (std::size_t i = 0; i < MaxFramesInFlight; ++i) {
        if (uniformBuffersMapped_[i] != nullptr) {
            vkUnmapMemory(device_, uniformBuffersMemory_[i]);
            uniformBuffersMapped_[i] = nullptr;
        }
        if (uniformBuffers_[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, uniformBuffers_[i], nullptr);
            uniformBuffers_[i] = VK_NULL_HANDLE;
        }
        if (uniformBuffersMemory_[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device_, uniformBuffersMemory_[i], nullptr);
            uniformBuffersMemory_[i] = VK_NULL_HANDLE;
        }
    }

    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (compositeDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, compositeDescriptorSetLayout_, nullptr);
        compositeDescriptorSetLayout_ = VK_NULL_HANDLE;
    }

    destroyMeshBuffers();

    for (std::size_t i = 0; i < MaxFramesInFlight; ++i) {
        if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
            renderFinishedSemaphores_[i] = VK_NULL_HANDLE;
        }
        if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
            imageAvailableSemaphores_[i] = VK_NULL_HANDLE;
        }
        if (inFlightFences_[i] != VK_NULL_HANDLE) {
            vkDestroyFence(device_, inFlightFences_[i], nullptr);
            inFlightFences_[i] = VK_NULL_HANDLE;
        }
    }

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (debugMessenger_ != VK_NULL_HANDLE) {
        destroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
}

void VulkanRenderer::createInstance() {
    const bool enableValidation = WantValidationLayers && checkValidationLayerSupport();
    if (WantValidationLayers && !enableValidation) {
        std::cerr << "Vulkan validation layer was requested but VK_LAYER_KHRONOS_validation is unavailable.\n";
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Model Viewer";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "viewer";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    auto extensions = requiredInstanceExtensions(enableValidation);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (enableValidation) {
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(ValidationLayers.size());
        createInfo.ppEnabledLayerNames = ValidationLayers.data();
        populateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext = &debugCreateInfo;
    }

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "failed to create Vulkan instance");
}

void VulkanRenderer::setupDebugMessenger() {
    if (!WantValidationLayers || !checkValidationLayerSupport()) {
        return;
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);
    checkVk(createDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_), "failed to set up Vulkan debug messenger");
}

void VulkanRenderer::createSurface() {
    checkVk(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "failed to create window surface");
}

void VulkanRenderer::pickPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("failed to find a GPU with Vulkan support");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (VkPhysicalDevice device : devices) {
        if (!isDeviceSuitable(device)) {
            continue;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physicalDevice_ = device;
            break;
        }
        if (fallback == VK_NULL_HANDLE) {
            fallback = device;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        physicalDevice_ = fallback;
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("failed to find a suitable Vulkan GPU");
    }

    VkPhysicalDeviceProperties selectedProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &selectedProperties);
    std::cout << "Selected Vulkan device: " << selectedProperties.deviceName << '\n';
}

void VulkanRenderer::createLogicalDevice() {
    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    const std::set<std::uint32_t> uniqueQueueFamilies = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value(),
    };

    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    for (std::uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(DeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = DeviceExtensions.data();

    const bool enableValidation = WantValidationLayers && checkValidationLayerSupport();
    if (enableValidation) {
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(ValidationLayers.size());
        createInfo.ppEnabledLayerNames = ValidationLayers.data();
    }

    checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "failed to create Vulkan logical device");

    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
}

void VulkanRenderer::createSwapChain() {
    const SwapChainSupportDetails support = querySwapChainSupport(physicalDevice_);
    const VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = chooseSwapPresentMode(support.presentModes);
    const VkExtent2D extent = chooseSwapExtent(support.capabilities);

    std::uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    const std::uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};
    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    checkVk(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapChain_), "failed to create swapchain");

    vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, nullptr);
    swapChainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, swapChainImages_.data());

    swapChainImageFormat_ = surfaceFormat.format;
    swapChainExtent_ = extent;
}

void VulkanRenderer::createImageViews() {
    swapChainImageViews_.resize(swapChainImages_.size());
    for (std::size_t i = 0; i < swapChainImages_.size(); ++i) {
        swapChainImageViews_[i] = createImageView(swapChainImages_[i], swapChainImageFormat_, VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

void VulkanRenderer::createRenderPass() {
    transparencyAccumFormat_ = findTransparencyAccumFormat();
    transparencyRevealFormat_ = findTransparencyRevealFormat();

    VkAttachmentDescription finalColorAttachment{};
    finalColorAttachment.format = swapChainImageFormat_;
    finalColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    finalColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    finalColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    finalColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    finalColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    finalColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    finalColorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = findDepthFormat();
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription opaqueColorAttachment{};
    opaqueColorAttachment.format = swapChainImageFormat_;
    opaqueColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    opaqueColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    opaqueColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    opaqueColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    opaqueColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    opaqueColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    opaqueColorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription accumAttachment{};
    accumAttachment.format = transparencyAccumFormat_;
    accumAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    accumAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    accumAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    accumAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    accumAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    accumAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    accumAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription revealAttachment{};
    revealAttachment.format = transparencyRevealFormat_;
    revealAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    revealAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    revealAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    revealAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    revealAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    revealAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    revealAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription additiveAttachment = accumAttachment;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = DepthAttachment;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference opaqueColorRef{};
    opaqueColorRef.attachment = OpaqueColorAttachment;
    opaqueColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription opaqueSubpass{};
    opaqueSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    opaqueSubpass.colorAttachmentCount = 1;
    opaqueSubpass.pColorAttachments = &opaqueColorRef;
    opaqueSubpass.pDepthStencilAttachment = &depthAttachmentRef;

    const std::array<VkAttachmentReference, 3> transparentColorRefs = {{
        {OitAccumAttachment, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {OitRevealAttachment, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {OitAdditiveAttachment, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    }};

    VkSubpassDescription transparentSubpass{};
    transparentSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    transparentSubpass.colorAttachmentCount = static_cast<std::uint32_t>(transparentColorRefs.size());
    transparentSubpass.pColorAttachments = transparentColorRefs.data();
    transparentSubpass.pDepthStencilAttachment = &depthAttachmentRef;
    const std::array<std::uint32_t, 1> transparentPreserveAttachments = {OpaqueColorAttachment};
    transparentSubpass.preserveAttachmentCount = static_cast<std::uint32_t>(transparentPreserveAttachments.size());
    transparentSubpass.pPreserveAttachments = transparentPreserveAttachments.data();

    const std::array<VkAttachmentReference, 4> compositeInputRefs = {{
        {OpaqueColorAttachment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {OitAccumAttachment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {OitRevealAttachment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {OitAdditiveAttachment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    }};

    VkAttachmentReference finalColorRef{};
    finalColorRef.attachment = FinalColorAttachment;
    finalColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription compositeSubpass{};
    compositeSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    compositeSubpass.inputAttachmentCount = static_cast<std::uint32_t>(compositeInputRefs.size());
    compositeSubpass.pInputAttachments = compositeInputRefs.data();
    compositeSubpass.colorAttachmentCount = 1;
    compositeSubpass.pColorAttachments = &finalColorRef;

    VkSubpassDescription imguiSubpass{};
    imguiSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    imguiSubpass.colorAttachmentCount = 1;
    imguiSubpass.pColorAttachments = &finalColorRef;

    const std::array<VkSubpassDescription, 4> subpasses = {
        opaqueSubpass,
        transparentSubpass,
        compositeSubpass,
        imguiSubpass,
    };

    std::array<VkSubpassDependency, 5> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = 1;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[2].srcSubpass = 0;
    dependencies[2].dstSubpass = 2;
    dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[2].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[2].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    dependencies[2].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[3].srcSubpass = 1;
    dependencies[3].dstSubpass = 2;
    dependencies[3].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[3].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[3].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[3].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    dependencies[3].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[4].srcSubpass = 2;
    dependencies[4].dstSubpass = 3;
    dependencies[4].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[4].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[4].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[4].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[4].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    const std::array<VkAttachmentDescription, 6> attachments = {
        finalColorAttachment,
        depthAttachment,
        opaqueColorAttachment,
        accumAttachment,
        revealAttachment,
        additiveAttachment,
    };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = static_cast<std::uint32_t>(subpasses.size());
    renderPassInfo.pSubpasses = subpasses.data();
    renderPassInfo.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    checkVk(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_), "failed to create render pass");
}

void VulkanRenderer::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.pImmutableSamplers = nullptr;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding textureLayoutBinding{};
    textureLayoutBinding.binding = 1;
    textureLayoutBinding.descriptorCount = ApexTextureDescriptorCount;
    textureLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureLayoutBinding.pImmutableSamplers = nullptr;
    textureLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
        uboLayoutBinding,
        textureLayoutBinding,
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    checkVk(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_), "failed to create descriptor set layout");

    std::array<VkDescriptorSetLayoutBinding, 4> compositeBindings{};
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(compositeBindings.size()); ++i) {
        compositeBindings[i].binding = i;
        compositeBindings[i].descriptorCount = 1;
        compositeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        compositeBindings[i].pImmutableSamplers = nullptr;
        compositeBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo compositeLayoutInfo{};
    compositeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    compositeLayoutInfo.bindingCount = static_cast<std::uint32_t>(compositeBindings.size());
    compositeLayoutInfo.pBindings = compositeBindings.data();

    checkVk(vkCreateDescriptorSetLayout(
                device_,
                &compositeLayoutInfo,
                nullptr,
                &compositeDescriptorSetLayout_),
            "failed to create composite descriptor set layout");
}

void VulkanRenderer::createGraphicsPipeline() {
    const std::filesystem::path shaderDir = SHADER_DIR;
    const auto vertShaderCode = readFile(shaderDir / "mesh.vert.spv");
    const auto fragShaderCode = readFile(shaderDir / "mesh.frag.spv");
    const auto transparentFragShaderCode = readFile(shaderDir / "mesh_transparent.frag.spv");

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);
    VkShaderModule transparentFragShaderModule = createShaderModule(transparentFragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    const VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    VkPipelineShaderStageCreateInfo transparentFragShaderStageInfo = fragShaderStageInfo;
    transparentFragShaderStageInfo.module = transparentFragShaderModule;
    const VkPipelineShaderStageCreateInfo transparentShaderStages[] = {vertShaderStageInfo, transparentFragShaderStageInfo};

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions{};
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, position);
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, normal);
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Vertex, texCoord);
    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[3].offset = offsetof(Vertex, tangent);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(DrawPushConstants);
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    checkVk(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_), "failed to create pipeline layout");

    std::array<VkPipelineDepthStencilStateCreateInfo, 2> depthStates{};
    std::array<VkPipelineColorBlendAttachmentState, 4> blendAttachments{};
    std::array<VkPipelineColorBlendStateCreateInfo, 2> blendStates{};
    std::array<VkGraphicsPipelineCreateInfo, 2> pipelineInfos{};

    const auto configurePipelineState = [&](std::size_t index, std::uint32_t subpass, bool depthWrite) {
        depthStates[index] = depthStencil;
        depthStates[index].depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;

        VkGraphicsPipelineCreateInfo& pipelineInfo = pipelineInfos[index];
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = index == 0 ? shaderStages : transparentShaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStates[index];
        pipelineInfo.pColorBlendState = &blendStates[index];
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = subpass;
        pipelineInfo.basePipelineIndex = -1;
    };

    blendStates[0] = colorBlending;
    blendAttachments[0] = colorBlendAttachment;
    blendStates[0].attachmentCount = 1;
    blendStates[0].pAttachments = &blendAttachments[0];
    configurePipelineState(0, 0, true);

    blendStates[1] = colorBlending;
    blendAttachments[1] = colorBlendAttachment;
    blendAttachments[1].blendEnable = VK_TRUE;
    blendAttachments[1].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachments[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachments[1].colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachments[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachments[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachments[1].alphaBlendOp = VK_BLEND_OP_ADD;

    blendAttachments[2] = colorBlendAttachment;
    blendAttachments[2].blendEnable = VK_TRUE;
    blendAttachments[2].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachments[2].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    blendAttachments[2].colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachments[2].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachments[2].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachments[2].alphaBlendOp = VK_BLEND_OP_ADD;

    blendAttachments[3] = blendAttachments[1];

    blendStates[1].attachmentCount = 3;
    blendStates[1].pAttachments = &blendAttachments[1];
    configurePipelineState(1, 1, false);

    std::array<VkPipeline, 2> pipelines{};
    checkVk(vkCreateGraphicsPipelines(
                device_,
                VK_NULL_HANDLE,
                static_cast<std::uint32_t>(pipelineInfos.size()),
                pipelineInfos.data(),
                nullptr,
                pipelines.data()),
            "failed to create graphics pipelines");
    graphicsPipeline_ = pipelines[0];
    transparentPipeline_ = pipelines[1];

    vkDestroyShaderModule(device_, transparentFragShaderModule, nullptr);
    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);
}

void VulkanRenderer::createCompositePipeline() {
    const std::filesystem::path shaderDir = SHADER_DIR;
    const auto vertShaderCode = readFile(shaderDir / "composite.vert.spv");
    const auto fragShaderCode = readFile(shaderDir / "composite.frag.spv");

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    const VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &compositeDescriptorSetLayout_;

    checkVk(vkCreatePipelineLayout(
                device_,
                &pipelineLayoutInfo,
                nullptr,
                &compositePipelineLayout_),
            "failed to create composite pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineInfo{};
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
    pipelineInfo.layout = compositePipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 2;
    pipelineInfo.basePipelineIndex = -1;

    checkVk(vkCreateGraphicsPipelines(
                device_,
                VK_NULL_HANDLE,
                1,
                &pipelineInfo,
                nullptr,
                &compositePipeline_),
            "failed to create composite pipeline");

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);
}

void VulkanRenderer::createCommandPool() {
    const QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice_);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    checkVk(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "failed to create command pool");
}

void VulkanRenderer::createDefaultApexTextures() {
    for (std::size_t i = 0; i < ApexTextureKindCount; ++i) {
        const auto kind = static_cast<ApexTextureKind>(i);
        const std::array<std::uint8_t, 4> pixel = defaultPixel(kind);
        createTextureFromPixels(
            1,
            1,
            std::vector<std::uint8_t>(pixel.begin(), pixel.end()),
            textureFormatForKind(kind),
            apexTextureKindPrefersNearest(kind),
            defaultApexTextures_[i]);
    }
    updateApexTextureDescriptors();
}

void VulkanRenderer::rebuildApexMaterialResources() {
    destroyApexMaterialResources();
    apexMaterialSlotLookup_.clear();

    const std::uint32_t slotCount = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(apexMaterialSet_.slots.size()),
        MaxApexMaterialSlots);
    if (apexMaterialSet_.slots.size() > MaxApexMaterialSlots) {
        std::cerr << "[ApexMaterial] warning: renderer supports " << MaxApexMaterialSlots
                  << " material slots; extra slots will use the last available fallback.\n";
    }

    apexTextures_.resize(static_cast<std::size_t>(slotCount) * ApexTextureKindCount);
    for (std::uint32_t slotIndex = 0; slotIndex < slotCount; ++slotIndex) {
        const ApexMaterialSlot& slot = apexMaterialSet_.slots[slotIndex];
        apexMaterialSlotLookup_[lowerAscii(slot.name)] = slotIndex;

        for (std::size_t kindIndex = 0; kindIndex < ApexTextureKindCount; ++kindIndex) {
            if (!slot.hasTexture[kindIndex]) {
                continue;
            }

            const auto kind = static_cast<ApexTextureKind>(kindIndex);
            TextureResource& texture = apexTextures_[textureDescriptorIndex(slotIndex, kind)];
            try {
                createTextureFromFile(slot.textures[kindIndex], kind, texture);
                std::cout << "[ApexMaterial] uploaded " << slot.name << "."
                          << apexTextureKindName(kind) << ": " << slot.textures[kindIndex] << '\n';
            } catch (const std::exception& ex) {
                std::string message = "[ApexMaterial] warning: failed to upload " +
                    slot.textures[kindIndex].string() + ": " + ex.what() + "; using default";
                std::cerr << message << '\n';
                apexMaterialSet_.logLines.push_back(std::move(message));
            }
        }
    }

    updateApexTextureDescriptors();
}

void VulkanRenderer::destroyApexTexture(TextureResource& texture) {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (texture.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, texture.sampler, nullptr);
        texture.sampler = VK_NULL_HANDLE;
    }
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
    texture.format = VK_FORMAT_UNDEFINED;
}

void VulkanRenderer::destroyApexMaterialResources() {
    for (TextureResource& texture : apexTextures_) {
        destroyApexTexture(texture);
    }
    apexTextures_.clear();
    apexMaterialSlotLookup_.clear();
}

void VulkanRenderer::destroyDefaultApexTextures() {
    for (TextureResource& texture : defaultApexTextures_) {
        destroyApexTexture(texture);
    }
}

void VulkanRenderer::createTextureFromPixels(
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t>& rgbaPixels,
    VkFormat format,
    bool nearest,
    TextureResource& texture) {
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    if (rgbaPixels.size() < static_cast<std::size_t>(imageSize)) {
        throw std::runtime_error("texture pixel buffer is smaller than expected");
    }

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
    createBuffer(
        imageSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingBufferMemory);

    void* data = nullptr;
    vkMapMemory(device_, stagingBufferMemory, 0, imageSize, 0, &data);
    std::memcpy(data, rgbaPixels.data(), static_cast<std::size_t>(imageSize));
    vkUnmapMemory(device_, stagingBufferMemory);

    createImage(
        width,
        height,
        format,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        texture.image,
        texture.memory);
    texture.format = format;

    transitionImageLayout(texture.image, format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(stagingBuffer, texture.image, width, height);
    transitionImageLayout(texture.image, format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingBufferMemory, nullptr);

    texture.view = createImageView(texture.image, format, VK_IMAGE_ASPECT_COLOR_BIT);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    samplerInfo.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &texture.sampler), "failed to create texture sampler");
}

void VulkanRenderer::createTextureFromFile(
    const std::filesystem::path& path,
    ApexTextureKind kind,
    TextureResource& texture) {
#ifdef _WIN32
    const LoadedImagePixels image = loadImagePixelsWic(path);
    createTextureFromPixels(
        image.width,
        image.height,
        image.rgba,
        textureFormatForKind(kind),
        apexTextureKindPrefersNearest(kind),
        texture);
#else
    static_cast<void>(path);
    static_cast<void>(kind);
    static_cast<void>(texture);
    throw std::runtime_error("texture file loading is only implemented through WIC on Windows");
#endif
}

void VulkanRenderer::updateApexTextureDescriptors() {
    for (std::uint32_t slotIndex = 0; slotIndex < MaxApexMaterialSlots; ++slotIndex) {
        for (std::uint32_t kindIndex = 0; kindIndex < static_cast<std::uint32_t>(ApexTextureKindCount); ++kindIndex) {
            const auto kind = static_cast<ApexTextureKind>(kindIndex);
            const std::uint32_t descriptorIndex = textureDescriptorIndex(slotIndex, kind);

            const TextureResource* texture = &defaultApexTextures_[kindIndex];
            const std::size_t ownedIndex = descriptorIndex;
            if (ownedIndex < apexTextures_.size() && apexTextures_[ownedIndex].view != VK_NULL_HANDLE) {
                texture = &apexTextures_[ownedIndex];
            }

            apexTextureDescriptors_[descriptorIndex].sampler = texture->sampler;
            apexTextureDescriptors_[descriptorIndex].imageView = texture->view;
            apexTextureDescriptors_[descriptorIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    if (descriptorPool_ == VK_NULL_HANDLE || descriptorSets_[0] == VK_NULL_HANDLE) {
        return;
    }

    for (std::size_t frame = 0; frame < MaxFramesInFlight; ++frame) {
        VkWriteDescriptorSet textureWrite{};
        textureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        textureWrite.dstSet = descriptorSets_[frame];
        textureWrite.dstBinding = 1;
        textureWrite.dstArrayElement = 0;
        textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textureWrite.descriptorCount = ApexTextureDescriptorCount;
        textureWrite.pImageInfo = apexTextureDescriptors_.data();
        vkUpdateDescriptorSets(device_, 1, &textureWrite, 0, nullptr);
    }
}

void VulkanRenderer::createDepthResources() {
    const VkFormat depthFormat = findDepthFormat();
    createImage(
        swapChainExtent_.width,
        swapChainExtent_.height,
        depthFormat,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        depthImage_,
        depthImageMemory_);
    depthImageView_ = createImageView(depthImage_, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void VulkanRenderer::createTransparencyResources() {
    transparencyResources_.resize(swapChainImageViews_.size());

    const auto createAttachment = [this](VkFormat format, AttachmentResource& attachment) {
        attachment.format = format;
        createImage(
            swapChainExtent_.width,
            swapChainExtent_.height,
            format,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            attachment.image,
            attachment.memory);
        attachment.view = createImageView(attachment.image, format, VK_IMAGE_ASPECT_COLOR_BIT);
    };

    for (TransparencyFramebufferResources& resources : transparencyResources_) {
        createAttachment(swapChainImageFormat_, resources.opaqueColor);
        createAttachment(transparencyAccumFormat_, resources.accum);
        createAttachment(transparencyRevealFormat_, resources.reveal);
        createAttachment(transparencyAccumFormat_, resources.additive);
    }
}

void VulkanRenderer::destroyAttachmentResource(AttachmentResource& attachment) {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (attachment.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, attachment.view, nullptr);
        attachment.view = VK_NULL_HANDLE;
    }
    if (attachment.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, attachment.image, nullptr);
        attachment.image = VK_NULL_HANDLE;
    }
    if (attachment.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, attachment.memory, nullptr);
        attachment.memory = VK_NULL_HANDLE;
    }
    attachment.format = VK_FORMAT_UNDEFINED;
}

void VulkanRenderer::destroyTransparencyResources() {
    for (TransparencyFramebufferResources& resources : transparencyResources_) {
        destroyAttachmentResource(resources.opaqueColor);
        destroyAttachmentResource(resources.accum);
        destroyAttachmentResource(resources.reveal);
        destroyAttachmentResource(resources.additive);
    }
    transparencyResources_.clear();
}

void VulkanRenderer::createFramebuffers() {
    swapChainFramebuffers_.resize(swapChainImageViews_.size());

    for (std::size_t i = 0; i < swapChainImageViews_.size(); ++i) {
        const TransparencyFramebufferResources& resources = transparencyResources_[i];
        const std::array<VkImageView, 6> attachments = {
            swapChainImageViews_[i],
            depthImageView_,
            resources.opaqueColor.view,
            resources.accum.view,
            resources.reveal.view,
            resources.additive.view,
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapChainExtent_.width;
        framebufferInfo.height = swapChainExtent_.height;
        framebufferInfo.layers = 1;

        checkVk(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &swapChainFramebuffers_[i]), "failed to create framebuffer");
    }
}

void VulkanRenderer::createVertexBuffer() {
    if (model_.vertices.empty()) {
        throw std::runtime_error("cannot create a vertex buffer for an empty model");
    }

    const VkDeviceSize bufferSize = sizeof(model_.vertices[0]) * model_.vertices.size();
    createBuffer(
        bufferSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        vertexBuffer_,
        vertexBufferMemory_);

    void* data = nullptr;
    vkMapMemory(device_, vertexBufferMemory_, 0, bufferSize, 0, &data);
    std::memcpy(data, model_.vertices.data(), static_cast<std::size_t>(bufferSize));
    vkUnmapMemory(device_, vertexBufferMemory_);
}

void VulkanRenderer::createIndexBuffer() {
    if (model_.indices.empty()) {
        throw std::runtime_error("cannot create an index buffer for an empty model");
    }

    const VkDeviceSize bufferSize = sizeof(model_.indices[0]) * model_.indices.size();
    createBuffer(
        bufferSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        indexBuffer_,
        indexBufferMemory_);

    void* data = nullptr;
    vkMapMemory(device_, indexBufferMemory_, 0, bufferSize, 0, &data);
    std::memcpy(data, model_.indices.data(), static_cast<std::size_t>(bufferSize));
    vkUnmapMemory(device_, indexBufferMemory_);
}

void VulkanRenderer::destroyMeshBuffers() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    if (indexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, indexBuffer_, nullptr);
        indexBuffer_ = VK_NULL_HANDLE;
    }
    if (indexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, indexBufferMemory_, nullptr);
        indexBufferMemory_ = VK_NULL_HANDLE;
    }
    if (vertexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vertexBuffer_ = VK_NULL_HANDLE;
    }
    if (vertexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);
        vertexBufferMemory_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::createUniformBuffers() {
    const VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    for (std::size_t i = 0; i < MaxFramesInFlight; ++i) {
        createBuffer(
            bufferSize,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            uniformBuffers_[i],
            uniformBuffersMemory_[i]);

        checkVk(vkMapMemory(device_, uniformBuffersMemory_[i], 0, bufferSize, 0, &uniformBuffersMapped_[i]),
                "failed to map uniform buffer memory");
    }
}

void VulkanRenderer::createDescriptorPool() {
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MaxFramesInFlight;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MaxFramesInFlight * ApexTextureDescriptorCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = MaxFramesInFlight;

    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "failed to create descriptor pool");
}

void VulkanRenderer::createDescriptorSets() {
    std::array<VkDescriptorSetLayout, MaxFramesInFlight> layouts{};
    layouts.fill(descriptorSetLayout_);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = MaxFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();

    checkVk(vkAllocateDescriptorSets(device_, &allocInfo, descriptorSets_.data()), "failed to allocate descriptor sets");

    for (std::size_t i = 0; i < MaxFramesInFlight; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSets_[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);
    }

    updateApexTextureDescriptors();
}

void VulkanRenderer::createCompositeDescriptorPool() {
    if (compositeDescriptorPool_ != VK_NULL_HANDLE) {
        return;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    poolSize.descriptorCount = static_cast<std::uint32_t>(swapChainImages_.size() * 4);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = static_cast<std::uint32_t>(swapChainImages_.size());

    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &compositeDescriptorPool_), "failed to create composite descriptor pool");
}

void VulkanRenderer::createCompositeDescriptorSets() {
    compositeDescriptorSets_.clear();
    if (swapChainImages_.empty()) {
        return;
    }
    if (transparencyResources_.size() != swapChainImages_.size()) {
        throw std::runtime_error("cannot create composite descriptor sets without transparency attachments");
    }

    std::vector<VkDescriptorSetLayout> layouts(swapChainImages_.size(), compositeDescriptorSetLayout_);
    compositeDescriptorSets_.resize(swapChainImages_.size());

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = compositeDescriptorPool_;
    allocInfo.descriptorSetCount = static_cast<std::uint32_t>(compositeDescriptorSets_.size());
    allocInfo.pSetLayouts = layouts.data();

    checkVk(vkAllocateDescriptorSets(device_, &allocInfo, compositeDescriptorSets_.data()), "failed to allocate composite descriptor sets");

    for (std::size_t i = 0; i < compositeDescriptorSets_.size(); ++i) {
        const TransparencyFramebufferResources& resources = transparencyResources_[i];
        const std::array<VkDescriptorImageInfo, 4> imageInfos = {{
            {VK_NULL_HANDLE, resources.opaqueColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, resources.accum.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, resources.reveal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, resources.additive.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        }};

        std::array<VkWriteDescriptorSet, 4> descriptorWrites{};
        for (std::uint32_t binding = 0; binding < static_cast<std::uint32_t>(descriptorWrites.size()); ++binding) {
            descriptorWrites[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[binding].dstSet = compositeDescriptorSets_[i];
            descriptorWrites[binding].dstBinding = binding;
            descriptorWrites[binding].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            descriptorWrites[binding].descriptorCount = 1;
            descriptorWrites[binding].pImageInfo = &imageInfos[binding];
        }

        vkUpdateDescriptorSets(
            device_,
            static_cast<std::uint32_t>(descriptorWrites.size()),
            descriptorWrites.data(),
            0,
            nullptr);
    }
}

void VulkanRenderer::destroyCompositeDescriptorPool() {
    compositeDescriptorSets_.clear();
    if (compositeDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, compositeDescriptorPool_, nullptr);
        compositeDescriptorPool_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::createImGuiDescriptorPool() {
    if (imguiDescriptorPool_ != VK_NULL_HANDLE) {
        return;
    }

    const std::array<VkDescriptorPoolSize, 11> poolSizes = {{
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000 * static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &imguiDescriptorPool_), "failed to create ImGui descriptor pool");
}

void VulkanRenderer::destroyImGuiDescriptorPool() {
    if (imguiDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, imguiDescriptorPool_, nullptr);
        imguiDescriptorPool_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::initializeImGui() {
    if (imguiInitialized_) {
        return;
    }

    createImGuiDescriptorPool();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    ImGui_ImplGlfw_InitForVulkan(window_, true);

    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    if (!indices.graphicsFamily.has_value()) {
        throw std::runtime_error("failed to initialize ImGui: graphics queue family is missing");
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = instance_;
    initInfo.PhysicalDevice = physicalDevice_;
    initInfo.Device = device_;
    initInfo.QueueFamily = *indices.graphicsFamily;
    initInfo.Queue = graphicsQueue_;
    initInfo.DescriptorPool = imguiDescriptorPool_;
    initInfo.RenderPass = renderPass_;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = static_cast<std::uint32_t>(swapChainImages_.size());
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.PipelineCache = VK_NULL_HANDLE;
    initInfo.Subpass = ImGuiSubpass;
    initInfo.UseDynamicRendering = false;
    initInfo.Allocator = nullptr;
    initInfo.MinAllocationSize = 1024 * 1024;
    initInfo.CheckVkResultFn = [](VkResult result) {
        checkVk(result, "ImGui Vulkan backend reported an error");
    };

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        throw std::runtime_error("failed to initialize ImGui Vulkan backend");
    }
    if (!ImGui_ImplVulkan_CreateFontsTexture()) {
        throw std::runtime_error("failed to create ImGui font texture");
    }

    imguiInitialized_ = true;
}

void VulkanRenderer::shutdownImGui() {
    if (imguiInitialized_) {
        if (uiFrameActive_) {
            ImGui::EndFrame();
            uiFrameActive_ = false;
        }
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    destroyImGuiDescriptorPool();
}

void VulkanRenderer::createCommandBuffers() {
    commandBuffers_.resize(MaxFramesInFlight);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());

    checkVk(vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()), "failed to allocate command buffers");
}

void VulkanRenderer::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (std::size_t i = 0; i < MaxFramesInFlight; ++i) {
        checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]), "failed to create image semaphore");
        checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]), "failed to create render semaphore");
        checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]), "failed to create frame fence");
    }
}

void VulkanRenderer::recreateSwapChain() {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window_, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_);

    shutdownImGui();
    cleanupSwapChain();
    createSwapChain();
    createImageViews();
    createRenderPass();
    createGraphicsPipeline();
    createCompositePipeline();
    createDepthResources();
    createTransparencyResources();
    createFramebuffers();
    createCompositeDescriptorPool();
    createCompositeDescriptorSets();
    initializeImGui();
}

void VulkanRenderer::cleanupSwapChain() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    destroyCompositeDescriptorPool();

    for (VkFramebuffer framebuffer : swapChainFramebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    swapChainFramebuffers_.clear();

    if (graphicsPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
        graphicsPipeline_ = VK_NULL_HANDLE;
    }
    if (transparentPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, transparentPipeline_, nullptr);
        transparentPipeline_ = VK_NULL_HANDLE;
    }
    if (compositePipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, compositePipeline_, nullptr);
        compositePipeline_ = VK_NULL_HANDLE;
    }
    if (compositePipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, compositePipelineLayout_, nullptr);
        compositePipelineLayout_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }

    for (VkImageView imageView : swapChainImageViews_) {
        vkDestroyImageView(device_, imageView, nullptr);
    }
    swapChainImageViews_.clear();

    if (depthImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, depthImageView_, nullptr);
        depthImageView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthImageMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, depthImageMemory_, nullptr);
        depthImageMemory_ = VK_NULL_HANDLE;
    }

    destroyTransparencyResources();

    if (swapChain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapChain_, nullptr);
        swapChain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "failed to begin recording command buffer");

    std::array<VkClearValue, 6> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    clearValues[2].color = {{0.015f, 0.018f, 0.024f, 1.0f}};
    clearValues[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[4].color = {{1.0f, 1.0f, 1.0f, 1.0f}};
    clearValues[5].color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = swapChainFramebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapChainExtent_;
    renderPassInfo.clearValueCount = static_cast<std::uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    const auto setViewportAndScissor = [&]() {
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChainExtent_.width);
        viewport.height = static_cast<float>(swapChainExtent_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent_;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    };

    setViewportAndScissor();

    VkBuffer vertexBuffers[] = {vertexBuffer_};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout_,
        0,
        1,
        &descriptorSets_[currentFrame_],
        0,
        nullptr);
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    const auto drawRange = [&](const DrawRange& range, VkPipeline pipeline) {
        if (boundPipeline != pipeline) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            boundPipeline = pipeline;
        }

        const DrawPushConstants pushConstants{
            range.materialIndex,
            static_cast<std::uint32_t>(range.alphaMode),
            static_cast<std::uint32_t>(range.opacitySource),
            static_cast<std::uint32_t>(range.opacityChannel),
        };
        vkCmdPushConstants(
            commandBuffer,
            pipelineLayout_,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(pushConstants),
            &pushConstants);
        vkCmdDrawIndexed(commandBuffer, range.indexCount, 1, range.firstIndex, 0, 0);
    };

    if (drawRanges_.empty()) {
        DrawRange fallbackRange{};
        fallbackRange.indexCount = static_cast<std::uint32_t>(model_.indices.size());
        drawRange(fallbackRange, graphicsPipeline_);
    } else {
        for (const DrawRange& range : drawRanges_) {
            if (range.alphaMode != ApexAlphaMode::Translucent && range.alphaMode != ApexAlphaMode::Additive) {
                drawRange(range, graphicsPipeline_);
            }
        }
    }

    vkCmdNextSubpass(commandBuffer, VK_SUBPASS_CONTENTS_INLINE);
    setViewportAndScissor();
    boundPipeline = VK_NULL_HANDLE;
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout_,
        0,
        1,
        &descriptorSets_[currentFrame_],
        0,
        nullptr);

    if (!drawRanges_.empty()) {
        for (const DrawRange& range : drawRanges_) {
            if (range.alphaMode == ApexAlphaMode::Translucent || range.alphaMode == ApexAlphaMode::Additive) {
                drawRange(range, transparentPipeline_);
            }
        }
    }

    vkCmdNextSubpass(commandBuffer, VK_SUBPASS_CONTENTS_INLINE);
    setViewportAndScissor();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        compositePipelineLayout_,
        0,
        1,
        &compositeDescriptorSets_[imageIndex],
        0,
        nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdNextSubpass(commandBuffer, VK_SUBPASS_CONTENTS_INLINE);
    if (imguiInitialized_) {
        if (ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr && drawData->CmdListsCount > 0) {
            ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        }
    }

    vkCmdEndRenderPass(commandBuffer);

    checkVk(vkEndCommandBuffer(commandBuffer), "failed to record command buffer");
}

void VulkanRenderer::updateUniformBuffer(std::uint32_t currentImage, const CameraState& camera) {
    const float aspect = static_cast<float>(swapChainExtent_.width) / static_cast<float>(swapChainExtent_.height);
    const float clampedPitch = std::clamp(camera.pitch, -1.45f, 1.45f);
    const float distance = std::clamp(camera.distance, 1.5f, 100.0f);

    const Vec3 eye = {
        distance * std::cos(clampedPitch) * std::sin(camera.yaw),
        distance * std::sin(clampedPitch),
        distance * std::cos(clampedPitch) * std::cos(camera.yaw),
    };
    lastCameraPosition_ = eye;

    UniformBufferObject ubo{};
    ubo.model = identity();
    ubo.view = lookAt(eye, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    ubo.proj = perspective(radians(60.0f), aspect, 0.05f, 200.0f);
    ubo.lightDirection = {-0.35f, -0.8f, -0.45f, 0.0f};
    ubo.cameraPosition = {eye.x, eye.y, eye.z, 1.0f};

    const ApexMaterialParameters& p = apexMaterialSet_.parameters;
    ubo.apexFlags = {
        p.enableApexMaterialMode ? 1.0f : 0.0f,
        p.flipNormalGreen ? 1.0f : 0.0f,
        p.enableSubsurfaceApproximation ? 1.0f : 0.0f,
        p.enableAnisotropy ? 1.0f : 0.0f,
    };
    ubo.apexFactors0 = {
        p.roughnessMultiplier,
        p.specularMultiplier,
        p.aoStrength,
        p.cavityStrength,
    };
    ubo.apexFactors1 = {
        p.emissiveStrength,
        p.alphaCutoff,
        p.subsurfaceStrength,
        p.subsurfaceThicknessScale,
    };
    ubo.apexSubsurfaceColor = {
        p.subsurfaceColor.x,
        p.subsurfaceColor.y,
        p.subsurfaceColor.z,
        p.anisotropyStrength,
    };
    ubo.apexEmissiveTint = {
        p.emissiveTint.x,
        p.emissiveTint.y,
        p.emissiveTint.z,
        1.0f,
    };
    ubo.apexDebug = {
        static_cast<float>(static_cast<std::uint32_t>(p.debugView)),
        0.0f,
        0.0f,
        0.0f,
    };

    std::memcpy(uniformBuffersMapped_[currentImage], &ubo, sizeof(ubo));
}

void VulkanRenderer::rebuildDrawRanges() {
    drawRanges_.clear();
    if (model_.indices.empty()) {
        return;
    }

    if (model_.materialSlots.empty()) {
        drawRanges_.push_back({0, static_cast<std::uint32_t>(model_.indices.size()), 0});
        return;
    }

    for (const MaterialSlotRange& slotRange : model_.materialSlots) {
        if (slotRange.indexCount == 0 || slotRange.firstIndex >= model_.indices.size()) {
            continue;
        }

        const std::uint32_t availableCount =
            static_cast<std::uint32_t>(model_.indices.size() - slotRange.firstIndex);
        const std::uint32_t clampedCount = std::min(slotRange.indexCount, availableCount);

        std::uint32_t materialIndex = 0;
        const auto found = apexMaterialSlotLookup_.find(lowerAscii(slotRange.name));
        if (found != apexMaterialSlotLookup_.end()) {
            materialIndex = found->second;
        } else if (apexMaterialSet_.parameters.enableApexMaterialMode) {
            std::cerr << "[ApexMaterial] warning: draw range material slot '" << slotRange.name
                      << "' has no matching Apex texture slot; using slot 0 defaults.\n";
        }

        ApexAlphaMode alphaMode = ApexAlphaMode::Opaque;
        ApexOpacitySource opacitySource = ApexOpacitySource::One;
        ApexOpacityChannel opacityChannel = ApexOpacityChannel::R;
        if (apexMaterialSet_.parameters.enableApexMaterialMode &&
            materialIndex < static_cast<std::uint32_t>(apexMaterialSet_.slots.size())) {
            const ApexMaterialSlot& slot = apexMaterialSet_.slots[materialIndex];
            alphaMode = slot.alphaMode;
            opacitySource = slot.opacitySource;
            opacityChannel = slot.opacityChannel;
        }

        Vec3 center{};
        std::uint32_t centerSampleCount = 0;
        for (std::uint32_t i = 0; i < clampedCount; ++i) {
            const std::uint32_t index = model_.indices[slotRange.firstIndex + i];
            if (index >= model_.vertices.size()) {
                continue;
            }
            const Vec3 position = model_.vertices[index].position;
            center = center + position;
            ++centerSampleCount;
        }
        if (centerSampleCount > 0) {
            center = center / static_cast<float>(centerSampleCount);
        }

        drawRanges_.push_back({
            slotRange.firstIndex,
            clampedCount,
            materialIndex,
            alphaMode,
            opacitySource,
            opacityChannel,
            center,
        });
    }

    if (drawRanges_.empty()) {
        drawRanges_.push_back({0, static_cast<std::uint32_t>(model_.indices.size()), 0});
    }
}

bool VulkanRenderer::checkValidationLayerSupport() const {
    std::uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : ValidationLayers) {
        const auto found = std::find_if(
            availableLayers.begin(),
            availableLayers.end(),
            [layerName](const VkLayerProperties& layerProperties) {
                return std::strcmp(layerName, layerProperties.layerName) == 0;
            });
        if (found == availableLayers.end()) {
            return false;
        }
    }
    return true;
}

VulkanRenderer::QueueFamilyIndices VulkanRenderer::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (std::uint32_t i = 0; i < queueFamilies.size(); ++i) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport) {
            indices.presentFamily = i;
        }

        if (indices.complete()) {
            break;
        }
    }

    return indices;
}

VulkanRenderer::SwapChainSupportDetails VulkanRenderer::querySwapChainSupport(VkPhysicalDevice device) const {
    SwapChainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
    }

    std::uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, details.presentModes.data());
    }

    return details;
}

bool VulkanRenderer::isDeviceSuitable(VkPhysicalDevice device) const {
    const QueueFamilyIndices indices = findQueueFamilies(device);
    const bool extensionsSupported = checkDeviceExtensionSupport(device);
    bool swapChainAdequate = false;
    if (extensionsSupported) {
        const SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }
    return indices.complete() && extensionsSupported && swapChainAdequate;
}

bool VulkanRenderer::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
    std::uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::set<std::string> requiredExtensions(DeviceExtensions.begin(), DeviceExtensions.end());
    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }
    return requiredExtensions.empty();
}

VkSurfaceFormatKHR VulkanRenderer::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    for (const auto& availableFormat : formats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    return formats.front();
}

VkPresentModeKHR VulkanRenderer::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) const {
    for (const auto& availablePresentMode : modes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);

    VkExtent2D actualExtent = {
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };

    actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return actualExtent;
}

VkShaderModule VulkanRenderer::createShaderModule(const std::vector<char>& code) const {
    if ((code.size() % 4) != 0) {
        throw std::runtime_error("SPIR-V shader bytecode size is not divisible by four");
    }

    std::vector<std::uint32_t> alignedCode(code.size() / sizeof(std::uint32_t));
    std::memcpy(alignedCode.data(), code.data(), code.size());

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = alignedCode.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule), "failed to create shader module");
    return shaderModule;
}

std::uint32_t VulkanRenderer::findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) != 0 &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable Vulkan memory type");
}

void VulkanRenderer::createBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    checkVk(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer), "failed to create buffer");

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, properties);

    checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &bufferMemory), "failed to allocate buffer memory");
    checkVk(vkBindBufferMemory(device_, buffer, bufferMemory, 0), "failed to bind buffer memory");
}

void VulkanRenderer::createImage(
    std::uint32_t width,
    std::uint32_t height,
    VkFormat format,
    VkImageTiling tiling,
    VkImageUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkImage& image,
    VkDeviceMemory& imageMemory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    checkVk(vkCreateImage(device_, &imageInfo, nullptr, &image), "failed to create image");

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(device_, image, &memoryRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, properties);

    checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &imageMemory), "failed to allocate image memory");
    checkVk(vkBindImageMemory(device_, image, imageMemory, 0), "failed to bind image memory");
}

VkImageView VulkanRenderer::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) const {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &imageView), "failed to create image view");
    return imageView;
}

VkCommandBuffer VulkanRenderer::beginSingleTimeCommands() const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer), "failed to allocate one-time command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "failed to begin one-time command buffer");

    return commandBuffer;
}

void VulkanRenderer::endSingleTimeCommands(VkCommandBuffer commandBuffer) const {
    checkVk(vkEndCommandBuffer(commandBuffer), "failed to end one-time command buffer");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    checkVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE), "failed to submit one-time command buffer");
    checkVk(vkQueueWaitIdle(graphicsQueue_), "failed to wait for one-time command buffer");

    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

void VulkanRenderer::transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) const {
    static_cast<void>(format);

    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
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

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::runtime_error("unsupported texture image layout transition");
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage,
        destinationStage,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);

    endSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::copyBufferToImage(VkBuffer buffer, VkImage image, std::uint32_t width, std::uint32_t height) const {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(
        commandBuffer,
        buffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);

    endSingleTimeCommands(commandBuffer);
}

VkFormat VulkanRenderer::findDepthFormat() const {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

VkFormat VulkanRenderer::findTransparencyAccumFormat() const {
    return findSupportedFormat(
        {
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_FORMAT_R16G16B16A16_UNORM,
        },
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
}

VkFormat VulkanRenderer::findTransparencyRevealFormat() const {
    return findSupportedFormat(
        {
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R16G16B16A16_UNORM,
        },
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
}

VkFormat VulkanRenderer::findSupportedFormat(
    const std::vector<VkFormat>& candidates,
    VkImageTiling tiling,
    VkFormatFeatureFlags features) const {
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);

        if (tiling == VK_IMAGE_TILING_LINEAR && (properties.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL && (properties.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    throw std::runtime_error("failed to find supported Vulkan format");
}

} // namespace viewer
