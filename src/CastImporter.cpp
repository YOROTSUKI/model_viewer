#include "CastImporter.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace viewer {
namespace {

constexpr std::uint32_t CastMagic = 0x74736163;
constexpr std::uint32_t CastVersion = 0x1;

constexpr std::uint32_t CastIdRoot = 0x746F6F72;
constexpr std::uint32_t CastIdModel = 0x6C646F6D;
constexpr std::uint32_t CastIdMesh = 0x6873656D;
constexpr std::uint32_t CastIdMaterial = 0x6C74616D;
constexpr std::uint32_t CastIdSkeleton = 0x6C656B73;
constexpr std::uint32_t CastIdBone = 0x656E6F62;
constexpr std::uint32_t CastIdAnimation = 0x6D696E61;
constexpr std::uint32_t CastIdCurve = 0x76727563;
constexpr std::uint32_t CastIdNotificationTrack = 0x6669746E;

struct CastProperty {
    std::string name;
    std::string type;
    std::string stringValue;
    std::vector<std::uint64_t> unsignedValues;
    std::vector<double> numericValues;
};

struct CastNode {
    std::uint32_t identifier = 0;
    std::uint64_t hash = 0;
    std::unordered_map<std::string, CastProperty> properties;
    std::vector<CastNode> children;
};

struct CastMaterialInfo {
    std::uint64_t hash = 0;
    std::string name;
};

class BinaryReader {
public:
    explicit BinaryReader(const std::filesystem::path& path)
        : input_(path, std::ios::binary), path_(path) {
        if (!input_) {
            throw std::runtime_error("could not open Cast file: " + path.string());
        }
    }

    std::uint64_t position() {
        const auto pos = input_.tellg();
        if (pos < 0) {
            throw std::runtime_error("failed to query Cast reader position");
        }
        return static_cast<std::uint64_t>(pos);
    }

    void seek(std::uint64_t position) {
        input_.seekg(static_cast<std::streamoff>(position), std::ios::beg);
        if (!input_) {
            throw std::runtime_error("failed to seek Cast file: " + path_.string());
        }
    }

    std::uint8_t readU8() {
        char value = 0;
        readBytes(&value, sizeof(value));
        return static_cast<std::uint8_t>(value);
    }

    std::uint16_t readU16() {
        std::uint8_t bytes[2]{};
        readBytes(bytes, sizeof(bytes));
        return static_cast<std::uint16_t>(bytes[0]) |
               static_cast<std::uint16_t>(bytes[1] << 8);
    }

