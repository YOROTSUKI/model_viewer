#include "Model.h"

#include "CastImporter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace viewer {
namespace {

std::vector<std::string> splitSlash(std::string_view token) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : token) {
        if (ch == '/') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

int parseObjIndex(const std::string& text, std::size_t count) {
    if (text.empty()) {
        return -1;
    }

    const int raw = std::stoi(text);
    if (raw > 0) {
        return raw - 1;
    }
    if (raw < 0) {
        return static_cast<int>(count) + raw;
    }
    return -1;
}

struct ObjRef {
    int position = -1;
    int texCoord = -1;
    int normal = -1;
};

ObjRef parseFaceRef(std::string_view token, std::size_t positionCount, std::size_t texCoordCount, std::size_t normalCount) {
    const auto parts = splitSlash(token);
    ObjRef ref{};
    if (!parts.empty()) {
        ref.position = parseObjIndex(parts[0], positionCount);
    }
    if (parts.size() > 1) {
        ref.texCoord = parseObjIndex(parts[1], texCoordCount);
    }
    if (parts.size() > 2) {
        ref.normal = parseObjIndex(parts[2], normalCount);
    }

    if (ref.position < 0 || ref.position >= static_cast<int>(positionCount)) {
        throw std::runtime_error("OBJ face references an invalid position index");
    }
    if (ref.texCoord >= static_cast<int>(texCoordCount) || ref.normal >= static_cast<int>(normalCount)) {
        throw std::runtime_error("OBJ face references an invalid attribute index");
    }
    return ref;
}

std::string makeRefKey(const ObjRef& ref) {
    return std::to_string(ref.position) + "/" + std::to_string(ref.texCoord) + "/" + std::to_string(ref.normal);
}

void normalizeModel(LoadedModel& model) {
    if (model.vertices.empty()) {
        return;
    }

    Vec3 minBounds = model.vertices.front().position;
    Vec3 maxBounds = model.vertices.front().position;
    for (const Vertex& vertex : model.vertices) {
        minBounds.x = std::min(minBounds.x, vertex.position.x);
        minBounds.y = std::min(minBounds.y, vertex.position.y);
        minBounds.z = std::min(minBounds.z, vertex.position.z);
        maxBounds.x = std::max(maxBounds.x, vertex.position.x);
        maxBounds.y = std::max(maxBounds.y, vertex.position.y);
        maxBounds.z = std::max(maxBounds.z, vertex.position.z);
    }

    const Vec3 center = (minBounds + maxBounds) * 0.5f;
    const Vec3 extent = maxBounds - minBounds;
    const float maxExtent = std::max({extent.x, extent.y, extent.z, 0.0001f});
    const float scale = 2.0f / maxExtent;

    for (Vertex& vertex : model.vertices) {
        vertex.position = (vertex.position - center) * scale;
    }
}

void generateMissingNormals(LoadedModel& model) {
    bool allHaveNormals = true;
    for (const Vertex& vertex : model.vertices) {
        if (length(vertex.normal) <= 0.0001f) {
            allHaveNormals = false;
            break;
        }
    }
    if (allHaveNormals) {
        return;
    }

    for (Vertex& vertex : model.vertices) {
        vertex.normal = {};
    }

    for (std::size_t index = 0; index + 2 < model.indices.size(); index += 3) {
        Vertex& v0 = model.vertices[model.indices[index + 0]];
        Vertex& v1 = model.vertices[model.indices[index + 1]];
        Vertex& v2 = model.vertices[model.indices[index + 2]];
        const Vec3 normal = normalize(cross(v1.position - v0.position, v2.position - v0.position));
        v0.normal = v0.normal + normal;
        v1.normal = v1.normal + normal;
        v2.normal = v2.normal + normal;
    }

    for (Vertex& vertex : model.vertices) {
        vertex.normal = normalize(vertex.normal);
    }
}

LoadedModel loadObj(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open model file: " + path.string());
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> texCoords;
    std::unordered_map<std::string, std::uint32_t> vertexLookup;

    LoadedModel model;
    model.name = path.filename().string();
    model.sourcePath = path;

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream lineStream(line);
        std::string tag;
        lineStream >> tag;

        if (tag == "v") {
            Vec3 position{};
            lineStream >> position.x >> position.y >> position.z;
            positions.push_back(position);
        } else if (tag == "vn") {
            Vec3 normal{};
            lineStream >> normal.x >> normal.y >> normal.z;
            normals.push_back(normalize(normal));
        } else if (tag == "vt") {
            Vec2 texCoord{};
            lineStream >> texCoord.x >> texCoord.y;
            texCoords.push_back(texCoord);
        } else if (tag == "f") {
            std::vector<std::uint32_t> faceIndices;
            std::string token;
            while (lineStream >> token) {
                const ObjRef ref = parseFaceRef(token, positions.size(), texCoords.size(), normals.size());
                const std::string key = makeRefKey(ref);

                auto found = vertexLookup.find(key);
                if (found == vertexLookup.end()) {
                    Vertex vertex{};
                    vertex.position = positions[static_cast<std::size_t>(ref.position)];
                    if (ref.texCoord >= 0) {
                        vertex.texCoord = texCoords[static_cast<std::size_t>(ref.texCoord)];
                    }
                    if (ref.normal >= 0) {
                        vertex.normal = normals[static_cast<std::size_t>(ref.normal)];
                    }

                    const auto newIndex = static_cast<std::uint32_t>(model.vertices.size());
                    vertexLookup.emplace(key, newIndex);
                    model.vertices.push_back(vertex);
                    faceIndices.push_back(newIndex);
                } else {
                    faceIndices.push_back(found->second);
                }
            }

            if (faceIndices.size() < 3) {
                throw std::runtime_error("OBJ face has fewer than three vertices");
            }

            for (std::size_t i = 1; i + 1 < faceIndices.size(); ++i) {
                model.indices.push_back(faceIndices[0]);
                model.indices.push_back(faceIndices[i]);
                model.indices.push_back(faceIndices[i + 1]);
            }
        }
    }

    if (model.vertices.empty() || model.indices.empty()) {
        throw std::runtime_error("OBJ did not contain drawable geometry");
    }

    normalizeModel(model);
    generateMissingNormals(model);
    model.materialSlots.push_back({"default", 0, static_cast<std::uint32_t>(model.indices.size())});
    return model;
}

} // namespace

