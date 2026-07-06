#include "ApexMaterial.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace viewer {
namespace {

struct TokenMapping {
    const char* token;
    ApexTextureKind kind;
};

constexpr TokenMapping TextureTokenMappings[] = {
    {"albedotexture", ApexTextureKind::Albedo},
    {"col", ApexTextureKind::Albedo},
    {"normaltexture", ApexTextureKind::Normal},
    {"nml", ApexTextureKind::Normal},
    {"glosstexture", ApexTextureKind::Gloss},
    {"gls", ApexTextureKind::Gloss},
    {"spectexture", ApexTextureKind::Specular},
    {"spc", ApexTextureKind::Specular},
    {"aotexture", ApexTextureKind::AmbientOcclusion},
    {"ao", ApexTextureKind::AmbientOcclusion},
    {"cavitytexture", ApexTextureKind::Cavity},
    {"cvt", ApexTextureKind::Cavity},
    {"cav", ApexTextureKind::Cavity},
    {"opacitymultiplytexture", ApexTextureKind::Opacity},
    {"opa", ApexTextureKind::Opacity},
    {"msk", ApexTextureKind::Opacity},
    {"scatterthicknesstexture", ApexTextureKind::ScatterThickness},
    {"scattertickeness", ApexTextureKind::ScatterThickness},
    {"thk", ApexTextureKind::ScatterThickness},
    {"sctr", ApexTextureKind::ScatterThickness},
    {"anisospecdirtexture", ApexTextureKind::Anisotropy},
    {"asd", ApexTextureKind::Anisotropy},
    {"emissivetexture", ApexTextureKind::Emissive},
    {"ilm", ApexTextureKind::Emissive},
    {"ehl", ApexTextureKind::Emissive},
    {"emissivemultiplytexture", ApexTextureKind::EmissiveMultiply},
    {"ehm", ApexTextureKind::EmissiveMultiply},
};

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool isAllDigits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

std::string stripLeadingTexturePrefix(std::string value) {
    if (startsWith(value, "T_") || startsWith(value, "t_")) {
        return value.substr(2);
    }
    return value;
}

bool isTextureExtension(const std::filesystem::path& path) {
    const std::string extension = toLower(path.extension().string());
    return extension == ".png" ||
           extension == ".jpg" ||
           extension == ".jpeg" ||
           extension == ".bmp" ||
           extension == ".tif" ||
           extension == ".tiff" ||
           extension == ".tga" ||
           extension == ".dds";
}

std::filesystem::path makeSidecarPath(const std::filesystem::path& anchorPath) {
    if (anchorPath.empty()) {
        return {};
    }

    std::error_code error;
    if (std::filesystem::is_directory(anchorPath, error)) {
        const std::string name = anchorPath.filename().empty() ? "model" : anchorPath.filename().string();
        return anchorPath / (name + ".apexmat.json");
    }

    std::filesystem::path sidecarPath = anchorPath;
    sidecarPath.replace_extension(".apexmat.json");
    return sidecarPath;
}

std::vector<std::filesystem::path> makeCandidateTextureDirectories(const std::filesystem::path& modelOrDirectoryPath) {
    std::vector<std::filesystem::path> candidates;
    const auto add = [&candidates](const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
            candidates.push_back(path);
        }
    };

    std::error_code error;
    if (std::filesystem::is_directory(modelOrDirectoryPath, error)) {
        add(modelOrDirectoryPath);
        add(modelOrDirectoryPath / "Textures");
        add(modelOrDirectoryPath / "textures");
        add(modelOrDirectoryPath / "images");
        add(modelOrDirectoryPath / "material");
        return candidates;
    }

    std::filesystem::path current = modelOrDirectoryPath.parent_path();
    add(current);
    add(current / "Textures");
    add(current / "textures");
    add(current / "images");

    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        add(current / "Textures");
        add(current / "textures");
        add(current / "images");
        add(current / "material");

        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return candidates;
}

std::vector<std::filesystem::path> collectTextureFiles(const std::vector<std::filesystem::path>& directories, std::vector<std::string>& logLines) {
    std::vector<std::filesystem::path> files;
    std::set<std::filesystem::path> seen;

    for (const std::filesystem::path& directory : directories) {
        std::error_code error;
        if (!std::filesystem::exists(directory, error) || !std::filesystem::is_directory(directory, error)) {
            continue;
        }

        logLines.push_back("[ApexMaterial] scanning texture directory: " + directory.string());
        for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && it != end;
             it.increment(error)) {
            if (!it->is_regular_file(error) || !isTextureExtension(it->path())) {
                continue;
            }
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(it->path(), error);
            const std::filesystem::path key = error ? it->path() : canonical;
            if (seen.insert(key).second) {
                files.push_back(it->path());
            }
        }
        if (error) {
            logLines.push_back("[ApexMaterial] warning: stopped scanning " + directory.string() + ": " + error.message());
            error.clear();
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> expectedMaterialSlotNames(const LoadedModel& model) {
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (const MaterialSlotRange& range : model.materialSlots) {
        if (range.name.empty()) {
            continue;
        }
        const std::string key = toLower(range.name);
        if (seen.insert(key).second) {
            names.push_back(range.name);
        }
    }

    if (!names.empty()) {
        return names;
    }

    std::string fallback = model.name.empty() ? "default" : model.name;
    const std::size_t dot = fallback.find_last_of('.');
    if (dot != std::string::npos) {
        fallback = fallback.substr(0, dot);
    }
    for (const char* suffix : {"_LOD0", "_lod0", "_LOD1", "_lod1", "_LOD2", "_lod2"}) {
        if (endsWith(fallback, suffix)) {
            fallback.resize(fallback.size() - std::char_traits<char>::length(suffix));
            break;
        }
    }
    names.push_back(fallback);
    return names;
}

std::optional<double> findJsonNumber(const std::string& json, const char* name) {
    const std::regex pattern(std::string("\"") + name + R"("\s*:\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?))");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return std::stod(match[1].str());
    }
    return std::nullopt;
}

