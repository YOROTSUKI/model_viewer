#include "Application.h"

#include "Model.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <commdlg.h>
#include <shlobj.h>
#include <windows.h>
#endif

namespace viewer {
namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string shortenedLabel(std::string value, std::size_t maxLength = 36) {
    if (value.size() <= maxLength) {
        return value;
    }
    return "..." + value.substr(value.size() - (maxLength - 3));
}

std::vector<std::string> modelSlotNamesForUi(const LoadedModel& model) {
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const MaterialSlotRange& slot : model.materialSlots) {
        if (slot.name.empty()) {
            continue;
        }
        const std::string key = lowerAscii(slot.name);
        if (seen.insert(key).second) {
            names.push_back(slot.name);
        }
    }
    if (names.empty()) {
        names.push_back(model.name.empty() ? "default" : model.name);
    }
    return names;
}

std::vector<std::string> textureSlotNamesForUi(const ApexMaterialSet& materialSet) {
    std::vector<std::string> names = materialSet.detectedTextureSlots;
    if (!names.empty()) {
        return names;
    }

    std::set<std::string> seen;
    for (const ApexMaterialSlot& slot : materialSet.slots) {
        if (slot.name.empty()) {
            continue;
        }
        const std::string key = lowerAscii(slot.name);
        if (seen.insert(key).second) {
            names.push_back(slot.name);
        }
    }
    if (names.empty()) {
        names.push_back("default");
    }
    return names;
}

ApexMaterialSlot* findApexMaterialSlot(ApexMaterialSet& materialSet, const std::string& slotName) {
    const std::string key = lowerAscii(slotName);
    for (ApexMaterialSlot& slot : materialSet.slots) {
        if (lowerAscii(slot.name) == key) {
            return &slot;
        }
    }
    return nullptr;
}

#ifdef _WIN32
std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return L"Unknown error";
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}
#endif

} // namespace

Application::Application(std::filesystem::path modelPath)
    : modelPath_(std::move(modelPath)) {}

Application::~Application() {
    destroyNativeControls();
    renderer_.cleanup();
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}

void Application::run() {
    initializeWindow();
    LoadedModel loadedModel = loadModelOrFallback(modelPath_);
    apexScanAnchor_ = modelPath_;
    apexScanAnchorIsDirectory_ = false;
    apexMaterialSet_ = buildApexMaterialSet(modelPath_, loadedModel);
    logAndSaveApexMaterialSet(apexMaterialSet_);
    currentModel_ = loadedModel;
    renderer_.initialize(window_, std::move(loadedModel), apexMaterialSet_);
    clampUiSelectionState();
    updateWindowTitle();

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        handleKeyInput();
        renderer_.beginUiFrame();
        renderUi();
        renderer_.drawFrame(camera_);
    }

    renderer_.waitIdle();
}

void Application::runSmokeTest() {
    initializeWindow(false);
    LoadedModel loadedModel = loadModelOrFallback(modelPath_);
    apexScanAnchor_ = modelPath_;
    apexScanAnchorIsDirectory_ = false;
    apexMaterialSet_ = buildApexMaterialSet(modelPath_, loadedModel);
    logAndSaveApexMaterialSet(apexMaterialSet_);
    currentModel_ = loadedModel;
    renderer_.initialize(window_, std::move(loadedModel), apexMaterialSet_);
    renderer_.drawFrame(camera_);
    renderer_.waitIdle();
    std::cout << "[SmokeTest] Vulkan pipeline created and one frame rendered.\n";
}

void Application::initializeWindow(bool visible) {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    if (!visible) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    window_ = glfwCreateWindow(1280, 720, "Vulkan Model Viewer", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetCursorPosCallback(window_, cursorPositionCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwSetScrollCallback(window_, scrollCallback);

    if (visible) {
        initializeNativeControls();
    }
}

void Application::initializeNativeControls() {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window_);
    nativeWindow_ = hwnd;
#endif
}

void Application::destroyNativeControls() {
    nativeWindow_ = nullptr;
}

void Application::handleKeyInput() {
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

void Application::framebufferResizeCallback(GLFWwindow* window, int, int) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->renderer_.notifyFramebufferResized();
    }
}