    std::uint32_t readU32() {
        std::uint8_t bytes[4]{};
        readBytes(bytes, sizeof(bytes));
        return static_cast<std::uint32_t>(bytes[0]) |
               (static_cast<std::uint32_t>(bytes[1]) << 8) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    std::uint64_t readU64() {
        const std::uint64_t lo = readU32();
        const std::uint64_t hi = readU32();
        return lo | (hi << 32);
    }

    float readF32() {
        const std::uint32_t raw = readU32();
        float value = 0.0f;
        static_assert(sizeof(value) == sizeof(raw));
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    double readF64() {
        const std::uint64_t raw = readU64();
        double value = 0.0;
        static_assert(sizeof(value) == sizeof(raw));
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    std::string readString(std::size_t length) {
        std::string result(length, '\0');
        if (length > 0) {
            readBytes(result.data(), length);
        }
        return result;
    }

    std::string readNullTerminatedString() {
        std::string result;
        while (true) {
            const char ch = static_cast<char>(readU8());
            if (ch == '\0') {
                break;
            }
            result.push_back(ch);
        }
        return result;
    }

private:
    void readBytes(void* destination, std::size_t size) {
        input_.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
        if (!input_) {
            throw std::runtime_error("unexpected end of Cast file: " + path_.string());
        }
    }

    std::ifstream input_;
    std::filesystem::path path_;
};

std::string readPropertyType(BinaryReader& reader) {
    char typeBytes[2]{};
    typeBytes[0] = static_cast<char>(reader.readU8());
    typeBytes[1] = static_cast<char>(reader.readU8());

    std::string type;
    if (typeBytes[0] != '\0') {
        type.push_back(typeBytes[0]);
    }
    if (typeBytes[1] != '\0') {
        type.push_back(typeBytes[1]);
    }
    return type;
}

CastProperty readProperty(BinaryReader& reader) {
    CastProperty property;
    property.type = readPropertyType(reader);
    if (property.type == "v2") {
        property.type = "2v";
    } else if (property.type == "v3") {
        property.type = "3v";
    } else if (property.type == "v4") {
        property.type = "4v";
    }
    const std::uint16_t nameSize = reader.readU16();
    const std::uint32_t arrayLength = reader.readU32();
    property.name = reader.readString(nameSize);

    if (property.type == "s") {
        property.stringValue = reader.readNullTerminatedString();
        return property;
    }

    if (property.type == "b") {
        property.unsignedValues.reserve(arrayLength);
        for (std::uint32_t i = 0; i < arrayLength; ++i) {
            property.unsignedValues.push_back(reader.readU8());
        }
    } else if (property.type == "h") {
        property.unsignedValues.reserve(arrayLength);
        for (std::uint32_t i = 0; i < arrayLength; ++i) {
            property.unsignedValues.push_back(reader.readU16());
        }
    } else if (property.type == "i") {
        property.unsignedValues.reserve(arrayLength);
        for (std::uint32_t i = 0; i < arrayLength; ++i) {
            property.unsignedValues.push_back(reader.readU32());
        }
    } else if (property.type == "l") {
        property.unsignedValues.reserve(arrayLength);
        for (std::uint32_t i = 0; i < arrayLength; ++i) {
            property.unsignedValues.push_back(reader.readU64());
        }
    } else if (property.type == "f") {
        property.numericValues.reserve(arrayLength);
        for (std::uint32_t i = 0; i < arrayLength; ++i) {
            property.numericValues.push_back(reader.readF32());
        }
    } else if (property.type == "d") {
        property.numericValues.reserve(arrayLength);
        for (std::uint32_t i = 0; i < arrayLength; ++i) {
            property.numericValues.push_back(reader.readF64());
        }
    } else if (property.type == "2v" || property.type == "3v" || property.type == "4v") {
        const std::uint32_t width = static_cast<std::uint32_t>(property.type[0] - '0');
        property.numericValues.reserve(static_cast<std::size_t>(arrayLength) * width);
        for (std::uint32_t i = 0; i < arrayLength * width; ++i) {
            property.numericValues.push_back(reader.readF32());
        }
    } else {
        throw std::runtime_error("unsupported Cast property type '" + property.type + "' on property '" + property.name + "'");
    }

    return property;
}

CastNode readNode(BinaryReader& reader) {
    const std::uint64_t nodeStart = reader.position();

    CastNode node;
    node.identifier = reader.readU32();
    const std::uint32_t nodeSize = reader.readU32();
    node.hash = reader.readU64();
    const std::uint32_t propertyCount = reader.readU32();
    const std::uint32_t childCount = reader.readU32();
    const std::uint64_t nodeEnd = nodeStart + nodeSize;

    node.properties.reserve(propertyCount);
    for (std::uint32_t i = 0; i < propertyCount; ++i) {
        CastProperty property = readProperty(reader);
        node.properties.emplace(property.name, std::move(property));
    }

    node.children.reserve(childCount);
    for (std::uint32_t i = 0; i < childCount; ++i) {
        node.children.push_back(readNode(reader));
    }

    const std::uint64_t endPosition = reader.position();
    if (endPosition > nodeEnd) {
        throw std::runtime_error("Cast node size was exceeded while reading node");
    }
    if (endPosition < nodeEnd) {
        reader.seek(nodeEnd);
    }

    return node;
}

const CastProperty* findProperty(const CastNode& node, const std::string& name) {
    const auto found = node.properties.find(name);
    if (found == node.properties.end()) {
        return nullptr;
    }
    return &found->second;
}

std::string stringProperty(const CastNode& node, const std::string& name, const std::string& fallback = {}) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr || property->type != "s") {
        return fallback;
    }
    return property->stringValue;
}

float floatProperty(const CastNode& node, const std::string& name, float fallback) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr || property->numericValues.empty()) {
        return fallback;
    }
    return static_cast<float>(property->numericValues.front());
}

bool boolProperty(const CastNode& node, const std::string& name, bool fallback) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr || property->unsignedValues.empty()) {
        return fallback;
    }
    return property->unsignedValues.front() >= 1;
}

std::uint32_t uintProperty(const CastNode& node, const std::string& name, std::uint32_t fallback) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr || property->unsignedValues.empty()) {
        return fallback;
    }
    return static_cast<std::uint32_t>(property->unsignedValues.front());
}

