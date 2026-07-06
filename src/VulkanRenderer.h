#pragma once

#include "ApexMaterial.h"
#include "ViewerMath.h"
#include "Model.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

struct CameraState {
    float yaw = 0.0f;
    float pitch = 0.2f;
    float distance = 4.0f;
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    ~VulkanRenderer();

    void initialize(GLFWwindow* window, LoadedModel model, ApexMaterialSet apexMaterialSet = {});
    void replaceModel(LoadedModel model, ApexMaterialSet apexMaterialSet = {});
    void replaceApexMaterialSet(ApexMaterialSet apexMaterialSet);
    void beginUiFrame();
    void drawFrame(const CameraState& camera);
    void waitIdle() const;
    void notifyFramebufferResized();
    const ApexMaterialSet& apexMaterialSet() const;
    void setApexMaterialParameters(const ApexMaterialParameters& parameters);
    void cleanup();

private:
    struct QueueFamilyIndices {
        std::optional<std::uint32_t> graphicsFamily;
        std::optional<std::uint32_t> presentFamily;

        bool complete() const {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    struct UniformBufferObject {
        Mat4 model;
        Mat4 view;
        Mat4 proj;
        Vec4 lightDirection;
        Vec4 cameraPosition;
        Vec4 apexFlags;
        Vec4 apexFactors0;
        Vec4 apexFactors1;
        Vec4 apexSubsurfaceColor;
        Vec4 apexEmissiveTint;
    };

    struct DrawRange {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t materialIndex = 0;
        ApexAlphaMode alphaMode = ApexAlphaMode::Opaque;
        ApexOpacitySource opacitySource = ApexOpacitySource::One;
        ApexOpacityChannel opacityChannel = ApexOpacityChannel::R;
        Vec3 center{};
    };

    struct DrawPushConstants {
        std::uint32_t materialIndex = 0;
        std::uint32_t alphaMode = 0;
        std::uint32_t opacitySource = 0;
        std::uint32_t opacityChannel = 0;
    };

    struct TextureResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };

    struct AttachmentResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };

    struct TransparencyFramebufferResources {
        AttachmentResource opaqueColor;
        AttachmentResource accum;
        AttachmentResource reveal;
        AttachmentResource additive;
    };

    static constexpr int MaxFramesInFlight = 2;
    static constexpr std::uint32_t MaxApexMaterialSlots = 16;
    static constexpr std::uint32_t ApexTextureDescriptorCount =
        MaxApexMaterialSlots * static_cast<std::uint32_t>(ApexTextureKindCount);

    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapChain();
    void createImageViews();
    void createRenderPass();
    void createDescriptorSetLayout();
    void createGraphicsPipeline();
    void createCompositePipeline();
    void createCommandPool();
    void createDefaultApexTextures();
    void rebuildApexMaterialResources();
    void destroyApexTexture(TextureResource& texture);
    void destroyApexMaterialResources();
    void destroyDefaultApexTextures();
    void createTextureFromPixels(
        std::uint32_t width,
        std::uint32_t height,
        const std::vector<std::uint8_t>& rgbaPixels,
        VkFormat format,
        bool nearest,
        TextureResource& texture);
    void createTextureFromFile(
        const std::filesystem::path& path,
        ApexTextureKind kind,
        TextureResource& texture);
    void updateApexTextureDescriptors();
    void createDepthResources();
    void createTransparencyResources();
    void destroyAttachmentResource(AttachmentResource& attachment);
    void destroyTransparencyResources();
    void createFramebuffers();
    void createVertexBuffer();
    void createIndexBuffer();
    void destroyMeshBuffers();
    void createUniformBuffers();
    void createDescriptorPool();
    void createDescriptorSets();
    void createCompositeDescriptorPool();
    void createCompositeDescriptorSets();
    void destroyCompositeDescriptorPool();
    void createCommandBuffers();
    void createSyncObjects();
    void initializeImGui();
    void shutdownImGui();
    void createImGuiDescriptorPool();
    void destroyImGuiDescriptorPool();

    void recreateSwapChain();
    void cleanupSwapChain();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);
    void updateUniformBuffer(std::uint32_t currentImage, const CameraState& camera);
    void rebuildDrawRanges();

    bool checkValidationLayerSupport() const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const;
    bool isDeviceSuitable(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

    VkShaderModule createShaderModule(const std::vector<char>& code) const;
    std::uint32_t findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    void createImage(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory);
    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) const;
    VkCommandBuffer beginSingleTimeCommands() const;
    void endSingleTimeCommands(VkCommandBuffer commandBuffer) const;
    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) const;
    void copyBufferToImage(VkBuffer buffer, VkImage image, std::uint32_t width, std::uint32_t height) const;
    VkFormat findDepthFormat() const;
    VkFormat findTransparencyAccumFormat() const;
    VkFormat findTransparencyRevealFormat() const;
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) const;

    GLFWwindow* window_ = nullptr;
    LoadedModel model_;
    ApexMaterialSet apexMaterialSet_;
    std::vector<DrawRange> drawRanges_;
    std::unordered_map<std::string, std::uint32_t> apexMaterialSlotLookup_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapChain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapChainExtent_{};
    std::vector<VkImageView> swapChainImageViews_;
    std::vector<VkFramebuffer> swapChainFramebuffers_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compositeDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;
    VkFormat transparencyAccumFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat transparencyRevealFormat_ = VK_FORMAT_UNDEFINED;
    std::vector<TransparencyFramebufferResources> transparencyResources_;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory_ = VK_NULL_HANDLE;

    std::array<VkBuffer, MaxFramesInFlight> uniformBuffers_{};
    std::array<VkDeviceMemory, MaxFramesInFlight> uniformBuffersMemory_{};
    std::array<void*, MaxFramesInFlight> uniformBuffersMapped_{};

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorPool compositeDescriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorPool imguiDescriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MaxFramesInFlight> descriptorSets_{};
    std::vector<VkDescriptorSet> compositeDescriptorSets_;
    std::array<TextureResource, ApexTextureKindCount> defaultApexTextures_{};
    std::vector<TextureResource> apexTextures_;
    std::array<VkDescriptorImageInfo, ApexTextureDescriptorCount> apexTextureDescriptors_{};

    std::array<VkSemaphore, MaxFramesInFlight> imageAvailableSemaphores_{};
    std::array<VkSemaphore, MaxFramesInFlight> renderFinishedSemaphores_{};
    std::array<VkFence, MaxFramesInFlight> inFlightFences_{};
    std::size_t currentFrame_ = 0;
    Vec3 lastCameraPosition_{};
    bool framebufferResized_ = false;
    bool initialized_ = false;
    bool imguiInitialized_ = false;
    bool uiFrameActive_ = false;
};

} // namespace viewer