std::optional<bool> findJsonBool(const std::string& json, const char* name) {
    const std::regex pattern(std::string("\"") + name + R"("\s*:\s*(true|false|1|0))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        const std::string value = toLower(match[1].str());
        return value == "true" || value == "1";
    }
    return std::nullopt;
}

std::optional<std::string> findJsonString(const std::string& json, const char* name) {
    const std::regex pattern(std::string("\"") + name + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return match[1].str();
    }
    return std::nullopt;
}

std::optional<Vec3> findJsonVec3(const std::string& json, const char* name) {
    const std::regex pattern(std::string("\"") + name +
                             R"("\s*:\s*\[\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*,\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*,\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*\])");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return Vec3{
            static_cast<float>(std::stod(match[1].str())),
            static_cast<float>(std::stod(match[2].str())),
            static_cast<float>(std::stod(match[3].str())),
        };
    }
    return std::nullopt;
}

std::unordered_map<std::string, std::string> findJsonSlotOverrides(const std::string& json) {
    std::unordered_map<std::string, std::string> overrides;
    const std::string key = "\"slotOverrides\"";
    const std::size_t keyPos = json.find(key);
    if (keyPos == std::string::npos) {
        return overrides;
    }

    const std::size_t objectStart = json.find('{', keyPos + key.size());
    if (objectStart == std::string::npos) {
        return overrides;
    }

    int depth = 0;
    std::size_t objectEnd = std::string::npos;
    for (std::size_t i = objectStart; i < json.size(); ++i) {
        if (json[i] == '{') {
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0) {
                objectEnd = i;
                break;
            }
        }
    }
    if (objectEnd == std::string::npos || objectEnd <= objectStart + 1) {
        return overrides;
    }

    const std::string objectText = json.substr(objectStart + 1, objectEnd - objectStart - 1);
    const std::regex entryPattern("\"([^\"]+)\"\\s*:\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(objectText.begin(), objectText.end(), entryPattern), end; it != end; ++it) {
        overrides.emplace(toLower((*it)[1].str()), (*it)[2].str());
    }
    return overrides;
}

std::unordered_map<std::string, std::string> findJsonStringMap(const std::string& json, const char* name) {
    std::unordered_map<std::string, std::string> values;
    const std::string key = std::string("\"") + name + "\"";
    const std::size_t keyPos = json.find(key);
    if (keyPos == std::string::npos) {
        return values;
    }

    const std::size_t objectStart = json.find('{', keyPos + key.size());
    if (objectStart == std::string::npos) {
        return values;
    }

    int depth = 0;
    std::size_t objectEnd = std::string::npos;
    for (std::size_t i = objectStart; i < json.size(); ++i) {
        if (json[i] == '{') {
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0) {
                objectEnd = i;
                break;
            }
        }
    }
    if (objectEnd == std::string::npos || objectEnd <= objectStart + 1) {
        return values;
    }

    const std::string objectText = json.substr(objectStart + 1, objectEnd - objectStart - 1);
    const std::regex entryPattern("\"([^\"]+)\"\\s*:\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(objectText.begin(), objectText.end(), entryPattern), end; it != end; ++it) {
        values.emplace(toLower((*it)[1].str()), (*it)[2].str());
    }
    return values;
}

std::unordered_map<std::string, ApexSlotAlphaOverride> findJsonSlotAlphaOverrides(const std::string& json) {
    std::unordered_map<std::string, ApexSlotAlphaOverride> overrides;

    for (const auto& item : findJsonStringMap(json, "slotAlphaModes")) {
        if (const std::optional<ApexAlphaMode> mode = apexAlphaModeFromString(item.second)) {
            overrides[item.first].mode = *mode;
        }
    }
    for (const auto& item : findJsonStringMap(json, "slotOpacitySources")) {
        if (const std::optional<ApexOpacitySource> source = apexOpacitySourceFromString(item.second)) {
            overrides[item.first].source = *source;
        }
    }
    for (const auto& item : findJsonStringMap(json, "slotOpacityChannels")) {
        if (const std::optional<ApexOpacityChannel> channel = apexOpacityChannelFromString(item.second)) {
            overrides[item.first].channel = *channel;
        }
    }

    return overrides;
}

