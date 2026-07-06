#pragma once

#include "Model.h"
#include "ViewerMath.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

enum class ApexTextureKind : std::uint32_t {
    Albedo = 0,
    Normal,
    Gloss,
    Specular,
    AmbientOcclusion,
    Cavity,
    Opacity,
    ScatterThickness,
    Anisotropy,
    Emissive,
    EmissiveMultiply,
    Count,
};

constexpr std::size_t ApexTextureKindCount = static_cast<std::size_t>(ApexTextureKind::Count);

enum class ApexAlphaMode : std::uint32_t {
    Opaque = 0,
    Masked,
    Translucent,
    Additive,
};

enum class ApexOpacitySource : std::uint32_t {
    One = 0,
    OpacityTexture,
    AlbedoAlpha,
    MinAlbedoAndOpacity,
};

enum class ApexOpacityChannel : std::uint32_t {
    R = 0,
    G,
    B,
    A,
};

enum class ApexMaterialDebugView : std::uint32_t {
    FinalLit = 0,
    BaseColor,
    Normal,
    Tangent,
    Roughness,
    SpecularF0,
    AmbientOcclusion,
    Cavity,
    OpacityCoverage,
    AnisotropyDirection,
    Emissive,
    ScatterThickness,
    Count,
};

struct ApexMaterialParameters {
    bool enableApexMaterialMode = false;
    bool flipNormalGreen = false;
    bool enableSubsurfaceApproximation = true;
    bool enableAnisotropy = false;
    float roughnessMultiplier = 1.0f;
    float specularMultiplier = 1.0f;
    float aoStrength = 1.0f;
    float cavityStrength = 1.0f;
    float emissiveStrength = 1.0f;
    float alphaCutoff = 0.33f;
    Vec3 subsurfaceColor{1.0f, 0.45f, 0.28f};
    float subsurfaceStrength = 0.18f;
    float subsurfaceThicknessScale = 1.0f;
    float anisotropyStrength = 0.25f;
    Vec3 emissiveTint{1.0f, 1.0f, 1.0f};
    ApexMaterialDebugView debugView = ApexMaterialDebugView::FinalLit;
};

struct ApexMaterialSlot {
    std::string name;
    std::array<std::filesystem::path, ApexTextureKindCount> textures{};
    std::array<bool, ApexTextureKindCount> hasTexture{};
    ApexAlphaMode alphaMode = ApexAlphaMode::Opaque;
    ApexOpacitySource opacitySource = ApexOpacitySource::One;
    ApexOpacityChannel opacityChannel = ApexOpacityChannel::R;
};

struct ApexSlotAlphaOverride {
    std::optional<ApexAlphaMode> mode;
    std::optional<ApexOpacitySource> source;
    std::optional<ApexOpacityChannel> channel;
};

struct ApexTextureNameMatch {
    std::string materialSlotName;
    std::string textureTypeToken;
    ApexTextureKind kind = ApexTextureKind::Albedo;
};

struct ApexMaterialSet {
    std::filesystem::path sourceDirectory;
    std::filesystem::path sidecarPath;
    ApexMaterialParameters parameters;
    std::vector<ApexMaterialSlot> slots;
    std::vector<std::string> detectedTextureSlots;
    std::vector<std::string> logLines;
    std::unordered_map<std::string, std::string> slotOverrides;
    std::unordered_map<std::string, ApexSlotAlphaOverride> slotAlphaOverrides;
};

const char* apexTextureKindName(ApexTextureKind kind);
const char* apexAlphaModeName(ApexAlphaMode mode);
const char* apexOpacitySourceName(ApexOpacitySource source);
const char* apexOpacityChannelName(ApexOpacityChannel channel);
const char* apexMaterialDebugViewName(ApexMaterialDebugView view);
bool apexTextureKindUsesSrgb(ApexTextureKind kind);
bool apexTextureKindPrefersNearest(ApexTextureKind kind);
std::optional<ApexTextureKind> apexTextureKindFromToken(std::string token);
std::optional<ApexAlphaMode> apexAlphaModeFromString(std::string value);
std::optional<ApexOpacitySource> apexOpacitySourceFromString(std::string value);
std::optional<ApexOpacityChannel> apexOpacityChannelFromString(std::string value);
std::optional<ApexMaterialDebugView> apexMaterialDebugViewFromString(std::string value);
std::optional<ApexTextureNameMatch> parseApexTextureName(const std::filesystem::path& path);

ApexMaterialSet scanApexMaterialsForModel(const std::filesystem::path& modelOrDirectoryPath, const LoadedModel& model);
ApexMaterialSet scanApexMaterialsInDirectory(
    const std::filesystem::path& directory,
    const LoadedModel& model,
    const std::filesystem::path& sidecarAnchorPath = {});

void saveApexMaterialSidecar(const ApexMaterialSet& materialSet);
std::string formatApexMaterialLog(const ApexMaterialSet& materialSet);

} // namespace viewer