std::int32_t int32FromUnsigned(std::uint64_t value) {
    const std::uint32_t raw = static_cast<std::uint32_t>(value & 0xFFFFFFFFu);
    std::int32_t result = 0;
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

Vec3 vec3Property(const CastNode& node, const std::string& name, Vec3 fallback) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr || property->numericValues.size() < 3) {
        return fallback;
    }
    return {
        static_cast<float>(property->numericValues[0]),
        static_cast<float>(property->numericValues[1]),
        static_cast<float>(property->numericValues[2]),
    };
}

Vec4 vec4Property(const CastNode& node, const std::string& name, Vec4 fallback) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr || property->numericValues.size() < 4) {
        return fallback;
    }
    return {
        static_cast<float>(property->numericValues[0]),
        static_cast<float>(property->numericValues[1]),
        static_cast<float>(property->numericValues[2]),
        static_cast<float>(property->numericValues[3]),
    };
}

std::vector<std::uint32_t> integerBuffer(const CastNode& node, const std::string& name) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr) {
        return {};
    }

    std::vector<std::uint32_t> result;
    result.reserve(property->unsignedValues.size());
    for (std::uint64_t value : property->unsignedValues) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("Cast integer buffer value exceeds uint32 range for property '" + name + "'");
        }
        result.push_back(static_cast<std::uint32_t>(value));
    }
    return result;
}

std::vector<float> floatBuffer(const CastNode& node, const std::string& name) {
    const CastProperty* property = findProperty(node, name);
    if (property == nullptr) {
        return {};
    }

    std::vector<float> result;
    result.reserve(property->numericValues.size());
    for (double value : property->numericValues) {
        result.push_back(static_cast<float>(value));
    }
    return result;
}

std::vector<Bone> parseSkeleton(const CastNode& skeletonNode) {
    std::vector<Bone> bones;
    for (const CastNode& child : skeletonNode.children) {
        if (child.identifier != CastIdBone) {
            continue;
        }

        Bone bone;
        bone.hash = child.hash;
        bone.name = stringProperty(child, "n");
        bone.parentIndex = -1;
        if (const CastProperty* parent = findProperty(child, "p"); parent != nullptr && !parent->unsignedValues.empty()) {
            bone.parentIndex = int32FromUnsigned(parent->unsignedValues.front());
        }
        bone.segmentScaleCompensate = boolProperty(child, "ssc", true);
        bone.localPosition = vec3Property(child, "lp", {});
        bone.localRotation = vec4Property(child, "lr", {0.0f, 0.0f, 0.0f, 1.0f});
        bone.worldPosition = vec3Property(child, "wp", {});
        bone.worldRotation = vec4Property(child, "wr", {0.0f, 0.0f, 0.0f, 1.0f});
        bone.scale = vec3Property(child, "s", {1.0f, 1.0f, 1.0f});
        bones.push_back(std::move(bone));
    }
    return bones;
}

void appendVec4Values(const CastProperty& property, std::vector<Vec4>& values) {
    if (property.numericValues.size() % 4 != 0) {
        throw std::runtime_error("Cast vec4 key buffer is not divisible by four");
    }

    values.reserve(property.numericValues.size() / 4);
    for (std::size_t i = 0; i < property.numericValues.size(); i += 4) {
        values.push_back({
            static_cast<float>(property.numericValues[i + 0]),
            static_cast<float>(property.numericValues[i + 1]),
            static_cast<float>(property.numericValues[i + 2]),
            static_cast<float>(property.numericValues[i + 3]),
        });
    }
}

AnimationCurve parseCurve(const CastNode& curveNode) {
    AnimationCurve curve;
    curve.nodeName = stringProperty(curveNode, "nn");
    curve.propertyName = stringProperty(curveNode, "kp");
    curve.mode = stringProperty(curveNode, "m", "absolute");
    curve.additiveBlendWeight = floatProperty(curveNode, "ab", 1.0f);
    curve.keyFrames = integerBuffer(curveNode, "kb");

    if (const CastProperty* values = findProperty(curveNode, "kv"); values != nullptr) {
        curve.valueType = values->type;
        if (values->type == "4v") {
            appendVec4Values(*values, curve.vector4Values);
        } else if (values->type == "f" || values->type == "d") {
            curve.scalarValues.reserve(values->numericValues.size());
            for (double value : values->numericValues) {
                curve.scalarValues.push_back(static_cast<float>(value));
            }
        } else if (values->type == "b" || values->type == "h" || values->type == "i") {
            curve.integerValues.reserve(values->unsignedValues.size());
            for (std::uint64_t value : values->unsignedValues) {
                curve.integerValues.push_back(static_cast<std::uint32_t>(value));
            }
        }
    }

    return curve;
}