bool containsAnyToken(const std::string& value, std::initializer_list<const char*> tokens) {
    for (const char* token : tokens) {
        if (value.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

ApexAlphaMode inferAlphaMode(const ApexMaterialSlot& slot) {
    const std::string name = toLower(slot.name);
    if (containsAnyToken(name, {"glow", "wisp", "lightning", "quasar", "energy", "electric", "spark", "beam"})) {
        return ApexAlphaMode::Additive;
    }
    if (containsAnyToken(name, {"glass", "visor", "lens", "screen", "holo"})) {
        return ApexAlphaMode::Translucent;
    }
    if (containsAnyToken(name, {"hair", "lash", "brow", "fur", "leaf", "foliage", "decal", "card"})) {
        return ApexAlphaMode::Masked;
    }
    return ApexAlphaMode::Opaque;
}

ApexOpacitySource inferOpacitySource(const ApexMaterialSlot& slot, ApexAlphaMode mode) {
    if (mode == ApexAlphaMode::Opaque) {
        return ApexOpacitySource::One;
    }
    if (slot.hasTexture[static_cast<std::size_t>(ApexTextureKind::Opacity)]) {
        return ApexOpacitySource::OpacityTexture;
    }
    return ApexOpacitySource::AlbedoAlpha;
}

void applySlotAlphaSettings(ApexMaterialSet& materialSet) {
    for (ApexMaterialSlot& slot : materialSet.slots) {
        slot.alphaMode = inferAlphaMode(slot);
        slot.opacitySource = inferOpacitySource(slot, slot.alphaMode);
        slot.opacityChannel = ApexOpacityChannel::R;

        if (const auto overrideIt = materialSet.slotAlphaOverrides.find(toLower(slot.name)); overrideIt != materialSet.slotAlphaOverrides.end()) {
            if (overrideIt->second.mode.has_value()) {
                slot.alphaMode = *overrideIt->second.mode;
            }
            if (overrideIt->second.source.has_value()) {
                slot.opacitySource = *overrideIt->second.source;
            } else {
                slot.opacitySource = inferOpacitySource(slot, slot.alphaMode);
            }
            if (overrideIt->second.channel.has_value()) {
                slot.opacityChannel = *overrideIt->second.channel;
            }
        }

        if (slot.alphaMode == ApexAlphaMode::Opaque) {
            slot.opacitySource = ApexOpacitySource::One;
        }

        materialSet.logLines.push_back(
            "[ApexMaterial] alpha '" + slot.name +
            "': mode=" + apexAlphaModeName(slot.alphaMode) +
            ", source=" + apexOpacitySourceName(slot.opacitySource) +
            ", channel=" + apexOpacityChannelName(slot.opacityChannel));
    }
}

ApexMaterialParameters loadApexMaterialSidecar(const std::filesystem::path& sidecarPath, bool& found, std::vector<std::string>& logLines) {
    found = false;
    ApexMaterialParameters parameters;
    if (sidecarPath.empty()) {
        return parameters;
    }

    std::ifstream input(sidecarPath);
    if (!input) {
        return parameters;
    }

    found = true;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string json = buffer.str();

    if (auto value = findJsonBool(json, "enableApexMaterialMode")) {
        parameters.enableApexMaterialMode = *value;
    }
    if (auto value = findJsonBool(json, "flipNormalGreen")) {
        parameters.flipNormalGreen = *value;
    }
    if (auto value = findJsonBool(json, "enableSubsurfaceApproximation")) {
        parameters.enableSubsurfaceApproximation = *value;
    }
    if (auto value = findJsonBool(json, "enableAnisotropy")) {
        parameters.enableAnisotropy = *value;
    }
    if (auto value = findJsonNumber(json, "roughnessMultiplier")) {
        parameters.roughnessMultiplier = static_cast<float>(*value);
    }
    if (auto value = findJsonNumber(json, "specularMultiplier")) {
        parameters.specularMultiplier = static_cast<float>(*value);
    }
    if (auto value = findJsonNumber(json, "aoStrength")) {
        parameters.aoStrength = static_cast<float>(*value);
    }
    if (auto value = findJsonNumber(json, "cavityStrength")) {
        parameters.cavityStrength = static_cast<float>(*value);
    }
    if (auto value = findJsonNumber(json, "emissiveStrength")) {
        parameters.emissiveStrength = static_cast<float>(*value);
    }
    if (auto value = findJsonNumber(json, "alphaCutoff")) {
        parameters.alphaCutoff = static_cast<float>(*value);
    }
    if (auto value = findJsonVec3(json, "subsurfaceColor")) {
        parameters.subsurfaceColor = *value;
    }
    if (auto value = findJsonNumber(json, "subsurfaceStrength")) {
        parameters.subsurfaceStrength = static_cast<float>(*value);
    }
    if (auto value = findJsonNumber(json, "subsurfaceThicknessScale")) {
        parameters.subsurfaceThicknessScale = static_cast<float>(*value);
    }
    if (auto value = findJsonNumber(json, "anisotropyStrength")) {
        parameters.anisotropyStrength = static_cast<float>(*value);
    }
    if (auto value = findJsonVec3(json, "emissiveTint")) {
        parameters.emissiveTint = *value;
    }
    if (auto debugViewString = findJsonString(json, "debugView")) {
        if (auto view = apexMaterialDebugViewFromString(*debugViewString)) {
            parameters.debugView = *view;
        }
    } else if (auto debugViewNumber = findJsonNumber(json, "debugView")) {
        const auto index = static_cast<std::uint32_t>(*debugViewNumber);
        if (index < static_cast<std::uint32_t>(ApexMaterialDebugView::Count)) {
            parameters.debugView = static_cast<ApexMaterialDebugView>(index);
        }
    }

    logLines.push_back("[ApexMaterial] loaded sidecar: " + sidecarPath.string());
    return parameters;
}

std::size_t texturePresenceScore(const ApexMaterialSlot& slot) {
    std::size_t score = 0;
    for (bool present : slot.hasTexture) {
        if (present) {
            ++score;
        }
    }
    return score;
}

} // namespace

const char* apexTextureKindName(ApexTextureKind kind) {
    switch (kind) {
    case ApexTextureKind::Albedo:
        return "Albedo";
    case ApexTextureKind::Normal:
        return "Normal";
    case ApexTextureKind::Gloss:
        return "Gloss";
    case ApexTextureKind::Specular:
        return "Specular";
    case ApexTextureKind::AmbientOcclusion:
        return "AO";
    case ApexTextureKind::Cavity:
        return "Cavity";
    case ApexTextureKind::Opacity:
        return "Opacity";
    case ApexTextureKind::ScatterThickness:
        return "ScatterThickness";
    case ApexTextureKind::Anisotropy:
        return "Anisotropy";
    case ApexTextureKind::Emissive:
        return "Emissive";
    case ApexTextureKind::EmissiveMultiply:
        return "EmissiveMultiply";
    case ApexTextureKind::Count:
        break;
    }
    return "Unknown";
}

const char* apexAlphaModeName(ApexAlphaMode mode) {
    switch (mode) {
    case ApexAlphaMode::Opaque:
        return "Opaque";
    case ApexAlphaMode::Masked:
        return "Masked";
    case ApexAlphaMode::Translucent:
        return "Translucent";
    case ApexAlphaMode::Additive:
        return "Additive";
    }
    return "Opaque";
}

const char* apexOpacitySourceName(ApexOpacitySource source) {
    switch (source) {
    case ApexOpacitySource::One:
        return "One";
    case ApexOpacitySource::OpacityTexture:
        return "OpacityTexture";
    case ApexOpacitySource::AlbedoAlpha:
        return "AlbedoAlpha";
    case ApexOpacitySource::MinAlbedoAndOpacity:
        return "MinAlbedoAndOpacity";
    }
    return "One";
}

const char* apexOpacityChannelName(ApexOpacityChannel channel) {
    switch (channel) {
    case ApexOpacityChannel::R:
        return "R";
    case ApexOpacityChannel::G:
        return "G";
    case ApexOpacityChannel::B:
        return "B";
    case ApexOpacityChannel::A:
        return "A";
    }
    return "R";
}

const char* apexMaterialDebugViewName(ApexMaterialDebugView view) {
    switch (view) {
    case ApexMaterialDebugView::FinalLit:
        return "Final Lit";
    case ApexMaterialDebugView::BaseColor:
        return "Base Color";
    case ApexMaterialDebugView::Normal:
        return "Normal";
    case ApexMaterialDebugView::Tangent:
        return "Tangent";
    case ApexMaterialDebugView::Roughness:
        return "Roughness";
    case ApexMaterialDebugView::SpecularF0:
        return "Specular/F0";
    case ApexMaterialDebugView::AmbientOcclusion:
        return "AO";
    case ApexMaterialDebugView::Cavity:
        return "Cavity";
    case ApexMaterialDebugView::OpacityCoverage:
        return "Opacity/Coverage";
    case ApexMaterialDebugView::AnisotropyDirection:
        return "Anisotropy Direction";
    case ApexMaterialDebugView::Emissive:
        return "Emissive";
    case ApexMaterialDebugView::ScatterThickness:
        return "Scatter/Thickness";
    case ApexMaterialDebugView::Count:
        break;
    }
    return "Final Lit";
}

bool apexTextureKindUsesSrgb(ApexTextureKind kind) {
    return kind == ApexTextureKind::Albedo ||
           kind == ApexTextureKind::Emissive;
}

bool apexTextureKindPrefersNearest(ApexTextureKind kind) {
    return kind == ApexTextureKind::Anisotropy;
}

std::optional<ApexTextureKind> apexTextureKindFromToken(std::string token) {
    token = toLower(std::move(token));
    for (const TokenMapping& mapping : TextureTokenMappings) {
        if (token == mapping.token) {
            return mapping.kind;
        }
    }
    return std::nullopt;
}

std::optional<ApexAlphaMode> apexAlphaModeFromString(std::string value) {
    value = toLower(std::move(value));
    if (value == "opaque") {
        return ApexAlphaMode::Opaque;
    }
    if (value == "masked" || value == "mask") {
        return ApexAlphaMode::Masked;
    }
    if (value == "translucent" || value == "transparent" || value == "blend" || value == "alpha") {
        return ApexAlphaMode::Translucent;
    }
    if (value == "additive" || value == "add") {
        return ApexAlphaMode::Additive;
    }
    return std::nullopt;
}

std::optional<ApexOpacitySource> apexOpacitySourceFromString(std::string value) {
    value = toLower(std::move(value));
    if (value == "one" || value == "none" || value == "ignore") {
        return ApexOpacitySource::One;
    }
    if (value == "opacitytexture" || value == "opacity" || value == "mask" || value == "msk") {
        return ApexOpacitySource::OpacityTexture;
    }
    if (value == "albedoalpha" || value == "albedo" || value == "alpha") {
        return ApexOpacitySource::AlbedoAlpha;
    }
    if (value == "minalbedoandopacity" || value == "min" || value == "both") {
        return ApexOpacitySource::MinAlbedoAndOpacity;
    }
    return std::nullopt;
}

std::optional<ApexOpacityChannel> apexOpacityChannelFromString(std::string value) {
    value = toLower(std::move(value));
    if (value == "r" || value == "red" || value == "0") {
        return ApexOpacityChannel::R;
    }
    if (value == "g" || value == "green" || value == "1") {
        return ApexOpacityChannel::G;
    }
    if (value == "b" || value == "blue" || value == "2") {
        return ApexOpacityChannel::B;
    }
    if (value == "a" || value == "alpha" || value == "3") {
        return ApexOpacityChannel::A;
    }
    return std::nullopt;
}

std::optional<ApexMaterialDebugView> apexMaterialDebugViewFromString(std::string value) {
    value = toLower(std::move(value));
    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
            return ch == ' ' || ch == '_' || ch == '-' || ch == '/';
        }),
        value.end());

    if (value == "finallit" || value == "final") {
        return ApexMaterialDebugView::FinalLit;
    }
    if (value == "basecolor" || value == "albedo") {
        return ApexMaterialDebugView::BaseColor;
    }
    if (value == "normal") {
        return ApexMaterialDebugView::Normal;
    }
    if (value == "tangent") {
        return ApexMaterialDebugView::Tangent;
    }
    if (value == "roughness") {
        return ApexMaterialDebugView::Roughness;
    }
    if (value == "specularf0" || value == "specular") {
        return ApexMaterialDebugView::SpecularF0;
    }
    if (value == "ao" || value == "ambientocclusion") {
        return ApexMaterialDebugView::AmbientOcclusion;
    }
    if (value == "cavity") {
        return ApexMaterialDebugView::Cavity;
    }
    if (value == "opacitycoverage" || value == "opacity" || value == "coverage") {
        return ApexMaterialDebugView::OpacityCoverage;
    }
    if (value == "anisotropydirection" || value == "aniso") {
        return ApexMaterialDebugView::AnisotropyDirection;
    }
    if (value == "emissive") {
        return ApexMaterialDebugView::Emissive;
    }
    if (value == "scatterthickness" || value == "scatter" || value == "thickness") {
        return ApexMaterialDebugView::ScatterThickness;
    }
    return std::nullopt;
}