void Application::cursorPositionCallback(GLFWwindow* window, double x, double y) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || !app->dragging_) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
        app->lastCursorX_ = x;
        app->lastCursorY_ = y;
        return;
    }

    const double deltaX = x - app->lastCursorX_;
    const double deltaY = y - app->lastCursorY_;
    app->lastCursorX_ = x;
    app->lastCursorY_ = y;

    app->camera_.yaw += static_cast<float>(deltaX) * 0.006f;
    app->camera_.pitch += static_cast<float>(deltaY) * 0.006f;
    app->camera_.pitch = std::clamp(app->camera_.pitch, -1.45f, 1.45f);
}

void Application::mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
        app->dragging_ = false;
        return;
    }

    if (action == GLFW_PRESS) {
        app->dragging_ = true;
        glfwGetCursorPos(window, &app->lastCursorX_, &app->lastCursorY_);
    } else if (action == GLFW_RELEASE) {
        app->dragging_ = false;
    }
}

void Application::scrollCallback(GLFWwindow* window, double, double yOffset) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app == nullptr) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
        return;
    }

    app->camera_.distance *= (yOffset > 0.0) ? 0.9f : 1.1f;
    app->camera_.distance = std::clamp(app->camera_.distance, 1.5f, 25.0f);
}

std::filesystem::path Application::openModelFileDialog() const {
#ifdef _WIN32
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW openFileName{};
    openFileName.lStructSize = sizeof(openFileName);
    openFileName.hwndOwner = static_cast<HWND>(nativeWindow_);
    openFileName.lpstrFilter =
        L"Model Files (*.cast;*.obj)\0*.cast;*.obj\0"
        L"Cast Files (*.cast)\0*.cast\0"
        L"Wavefront OBJ Files (*.obj)\0*.obj\0"
        L"All Files (*.*)\0*.*\0";
    openFileName.lpstrFile = filePath;
    openFileName.nMaxFile = MAX_PATH;
    openFileName.lpstrTitle = L"Import Model";
    openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&openFileName) == TRUE) {
        return std::filesystem::path(filePath);
    }

    const DWORD error = CommDlgExtendedError();
    if (error != 0) {
        throw std::runtime_error("model file dialog failed with error " + std::to_string(error));
    }
#endif
    return {};
}

std::filesystem::path Application::openFolderDialog(const wchar_t* title) const {
#ifdef _WIN32
    BROWSEINFOW browseInfo{};
    browseInfo.hwndOwner = static_cast<HWND>(nativeWindow_);
    browseInfo.lpszTitle = title;
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE itemList = SHBrowseForFolderW(&browseInfo);
    if (itemList == nullptr) {
        return {};
    }

    wchar_t folderPath[MAX_PATH] = {};
    const BOOL ok = SHGetPathFromIDListW(itemList, folderPath);
    CoTaskMemFree(itemList);
    if (ok == TRUE) {
        return std::filesystem::path(folderPath);
    }
#else
    static_cast<void>(title);
#endif
    return {};
}

ApexMaterialSet Application::buildApexMaterialSet(const std::filesystem::path& modelOrDirectoryPath, const LoadedModel& model) const {
    if (modelOrDirectoryPath.empty()) {
        ApexMaterialSet materialSet;
        materialSet.parameters.enableApexMaterialMode = false;
        materialSet.slots.push_back({"default"});
        return materialSet;
    }
    return scanApexMaterialsForModel(modelOrDirectoryPath, model);
}

void Application::logAndSaveApexMaterialSet(const ApexMaterialSet& materialSet) const {
    std::cout << formatApexMaterialLog(materialSet);
    saveApexMaterialSidecar(materialSet);
}

void Application::clampUiSelectionState() {
    const std::vector<std::string> modelSlots = modelSlotNamesForUi(currentModel_);
    const std::vector<std::string> textureSlots = textureSlotNamesForUi(apexMaterialSet_);

    selectedModelSlotIndex_ = modelSlots.empty() ? 0 : std::min(selectedModelSlotIndex_, modelSlots.size() - 1);
    selectedTextureSlotIndex_ = textureSlots.empty() ? 0 : std::min(selectedTextureSlotIndex_, textureSlots.size() - 1);
}

void Application::applyApexMaterialParameters(const ApexMaterialParameters& parameters) {
    apexMaterialSet_.parameters = parameters;
    renderer_.setApexMaterialParameters(apexMaterialSet_.parameters);
    saveApexMaterialSidecar(apexMaterialSet_);
}

void Application::renderUi() {
    renderApexMaterialPanel();
}