LoadedModel makeFallbackCube() {
    LoadedModel model;
    model.name = "fallback_cube";

    const auto addFace = [&model](Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 normal) {
        const auto start = static_cast<std::uint32_t>(model.vertices.size());
        model.vertices.push_back({a, normal, {0.0f, 0.0f}});
        model.vertices.push_back({b, normal, {1.0f, 0.0f}});
        model.vertices.push_back({c, normal, {1.0f, 1.0f}});
        model.vertices.push_back({d, normal, {0.0f, 1.0f}});
        model.indices.insert(model.indices.end(), {start, start + 1, start + 2, start, start + 2, start + 3});
    };

    addFace({-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, {0, 0, 1});
    addFace({1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {0, 0, -1});
    addFace({-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, 0, 0});
    addFace({1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, 0, 0});
    addFace({-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}, {0, 1, 0});
    addFace({-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}, {0, -1, 0});
    model.materialSlots.push_back({"fallback", 0, static_cast<std::uint32_t>(model.indices.size())});

    return model;
}

LoadedModel loadModelOrFallback(const std::filesystem::path& path) {
    if (path.empty()) {
        std::cout << "No model path provided; using fallback cube.\n";
        return makeFallbackCube();
    }

    const std::string extension = path.extension().string();
    try {
        if (extension == ".cast" || extension == ".CAST") {
            LoadedModel model = loadCastModel(path);
            if (model.vertices.empty()) {
                LoadedModel fallback = makeFallbackCube();
                fallback.name = model.name.empty() ? "animation_only_cast" : model.name;
                fallback.skeleton = std::move(model.skeleton);
                fallback.animations = std::move(model.animations);
                std::cerr << "Cast file has animation data but no drawable mesh; using fallback cube for preview.\n";
                return fallback;
            }

            normalizeModel(model);
            generateMissingNormals(model);
            return model;
        }

        if (extension == ".obj" || extension == ".OBJ") {
            LoadedModel model = loadObj(path);
            std::cout << "Loaded OBJ: " << path << " (" << model.vertices.size() << " vertices, "
                      << model.indices.size() / 3 << " triangles)\n";
            return model;
        }

        std::cerr << "Unsupported model extension '" << extension << "' for " << path
                  << "; using fallback cube. Add a loader in Model.cpp for this format.\n";
    } catch (const std::exception& ex) {
        std::cerr << "Failed to load " << path << ": " << ex.what() << "; using fallback cube.\n";
    }

    return makeFallbackCube();
}

} // namespace viewer