std::optional<ApexTextureNameMatch> parseApexTextureName(const std::filesystem::path& path) {
    std::string textureName = stripLeadingTexturePrefix(path.stem().string());
    if (textureName.empty()) {
        return std::nullopt;
    }

    std::string materialSlotName = textureName;
    std::string token;
    const std::size_t delimiter = textureName.find_last_of('_');
    if (delimiter != std::string::npos) {
        materialSlotName = textureName.substr(0, delimiter);
        token = textureName.substr(delimiter + 1);

        if (isAllDigits(token)) {
            const std::size_t previousDelimiter = materialSlotName.find_last_of('_');
            if (previousDelimiter != std::string::npos) {
                const std::string previousToken = materialSlotName.substr(previousDelimiter + 1);
                if (apexTextureKindFromToken(previousToken).has_value()) {
                    token = previousToken;
                    materialSlotName = materialSlotName.substr(0, previousDelimiter);
                }
            }
        }
    } else {
        const std::string lowerName = toLower(textureName);
        for (const TokenMapping& mapping : TextureTokenMappings) {
            if (endsWith(lowerName, mapping.token)) {
                token = mapping.token;
                materialSlotName = textureName.substr(0, textureName.size() - std::char_traits<char>::length(mapping.token));
                if (!materialSlotName.empty() && materialSlotName.back() == '_') {
                    materialSlotName.pop_back();
                }
                break;
            }
        }
    }

    const std::optional<ApexTextureKind> kind = apexTextureKindFromToken(token);
    if (!kind.has_value() || materialSlotName.empty()) {
        return std::nullopt;
    }

    return ApexTextureNameMatch{materialSlotName, token, *kind};
}