void Application::renderApexMaterialPanel() {
    clampUiSelectionState();

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Vulkan Model Viewer", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const std::string modelName = modelPath_.empty() ? std::string("<fallback>") : modelPath_.filename().string();
    ImGui::TextUnformatted(shortenedLabel(modelName, 54).c_str());
    ImGui::Separator();

    if (ImGui::Button("Import Model", ImVec2(126.0f, 0.0f))) {
        importModelFromDialog();
    }
    ImGui::SameLine();
    if (ImGui::Button("Apex Folder", ImVec2(126.0f, 0.0f))) {
        importApexMaterialFolderFromDialog();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan", ImVec2(78.0f, 0.0f))) {
        rescanApexMaterialSet();
    }

    if (ImGui::BeginTabBar("ViewerTabs")) {
        if (ImGui::BeginTabItem("Material")) {
            ApexMaterialParameters parameters = apexMaterialSet_.parameters;
            bool changed = false;

            changed |= ImGui::Checkbox("Enable Apex Material Mode", &parameters.enableApexMaterialMode);
            changed |= ImGui::Checkbox("Flip Normal Green", &parameters.flipNormalGreen);
            changed |= ImGui::Checkbox("Enable Subsurface Approximation", &parameters.enableSubsurfaceApproximation);
            changed |= ImGui::Checkbox("Enable Anisotropy", &parameters.enableAnisotropy);

            constexpr ApexMaterialDebugView debugViews[] = {
                ApexMaterialDebugView::FinalLit,
                ApexMaterialDebugView::BaseColor,
                ApexMaterialDebugView::Normal,
                ApexMaterialDebugView::Tangent,
                ApexMaterialDebugView::Roughness,
                ApexMaterialDebugView::SpecularF0,
                ApexMaterialDebugView::AmbientOcclusion,
                ApexMaterialDebugView::Cavity,
                ApexMaterialDebugView::OpacityCoverage,
                ApexMaterialDebugView::AnisotropyDirection,
                ApexMaterialDebugView::Emissive,
                ApexMaterialDebugView::ScatterThickness,
            };
            if (ImGui::BeginCombo("Debug View", apexMaterialDebugViewName(parameters.debugView))) {
                for (ApexMaterialDebugView candidate : debugViews) {
                    const bool selected = parameters.debugView == candidate;
                    if (ImGui::Selectable(apexMaterialDebugViewName(candidate), selected)) {
                        parameters.debugView = candidate;
                        changed = true;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            changed |= ImGui::SliderFloat("Roughness Multiplier", &parameters.roughnessMultiplier, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Specular Multiplier", &parameters.specularMultiplier, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("AO Strength", &parameters.aoStrength, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Cavity Strength", &parameters.cavityStrength, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Emissive Strength", &parameters.emissiveStrength, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Alpha Cutoff", &parameters.alphaCutoff, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Subsurface Strength", &parameters.subsurfaceStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Thickness Scale", &parameters.subsurfaceThicknessScale, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Anisotropy Strength", &parameters.anisotropyStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

            float subsurfaceColor[] = {
                parameters.subsurfaceColor.x,
                parameters.subsurfaceColor.y,
                parameters.subsurfaceColor.z,
            };
            if (ImGui::ColorEdit3("Subsurface Color", subsurfaceColor, ImGuiColorEditFlags_Float)) {
                parameters.subsurfaceColor = {subsurfaceColor[0], subsurfaceColor[1], subsurfaceColor[2]};
                changed = true;
            }

            float emissiveTint[] = {
                parameters.emissiveTint.x,
                parameters.emissiveTint.y,
                parameters.emissiveTint.z,
            };
            if (ImGui::ColorEdit3("Emissive Tint", emissiveTint, ImGuiColorEditFlags_Float)) {
                parameters.emissiveTint = {emissiveTint[0], emissiveTint[1], emissiveTint[2]};
                changed = true;
            }

            if (changed) {
                applyApexMaterialParameters(parameters);
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Bindings")) {
            const std::vector<std::string> modelSlots = modelSlotNamesForUi(currentModel_);
            const std::vector<std::string> textureSlots = textureSlotNamesForUi(apexMaterialSet_);
            clampUiSelectionState();

            const char* currentModelSlot = modelSlots.empty() ? "<none>" : modelSlots[selectedModelSlotIndex_].c_str();
            if (ImGui::BeginCombo("Model Material Slot", currentModelSlot)) {
                for (std::size_t i = 0; i < modelSlots.size(); ++i) {
                    const bool selected = i == selectedModelSlotIndex_;
                    if (ImGui::Selectable(modelSlots[i].c_str(), selected)) {
                        selectedModelSlotIndex_ = i;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            const char* currentTextureSlot = textureSlots.empty() ? "<none>" : textureSlots[selectedTextureSlotIndex_].c_str();
            if (ImGui::BeginCombo("Scanned Texture Slot", currentTextureSlot)) {
                for (std::size_t i = 0; i < textureSlots.size(); ++i) {
                    const bool selected = i == selectedTextureSlotIndex_;
                    if (ImGui::Selectable(textureSlots[i].c_str(), selected)) {
                        selectedTextureSlotIndex_ = i;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (!modelSlots.empty()) {
                const auto overrideIt = apexMaterialSet_.slotOverrides.find(lowerAscii(modelSlots[selectedModelSlotIndex_]));
                if (overrideIt != apexMaterialSet_.slotOverrides.end()) {
                    ImGui::Text("Override: %s", overrideIt->second.c_str());
                } else {
                    ImGui::TextUnformatted("Override: <auto>");
                }
            }

            renderSelectedSlotAlphaControls();
            ImGui::Spacing();

            if (ImGui::Button("Map Selected Slot", ImVec2(150.0f, 0.0f))) {
                applyManualSlotOverride();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Override", ImVec2(130.0f, 0.0f))) {
                clearManualSlotOverride();
            }

            ImGui::Spacing();
            renderMaterialBindingTable();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Log")) {
            ImGui::Text("Slots: %zu", apexMaterialSet_.slots.size());
            ImGui::SameLine();
            ImGui::Text("Detected: %zu", apexMaterialSet_.detectedTextureSlots.size());
            ImGui::BeginChild("ApexLog", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& line : apexMaterialSet_.logLines) {
                ImGui::TextUnformatted(line.c_str());
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void Application::renderSelectedSlotAlphaControls() {
    const std::vector<std::string> modelSlots = modelSlotNamesForUi(currentModel_);
    if (modelSlots.empty()) {
        return;
    }

    selectedModelSlotIndex_ = std::min(selectedModelSlotIndex_, modelSlots.size() - 1);
    ApexMaterialSlot* slot = findApexMaterialSlot(apexMaterialSet_, modelSlots[selectedModelSlotIndex_]);
    if (slot == nullptr) {
        ImGui::TextUnformatted("Alpha: <no matched Apex slot>");
        return;
    }

    ApexAlphaMode mode = slot->alphaMode;
    ApexOpacitySource source = slot->opacitySource;
    ApexOpacityChannel channel = slot->opacityChannel;
    bool changed = false;

    constexpr ApexAlphaMode alphaModes[] = {
        ApexAlphaMode::Opaque,
        ApexAlphaMode::Masked,
        ApexAlphaMode::Translucent,
        ApexAlphaMode::Additive,
    };
    if (ImGui::BeginCombo("Alpha Mode", apexAlphaModeName(mode))) {
        for (ApexAlphaMode candidate : alphaModes) {
            const bool selected = mode == candidate;
            if (ImGui::Selectable(apexAlphaModeName(candidate), selected)) {
                mode = candidate;
                if (mode == ApexAlphaMode::Opaque) {
                    source = ApexOpacitySource::One;
                } else if (source == ApexOpacitySource::One) {
                    source = slot->hasTexture[static_cast<std::size_t>(ApexTextureKind::Opacity)]
                        ? ApexOpacitySource::OpacityTexture
                        : ApexOpacitySource::AlbedoAlpha;
                }
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    constexpr ApexOpacitySource opacitySources[] = {
        ApexOpacitySource::One,
        ApexOpacitySource::OpacityTexture,
        ApexOpacitySource::AlbedoAlpha,
        ApexOpacitySource::MinAlbedoAndOpacity,
    };
    if (mode == ApexAlphaMode::Opaque) {
        source = ApexOpacitySource::One;
    }
    ImGui::BeginDisabled(mode == ApexAlphaMode::Opaque);
    if (ImGui::BeginCombo("Opacity Source", apexOpacitySourceName(source))) {
        for (ApexOpacitySource candidate : opacitySources) {
            const bool selected = source == candidate;
            if (ImGui::Selectable(apexOpacitySourceName(candidate), selected)) {
                source = candidate;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    constexpr ApexOpacityChannel opacityChannels[] = {
        ApexOpacityChannel::R,
        ApexOpacityChannel::G,
        ApexOpacityChannel::B,
        ApexOpacityChannel::A,
    };
    if (ImGui::BeginCombo("Opacity Channel", apexOpacityChannelName(channel))) {
        for (ApexOpacityChannel candidate : opacityChannels) {
            const bool selected = channel == candidate;
            if (ImGui::Selectable(apexOpacityChannelName(candidate), selected)) {
                channel = candidate;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (changed) {
        applySelectedSlotAlphaSettings(mode, source, channel);
    }
}

void Application::renderMaterialBindingTable() {
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable("ApexMaterialBindings", 5, flags, ImVec2(0.0f, 320.0f))) {
        return;
    }

    ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Alpha", ImGuiTableColumnFlags_WidthFixed, 92.0f);
    ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 74.0f);
    ImGui::TableSetupColumn("Surface", ImGuiTableColumnFlags_WidthFixed, 92.0f);
    ImGui::TableSetupColumn("Extra", ImGuiTableColumnFlags_WidthFixed, 96.0f);
    ImGui::TableHeadersRow();

    for (const ApexMaterialSlot& slot : apexMaterialSet_.slots) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(shortenedLabel(slot.name, 34).c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s/%s", apexAlphaModeName(slot.alphaMode), apexOpacityChannelName(slot.opacityChannel));

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s%s",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Albedo)] ? "COL " : "",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Emissive)] ? "EMI" : "");

        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%s%s%s",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Normal)] ? "NML " : "",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Gloss)] ? "GLS " : "",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Specular)] ? "SPC" : "");

        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%s%s%s%s",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::AmbientOcclusion)] ? "AO " : "",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Cavity)] ? "CAV " : "",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Opacity)] ? "MSK " : "",
                    slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::ScatterThickness)] ? "THK" : "");
    }

    ImGui::EndTable();
}

void Application::importApexMaterialFolderFromDialog() {
    try {
        const std::filesystem::path selectedPath = openFolderDialog(L"Select Apex Texture Folder");
        if (selectedPath.empty()) {
            return;
        }

        apexMaterialSet_ = scanApexMaterialsInDirectory(selectedPath, currentModel_, modelPath_);
        apexScanAnchor_ = selectedPath;
        apexScanAnchorIsDirectory_ = true;
        logAndSaveApexMaterialSet(apexMaterialSet_);
        renderer_.replaceApexMaterialSet(apexMaterialSet_);
        clampUiSelectionState();
    } catch (const std::exception& ex) {
        std::cerr << "Apex material folder import failed: " << ex.what() << '\n';
#ifdef _WIN32
        const std::wstring message = L"Apex material folder import failed:\n" + widenUtf8(ex.what());
        MessageBoxW(static_cast<HWND>(nativeWindow_), message.c_str(), L"Vulkan Model Viewer", MB_ICONERROR | MB_OK);
#endif
    }
}

void Application::rescanApexMaterialSet() {
    if (apexScanAnchorIsDirectory_) {
        apexMaterialSet_ = scanApexMaterialsInDirectory(apexScanAnchor_, currentModel_, modelPath_);
    } else {
        apexMaterialSet_ = buildApexMaterialSet(modelPath_, currentModel_);
    }

    logAndSaveApexMaterialSet(apexMaterialSet_);
    renderer_.replaceApexMaterialSet(apexMaterialSet_);
    clampUiSelectionState();
}

void Application::applyManualSlotOverride() {
    const std::vector<std::string> modelSlots = modelSlotNamesForUi(currentModel_);
    const std::vector<std::string> textureSlots = textureSlotNamesForUi(apexMaterialSet_);
    if (modelSlots.empty() || textureSlots.empty()) {
        return;
    }

    selectedModelSlotIndex_ = std::min(selectedModelSlotIndex_, modelSlots.size() - 1);
    selectedTextureSlotIndex_ = std::min(selectedTextureSlotIndex_, textureSlots.size() - 1);

    const std::string& modelSlot = modelSlots[selectedModelSlotIndex_];
    const std::string& textureSlot = textureSlots[selectedTextureSlotIndex_];
    apexMaterialSet_.slotOverrides[lowerAscii(modelSlot)] = textureSlot;
    std::cout << "[ApexMaterial] UI override: " << modelSlot << " -> " << textureSlot << '\n';
    saveApexMaterialSidecar(apexMaterialSet_);
    rescanApexMaterialSet();
}

void Application::clearManualSlotOverride() {
    const std::vector<std::string> modelSlots = modelSlotNamesForUi(currentModel_);
    if (modelSlots.empty()) {
        return;
    }

    selectedModelSlotIndex_ = std::min(selectedModelSlotIndex_, modelSlots.size() - 1);
    const std::string& modelSlot = modelSlots[selectedModelSlotIndex_];
    const std::size_t erased = apexMaterialSet_.slotOverrides.erase(lowerAscii(modelSlot));
    std::cout << "[ApexMaterial] UI override clear: " << modelSlot
              << (erased == 0 ? " (no override)" : "") << '\n';
    saveApexMaterialSidecar(apexMaterialSet_);
    rescanApexMaterialSet();
}

void Application::applySelectedSlotAlphaSettings(ApexAlphaMode mode, ApexOpacitySource source, ApexOpacityChannel channel) {
    const std::vector<std::string> modelSlots = modelSlotNamesForUi(currentModel_);
    if (modelSlots.empty()) {
        return;
    }

    selectedModelSlotIndex_ = std::min(selectedModelSlotIndex_, modelSlots.size() - 1);
    const std::string& modelSlot = modelSlots[selectedModelSlotIndex_];
    ApexMaterialSlot* slot = findApexMaterialSlot(apexMaterialSet_, modelSlot);
    if (slot == nullptr) {
        return;
    }

    if (mode == ApexAlphaMode::Opaque) {
        source = ApexOpacitySource::One;
    }

    slot->alphaMode = mode;
    slot->opacitySource = source;
    slot->opacityChannel = channel;

    ApexSlotAlphaOverride& overrideValue = apexMaterialSet_.slotAlphaOverrides[lowerAscii(modelSlot)];
    overrideValue.mode = mode;
    overrideValue.source = source;
    overrideValue.channel = channel;

    std::cout << "[ApexMaterial] UI alpha override: " << modelSlot
              << " mode=" << apexAlphaModeName(mode)
              << " source=" << apexOpacitySourceName(source)
              << " channel=" << apexOpacityChannelName(channel)
              << '\n';
    saveApexMaterialSidecar(apexMaterialSet_);
    renderer_.replaceApexMaterialSet(apexMaterialSet_);
}

void Application::importModelFromDialog() {
    try {
        const std::filesystem::path selectedPath = openModelFileDialog();
        if (selectedPath.empty()) {
            return;
        }

        LoadedModel model = loadModelOrFallback(selectedPath);
        apexScanAnchor_ = selectedPath;
        apexScanAnchorIsDirectory_ = false;
        ApexMaterialSet materialSet = buildApexMaterialSet(selectedPath, model);
        logAndSaveApexMaterialSet(materialSet);
        currentModel_ = model;
        apexMaterialSet_ = std::move(materialSet);
        renderer_.replaceModel(std::move(model), apexMaterialSet_);
        modelPath_ = selectedPath;
        camera_ = CameraState{};
        clampUiSelectionState();
        updateWindowTitle();
    } catch (const std::exception& ex) {
        std::cerr << "Import failed: " << ex.what() << '\n';
#ifdef _WIN32
        const std::wstring message = L"Import failed:\n" + widenUtf8(ex.what());
        MessageBoxW(static_cast<HWND>(nativeWindow_), message.c_str(), L"Vulkan Model Viewer", MB_ICONERROR | MB_OK);
#endif
    }
}

void Application::updateWindowTitle() {
    std::string title = "Vulkan Model Viewer";
    if (!modelPath_.empty()) {
        title += " - " + modelPath_.filename().string();
    }

#ifdef _WIN32
    if (nativeWindow_ != nullptr) {
        std::wstring wideTitle = L"Vulkan Model Viewer";
        if (!modelPath_.empty()) {
            wideTitle += L" - ";
            wideTitle += modelPath_.filename().wstring();
        }
        SetWindowTextW(static_cast<HWND>(nativeWindow_), wideTitle.c_str());
        return;
    }
#endif

    if (window_ != nullptr) {
        glfwSetWindowTitle(window_, title.c_str());
    }
}

} // namespace viewer
