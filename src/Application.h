#pragma once

#include "ApexMaterial.h"
#include "VulkanRenderer.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace viewer {

class Application {
public:
    explicit Application(std::filesystem::path modelPath);
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    ~Application();

    void run();
    void runSmokeTest();
    void importModelFromDialog();
    void importApexMaterialFolderFromDialog();

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void cursorPositionCallback(GLFWwindow* window, double x, double y);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);

    void initializeWindow(bool visible = true);
    void initializeNativeControls();
    void destroyNativeControls();
    void handleKeyInput();
    void renderUi();
    void renderApexMaterialPanel();
    void renderMaterialBindingTable();
    void renderSelectedSlotAlphaControls();
    std::filesystem::path openModelFileDialog() const;
    std::filesystem::path openFolderDialog(const wchar_t* title) const;
    ApexMaterialSet buildApexMaterialSet(const std::filesystem::path& modelOrDirectoryPath, const LoadedModel& model) const;
    void logAndSaveApexMaterialSet(const ApexMaterialSet& materialSet) const;
    void clampUiSelectionState();
    void applyApexMaterialParameters(const ApexMaterialParameters& parameters);
    void updateWindowTitle();
    void rescanApexMaterialSet();
    void applyManualSlotOverride();
    void clearManualSlotOverride();
    void applySelectedSlotAlphaSettings(ApexAlphaMode mode, ApexOpacitySource source, ApexOpacityChannel channel);

    std::filesystem::path modelPath_;
    std::filesystem::path apexScanAnchor_;
    bool apexScanAnchorIsDirectory_ = false;
    LoadedModel currentModel_;
    ApexMaterialSet apexMaterialSet_;
    GLFWwindow* window_ = nullptr;
    void* nativeWindow_ = nullptr;
    std::size_t selectedModelSlotIndex_ = 0;
    std::size_t selectedTextureSlotIndex_ = 0;
    VulkanRenderer renderer_;
    CameraState camera_;
    bool dragging_ = false;
    double lastCursorX_ = 0.0;
    double lastCursorY_ = 0.0;
};

} // namespace viewer