ApexMaterialSet scanApexMaterialsForModel(const std::filesystem::path& modelOrDirectoryPath, const LoadedModel& model) {
    std::error_code error;
    const bool isDirectory = std::filesystem::is_directory(modelOrDirectoryPath, error);
    const std::filesystem::path sidecarAnchor = isDirectory ? model.sourcePath : modelOrDirectoryPath;
    const std::vector<std::filesystem::path> candidates = makeCandidateTextureDirectories(modelOrDirectoryPath);

    ApexMaterialSet materialSet;
    materialSet.sourceDirectory = isDirectory ? modelOrDirectoryPath : modelOrDirectoryPath.parent_path();
    materialSet.sidecarPath = makeSidecarPath(sidecarAnchor);

    bool sidecarFound = false;
    materialSet.parameters = loadApexMaterialSidecar(materialSet.sidecarPath, sidecarFound, materialSet.logLines);
    if (sidecarFound) {
        std::ifstream input(materialSet.sidecarPath);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string json = buffer.str();
        materialSet.slotOverrides = findJsonSlotOverrides(json);
        materialSet.slotAlphaOverrides = findJsonSlotAlphaOverrides(json);
    }

    const std::vector<std::filesystem::path> textureFiles = collectTextureFiles(candidates, materialSet.logLines);
    std::map<std::string, ApexMaterialSlot> groups;
    for (const std::filesystem::path& textureFile : textureFiles) {
        const std::optional<ApexTextureNameMatch> parsed = parseApexTextureName(textureFile);
        if (!parsed.has_value()) {
            materialSet.logLines.push_back("[ApexMaterial] unrecognized texture name: " + textureFile.filename().string());
            continue;
        }

        const std::string key = toLower(parsed->materialSlotName);
        ApexMaterialSlot& slot = groups[key];
        if (slot.name.empty()) {
            slot.name = parsed->materialSlotName;
        }

        const std::size_t kindIndex = static_cast<std::size_t>(parsed->kind);
        if (slot.hasTexture[kindIndex]) {
            materialSet.logLines.push_back(
                "[ApexMaterial] duplicate " + std::string(apexTextureKindName(parsed->kind)) +
                " for slot '" + slot.name + "', keeping " + slot.textures[kindIndex].filename().string() +
                " and ignoring " + textureFile.filename().string());
            continue;
        }

        slot.textures[kindIndex] = textureFile;
        slot.hasTexture[kindIndex] = true;
        materialSet.logLines.push_back(
            "[ApexMaterial] " + slot.name + "." + apexTextureKindName(parsed->kind) +
            " <- " + textureFile.filename().string());
    }

    for (const auto& item : groups) {
        materialSet.detectedTextureSlots.push_back(item.second.name);
    }

    const std::vector<std::string> expectedSlots = expectedMaterialSlotNames(model);
    const bool hasModelMaterialSlots = !model.materialSlots.empty();
    const bool canBindFallbackSlot =
        expectedSlots.size() == 1 &&
        (groups.find(toLower(expectedSlots.front())) != groups.end() ||
         materialSet.slotOverrides.find(toLower(expectedSlots.front())) != materialSet.slotOverrides.end());
    if (hasModelMaterialSlots || canBindFallbackSlot || groups.empty()) {
        for (const std::string& expectedSlot : expectedSlots) {
            std::string lookup = toLower(expectedSlot);
            if (const auto overrideIt = materialSet.slotOverrides.find(lookup); overrideIt != materialSet.slotOverrides.end()) {
                materialSet.logLines.push_back(
                    "[ApexMaterial] sidecar override: " + expectedSlot + " -> " + overrideIt->second);
                lookup = toLower(overrideIt->second);
            }

            const auto found = groups.find(lookup);
            if (found == groups.end()) {
                ApexMaterialSlot missing;
                missing.name = expectedSlot;
                materialSet.slots.push_back(std::move(missing));
                materialSet.logLines.push_back("[ApexMaterial] warning: no texture cluster matched material slot '" + expectedSlot + "'");
                continue;
            }

            ApexMaterialSlot boundSlot = found->second;
            if (boundSlot.name != expectedSlot) {
                materialSet.logLines.push_back(
                    "[ApexMaterial] bound material slot '" + expectedSlot +
                    "' to texture cluster '" + boundSlot.name + "'");
                boundSlot.name = expectedSlot;
            }
            materialSet.slots.push_back(std::move(boundSlot));
        }
    }

    if (materialSet.slots.empty() && !groups.empty()) {
        for (const auto& item : groups) {
            materialSet.slots.push_back(item.second);
        }
        std::sort(materialSet.slots.begin(), materialSet.slots.end(), [](const ApexMaterialSlot& lhs, const ApexMaterialSlot& rhs) {
            return lhs.name < rhs.name;
        });
    }

    if (materialSet.slots.empty()) {
        materialSet.slots.push_back({"default"});
    }

    applySlotAlphaSettings(materialSet);

    bool hasAnyTexture = false;
    for (const ApexMaterialSlot& slot : materialSet.slots) {
        hasAnyTexture = hasAnyTexture || texturePresenceScore(slot) > 0;
        for (std::size_t i = 0; i < ApexTextureKindCount; ++i) {
            if (!slot.hasTexture[i]) {
                materialSet.logLines.push_back(
                    "[ApexMaterial] default " + std::string(apexTextureKindName(static_cast<ApexTextureKind>(i))) +
                    " for slot '" + slot.name + "'");
            }
        }
    }

    if (!sidecarFound) {
        materialSet.parameters.enableApexMaterialMode = hasAnyTexture;
    }
    materialSet.logLines.push_back(
        "[ApexMaterial] built " + std::to_string(materialSet.slots.size()) +
        " material slot(s); Apex mode " + (materialSet.parameters.enableApexMaterialMode ? "enabled" : "disabled"));

    return materialSet;
}