AnimationNotification parseNotification(const CastNode& node) {
    AnimationNotification notification;
    notification.name = stringProperty(node, "n");
    notification.keyFrames = integerBuffer(node, "kb");
    return notification;
}

AnimationClip parseAnimation(const CastNode& animationNode) {
    AnimationClip clip;
    clip.name = stringProperty(animationNode, "n");
    clip.framerate = floatProperty(animationNode, "fr", 30.0f);
    clip.looping = boolProperty(animationNode, "lo", false);

    for (const CastNode& child : animationNode.children) {
        if (child.identifier == CastIdSkeleton) {
            clip.skeleton = parseSkeleton(child);
        } else if (child.identifier == CastIdCurve) {
            clip.curves.push_back(parseCurve(child));
        } else if (child.identifier == CastIdNotificationTrack) {
            clip.notifications.push_back(parseNotification(child));
        }
    }

    return clip;
}

void appendMesh(const CastNode& meshNode, LoadedModel& model, std::string materialSlotName) {
    const std::vector<float> positions = floatBuffer(meshNode, "vp");
    const std::vector<std::uint32_t> faces = integerBuffer(meshNode, "f");
    if (positions.empty() || faces.empty()) {
        std::cerr << "Skipping Cast mesh without vertex positions or faces.\n";
        return;
    }
    if (positions.size() % 3 != 0) {
        throw std::runtime_error("Cast mesh vertex position buffer is not divisible by three");
    }

    const std::size_t vertexCount = positions.size() / 3;
    const std::size_t vertexOffset = model.vertices.size();
    const std::vector<float> normals = floatBuffer(meshNode, "vn");
    const std::vector<float> texCoords = floatBuffer(meshNode, "u0");

    const bool hasNormals = normals.size() >= vertexCount * 3;
    const bool hasTexCoords = texCoords.size() >= vertexCount * 2;

    model.vertices.reserve(model.vertices.size() + vertexCount);
    model.skinBindings.reserve(model.skinBindings.size() + vertexCount);

    for (std::size_t i = 0; i < vertexCount; ++i) {
        Vertex vertex{};
        vertex.position = {
            positions[i * 3 + 0],
            positions[i * 3 + 1],
            positions[i * 3 + 2],
        };
        if (hasNormals) {
            vertex.normal = normalize({
                normals[i * 3 + 0],
                normals[i * 3 + 1],
                normals[i * 3 + 2],
            });
        }
        if (hasTexCoords) {
            vertex.texCoord = {
                texCoords[i * 2 + 0],
                texCoords[i * 2 + 1],
            };
        }
        model.vertices.push_back(vertex);
        model.skinBindings.push_back({});
    }

    const std::uint32_t maximumInfluence = uintProperty(meshNode, "mi", 0);
    const std::vector<std::uint32_t> weightBones = integerBuffer(meshNode, "wb");
    const std::vector<float> weightValues = floatBuffer(meshNode, "wv");
    if (maximumInfluence > 0 &&
        weightBones.size() >= vertexCount * maximumInfluence &&
        weightValues.size() >= vertexCount * maximumInfluence) {
        for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
            VertexSkinBinding& binding = model.skinBindings[vertexOffset + vertexIndex];
            binding.boneIndices.reserve(maximumInfluence);
            binding.weights.reserve(maximumInfluence);

            for (std::uint32_t influence = 0; influence < maximumInfluence; ++influence) {
                const std::size_t offset = vertexIndex * maximumInfluence + influence;
                const float weight = weightValues[offset];
                if (weight <= 0.0f) {
                    continue;
                }
                binding.boneIndices.push_back(weightBones[offset]);
                binding.weights.push_back(weight);
            }
        }
    } else if (maximumInfluence > 0) {
        std::cerr << "Cast mesh has incomplete skinning buffers; imported geometry but skipped its weights.\n";
    }

    const std::uint32_t firstIndex = static_cast<std::uint32_t>(model.indices.size());
    std::size_t degenerateFaces = 0;
    std::size_t outOfRangeFaces = 0;
    model.indices.reserve(model.indices.size() + faces.size());
    for (std::size_t i = 0; i + 2 < faces.size(); i += 3) {
        const std::uint32_t a = faces[i + 0];
        const std::uint32_t b = faces[i + 1];
        const std::uint32_t c = faces[i + 2];
        if (a == b || a == c || b == c) {
            ++degenerateFaces;
            continue;
        }
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) {
            ++outOfRangeFaces;
            continue;
        }

        model.indices.push_back(static_cast<std::uint32_t>(vertexOffset + a));
        model.indices.push_back(static_cast<std::uint32_t>(vertexOffset + b));
        model.indices.push_back(static_cast<std::uint32_t>(vertexOffset + c));
    }

    if (degenerateFaces > 0 || outOfRangeFaces > 0) {
        std::cerr << "Cast mesh skipped " << degenerateFaces << " degenerate faces and "
                  << outOfRangeFaces << " out-of-range faces.\n";
    }

    const std::uint32_t indexCount = static_cast<std::uint32_t>(model.indices.size()) - firstIndex;
    if (indexCount > 0) {
        if (materialSlotName.empty()) {
            materialSlotName = "cast_material_" + std::to_string(model.materialSlots.size());
        }
        model.materialSlots.push_back({std::move(materialSlotName), firstIndex, indexCount});
    }
}

