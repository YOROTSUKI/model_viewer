#pragma once

#include "ViewerMath.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace viewer {

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 texCoord;
};

struct VertexSkinBinding {
    std::vector<std::uint32_t> boneIndices;
    std::vector<float> weights;
};

struct MaterialSlotRange {
    std::string name;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

struct Bone {
    std::uint64_t hash = 0;
    std::string name;
    std::int32_t parentIndex = -1;
    bool segmentScaleCompensate = true;
    Vec3 localPosition{};
    Vec4 localRotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 worldPosition{};
    Vec4 worldRotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct AnimationCurve {
    std::string nodeName;
    std::string propertyName;
    std::string mode;
    float additiveBlendWeight = 1.0f;
    std::string valueType;
    std::vector<std::uint32_t> keyFrames;
    std::vector<float> scalarValues;
    std::vector<Vec4> vector4Values;
    std::vector<std::uint32_t> integerValues;
};

struct AnimationNotification {
    std::string name;
    std::vector<std::uint32_t> keyFrames;
};

struct AnimationClip {
    std::string name;
    float framerate = 30.0f;
    bool looping = false;
    std::vector<Bone> skeleton;
    std::vector<AnimationCurve> curves;
    std::vector<AnimationNotification> notifications;
};

struct LoadedModel {
    std::string name;
    std::filesystem::path sourcePath;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MaterialSlotRange> materialSlots;
    std::vector<VertexSkinBinding> skinBindings;
    std::vector<Bone> skeleton;
    std::vector<AnimationClip> animations;
};

LoadedModel makeFallbackCube();
LoadedModel loadModelOrFallback(const std::filesystem::path& path);

} // namespace viewer