ApexMaterialSet scanApexMaterialsInDirectory(
    const std::filesystem::path& directory,
    const LoadedModel& model,
    const std::filesystem::path& sidecarAnchorPath) {
    ApexMaterialSet materialSet;
    materialSet.sourceDirectory = directory;
    materialSet.sidecarPath = makeSidecarPath(sidecarAnchorPath.empty() ? directory : sidecarAnchorPath);

    bool sidecarFound = false;
    materialSet.parameters = loadApexMaterialSidecar(materialSet.sidecarPath, sidecarFound, materialSet.logLines);
    if (sidecarFound) {
        std::ifstream input(materialSet.sidecarPath);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string json = buffer.str();
        materialSet.slotOverrides = findJsonSlotOverrides(json);
        materialSet.slotAlphaOverrides = findJsonSlotAlphaOverrides(json);
    }

    const std::vector<std::filesystem::path> textureFiles = collectTextureFiles({directory}, materialSet.logLines);
    std::map<std::string, ApexMaterialSlot> groups;
    for (const std::filesystem::path& textureFile : textureFiles) {
        const std::optional<ApexTextureNameMatch> parsed = parseApexTextureName(textureFile);
        if (!parsed.has_value()) {
            materialSet.logLines.push_back("[ApexMaterial] unrecognized texture name: " + textureFile.filename().string());
            continue;
        }
        ApexMaterialSlot& slot = groups[toLower(parsed->materialSlotName)];
        if (slot.name.empty()) {
            slot.name = parsed->materialSlotName;
        }
        const std::size_t kindIndex = static_cast<std::size_t>(parsed->kind);
        if (!slot.hasTexture[kindIndex]) {
            slot.textures[kindIndex] = textureFile;
            slot.hasTexture[kindIndex] = true;
            materialSet.logLines.push_back(
                "[ApexMaterial] " + slot.name + "." + apexTextureKindName(parsed->kind) +
                " <- " + textureFile.filename().string());
        }
    }

    for (const auto& item : groups) {
        materialSet.detectedTextureSlots.push_back(item.second.name);
    }

    const std::vector<std::string> expectedSlots = expectedMaterialSlotNames(model);
    const bool hasModelMaterialSlots = !model.materialSlots.empty();
    const bool canBindFallbackSlot =
        expectedSlots.size() == 1 &&
        (groups.find(toLower(expectedSlots.front())) != groups.end() ||
         materialSet.slotOverrides.find(toLower(expectedSlots.front())) != materialSet.slotOverrides.end());
    if (hasModelMaterialSlots || canBindFallbackSlot || groups.empty()) {
        for (const std::string& expectedSlot : expectedSlots) {
            std::string lookup = toLower(expectedSlot);
            if (const auto overrideIt = materialSet.slotOverrides.find(lookup); overrideIt != materialSet.slotOverrides.end()) {
                materialSet.logLines.push_back(
                    "[ApexMaterial] sidecar override: " + expectedSlot + " -> " + overrideIt->second);
                lookup = toLower(overrideIt->second);
            }

            const auto found = groups.find(lookup);
            if (found == groups.end()) {
                ApexMaterialSlot missing;
                missing.name = expectedSlot;
                materialSet.slots.push_back(std::move(missing));
                materialSet.logLines.push_back("[ApexMaterial] warning: no texture cluster matched material slot '" + expectedSlot + "'");
            } else {
                ApexMaterialSlot boundSlot = found->second;
                if (boundSlot.name != expectedSlot) {
                    materialSet.logLines.push_back(
                        "[ApexMaterial] bound material slot '" + expectedSlot +
                        "' to texture cluster '" + boundSlot.name + "'");
                    boundSlot.name = expectedSlot;
                }
                materialSet.slots.push_back(std::move(boundSlot));
            }
        }
    }

    if (materialSet.slots.empty()) {
        for (const auto& item : groups) {
            materialSet.slots.push_back(item.second);
        }
    }
    if (materialSet.slots.empty()) {
        materialSet.slots.push_back({"default"});
    }

    applySlotAlphaSettings(materialSet);

    bool hasAnyTexture = false;
    for (const ApexMaterialSlot& slot : materialSet.slots) {
        hasAnyTexture = hasAnyTexture || texturePresenceScore(slot) > 0;
        for (std::size_t i = 0; i < ApexTextureKindCount; ++i) {
            if (!slot.hasTexture[i]) {
                materialSet.logLines.push_back(
                    "[ApexMaterial] default " + std::string(apexTextureKindName(static_cast<ApexTextureKind>(i))) +
                    " for slot '" + slot.name + "'");
            }
        }
    }
    if (!sidecarFound) {
        materialSet.parameters.enableApexMaterialMode = hasAnyTexture;
    }

    return materialSet;
}