CastMaterialInfo parseMaterial(const CastNode& materialNode) {
    CastMaterialInfo material;
    material.hash = materialNode.hash;
    material.name = stringProperty(materialNode, "n");
    return material;
}

void processModelNode(const CastNode& modelNode, LoadedModel& model) {
    const std::string modelName = stringProperty(modelNode, "n");
    if (!modelName.empty()) {
        model.name = modelName;
    }

    for (const CastNode& child : modelNode.children) {
        if (child.identifier == CastIdSkeleton) {
            model.skeleton = parseSkeleton(child);
            break;
        }
    }

    std::vector<CastMaterialInfo> materials;
    for (const CastNode& child : modelNode.children) {
        if (child.identifier == CastIdMaterial) {
            materials.push_back(parseMaterial(child));
        }
    }

    std::size_t meshOrdinal = 0;
    for (const CastNode& child : modelNode.children) {
        if (child.identifier == CastIdMesh) {
            std::string materialSlotName;
            if (meshOrdinal < materials.size()) {
                materialSlotName = materials[meshOrdinal].name;
            }
            appendMesh(child, model, std::move(materialSlotName));
            ++meshOrdinal;
        }
    }

    if (!materials.empty() && materials.size() != meshOrdinal) {
        std::cerr << "Cast material count (" << materials.size() << ") did not match mesh count ("
                  << meshOrdinal << "); material slots were assigned by available order.\n";
    }
}

void processRootNode(const CastNode& rootNode, LoadedModel& model) {
    for (const CastNode& child : rootNode.children) {
        if (child.identifier == CastIdModel) {
            processModelNode(child, model);
        } else if (child.identifier == CastIdAnimation) {
            model.animations.push_back(parseAnimation(child));
        }
    }
}

void logCastSummary(const LoadedModel& model, const std::filesystem::path& path) {
    std::size_t weightedVertices = 0;
    for (const VertexSkinBinding& binding : model.skinBindings) {
        if (!binding.weights.empty()) {
            ++weightedVertices;
        }
    }

    std::size_t curveCount = 0;
    for (const AnimationClip& clip : model.animations) {
        curveCount += clip.curves.size();
    }

    std::cout << "Loaded Cast: " << path << " (" << model.vertices.size() << " vertices, "
              << model.indices.size() / 3 << " triangles, " << model.skeleton.size()
              << " bones, " << weightedVertices << " weighted vertices, "
              << model.animations.size() << " animations, " << curveCount << " curves)\n";
}

} // namespace

LoadedModel loadCastModel(const std::filesystem::path& path) {
    BinaryReader reader(path);

    const std::uint32_t magic = reader.readU32();
    const std::uint32_t version = reader.readU32();
    const std::uint32_t rootNodeCount = reader.readU32();
    static_cast<void>(reader.readU32());

    if (magic != CastMagic) {
        throw std::runtime_error("invalid Cast file magic");
    }
    if (version != CastVersion) {
        throw std::runtime_error("unsupported Cast file version: " + std::to_string(version));
    }

    LoadedModel model;
    model.name = path.filename().string();
    model.sourcePath = path;

    for (std::uint32_t i = 0; i < rootNodeCount; ++i) {
        CastNode root = readNode(reader);
        if (root.identifier != CastIdRoot) {
            std::cerr << "Cast root node " << i << " is not a Root node; attempting to process it anyway.\n";
        }
        processRootNode(root, model);
    }

    if (model.vertices.empty() && model.animations.empty()) {
        throw std::runtime_error("Cast file did not contain importable model or animation data");
    }

    logCastSummary(model, path);
    return model;
}

} // namespace viewer