void saveApexMaterialSidecar(const ApexMaterialSet& materialSet) {
    if (materialSet.sidecarPath.empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(materialSet.sidecarPath.parent_path(), error);

    std::ofstream output(materialSet.sidecarPath);
    if (!output) {
        std::cerr << "[ApexMaterial] failed to write sidecar: " << materialSet.sidecarPath << '\n';
        return;
    }

    const ApexMaterialParameters& p = materialSet.parameters;
    output << "{\n";
    output << "  \"enableApexMaterialMode\": " << (p.enableApexMaterialMode ? "true" : "false") << ",\n";
    output << "  \"flipNormalGreen\": " << (p.flipNormalGreen ? "true" : "false") << ",\n";
    output << "  \"roughnessMultiplier\": " << p.roughnessMultiplier << ",\n";
    output << "  \"specularMultiplier\": " << p.specularMultiplier << ",\n";
    output << "  \"aoStrength\": " << p.aoStrength << ",\n";
    output << "  \"cavityStrength\": " << p.cavityStrength << ",\n";
    output << "  \"emissiveStrength\": " << p.emissiveStrength << ",\n";
    output << "  \"alphaCutoff\": " << p.alphaCutoff << ",\n";
    output << "  \"enableSubsurfaceApproximation\": " << (p.enableSubsurfaceApproximation ? "true" : "false") << ",\n";
    output << "  \"subsurfaceColor\": [" << p.subsurfaceColor.x << ", " << p.subsurfaceColor.y << ", " << p.subsurfaceColor.z << "],\n";
    output << "  \"subsurfaceStrength\": " << p.subsurfaceStrength << ",\n";
    output << "  \"subsurfaceThicknessScale\": " << p.subsurfaceThicknessScale << ",\n";
    output << "  \"enableAnisotropy\": " << (p.enableAnisotropy ? "true" : "false") << ",\n";
    output << "  \"anisotropyStrength\": " << p.anisotropyStrength << ",\n";
    output << "  \"emissiveTint\": [" << p.emissiveTint.x << ", " << p.emissiveTint.y << ", " << p.emissiveTint.z << "],\n";
    output << "  \"debugView\": \"" << apexMaterialDebugViewName(p.debugView) << "\",\n";
    output << "  \"slotOverrides\": {\n";

    std::size_t index = 0;
    for (const auto& item : materialSet.slotOverrides) {
        output << "    \"" << item.first << "\": \"" << item.second << "\"";
        ++index;
        output << (index < materialSet.slotOverrides.size() ? ",\n" : "\n");
    }
    output << "  },\n";

    const auto writeOptionalMap = [&output](const char* name, const auto& entries, auto valueWriter) {
        output << "  \"" << name << "\": {\n";
        std::size_t written = 0;
        for (const auto& item : entries) {
            const std::optional<std::string> value = valueWriter(item.second);
            if (!value.has_value()) {
                continue;
            }
            if (written > 0) {
                output << ",\n";
            }
            output << "    \"" << item.first << "\": \"" << *value << "\"";
            ++written;
        }
        output << "\n  }";
    };

    writeOptionalMap(
        "slotAlphaModes",
        materialSet.slotAlphaOverrides,
        [](const ApexSlotAlphaOverride& overrideValue) -> std::optional<std::string> {
            if (!overrideValue.mode.has_value()) {
                return std::nullopt;
            }
            return std::string(apexAlphaModeName(*overrideValue.mode));
        });
    output << ",\n";

    writeOptionalMap(
        "slotOpacitySources",
        materialSet.slotAlphaOverrides,
        [](const ApexSlotAlphaOverride& overrideValue) -> std::optional<std::string> {
            if (!overrideValue.source.has_value()) {
                return std::nullopt;
            }
            return std::string(apexOpacitySourceName(*overrideValue.source));
        });
    output << ",\n";

    writeOptionalMap(
        "slotOpacityChannels",
        materialSet.slotAlphaOverrides,
        [](const ApexSlotAlphaOverride& overrideValue) -> std::optional<std::string> {
            if (!overrideValue.channel.has_value()) {
                return std::nullopt;
            }
            return std::string(apexOpacityChannelName(*overrideValue.channel));
        });
    output << "\n";
    output << "}\n";
}

std::string formatApexMaterialLog(const ApexMaterialSet& materialSet) {
    std::ostringstream output;
    for (const std::string& line : materialSet.logLines) {
        output << line << '\n';
    }

    output << "[ApexMaterial] slots:\n";
    for (const ApexMaterialSlot& slot : materialSet.slots) {
        output << "  - " << slot.name
               << " [" << apexAlphaModeName(slot.alphaMode)
               << ", " << apexOpacitySourceName(slot.opacitySource)
               << ", " << apexOpacityChannelName(slot.opacityChannel)
               << "]\n";
        for (std::size_t i = 0; i < ApexTextureKindCount; ++i) {
            const auto kind = static_cast<ApexTextureKind>(i);
            output << "      " << apexTextureKindName(kind) << ": ";
            if (slot.hasTexture[i]) {
                output << slot.textures[i].string();
            } else {
                output << "<default>";
            }
            output << '\n';
        }
    }
    return output.str();
}

} // namespace viewer
