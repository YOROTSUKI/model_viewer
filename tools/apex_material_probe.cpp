#include "ApexMaterial.h"
#include "Model.h"

#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

bool nearlyEqual(float lhs, float rhs, float epsilon = 0.001f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

bool tangentGenerationSmokeTest() {
    viewer::LoadedModel model;
    model.name = "tangent_self_test_quad";
    model.vertices = {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    };
    model.indices = {0, 1, 2, 0, 2, 3};

    viewer::generateTangents(model);

    for (const viewer::Vertex& vertex : model.vertices) {
        if (!nearlyEqual(vertex.normal.x, 0.0f) ||
            !nearlyEqual(vertex.normal.y, 0.0f) ||
            !nearlyEqual(vertex.normal.z, 1.0f)) {
            std::cerr << "self-test failed: normal changed unexpectedly\n";
            return false;
        }
        if (!nearlyEqual(vertex.tangent.x, 1.0f) ||
            !nearlyEqual(vertex.tangent.y, 0.0f) ||
            !nearlyEqual(vertex.tangent.z, 0.0f)) {
            std::cerr << "self-test failed: tangent was not approximately +X\n";
            return false;
        }
        if (vertex.tangent.w < 0.5f) {
            std::cerr << "self-test failed: tangent handedness was not positive\n";
            return false;
        }
    }

    return true;
}

bool textureScannerSmokeTest() {
    const auto albedo = viewer::parseApexTextureName("T_body_sknp_col.png");
    if (!albedo.has_value() ||
        albedo->materialSlotName != "body_sknp" ||
        albedo->kind != viewer::ApexTextureKind::Albedo) {
        std::cerr << "self-test failed: _col texture parsing regressed\n";
        return false;
    }

    const auto aniso = viewer::parseApexTextureName("pilot_helmet_anisoSpecDirTexture.tga");
    if (!aniso.has_value() ||
        aniso->materialSlotName != "pilot_helmet" ||
        aniso->kind != viewer::ApexTextureKind::Anisotropy) {
        std::cerr << "self-test failed: anisotropy texture parsing regressed\n";
        return false;
    }

    const auto numbered = viewer::parseApexTextureName("T_glass_visor_opa_0001.png");
    if (!numbered.has_value() ||
        numbered->materialSlotName != "glass_visor" ||
        numbered->kind != viewer::ApexTextureKind::Opacity) {
        std::cerr << "self-test failed: numbered texture parsing regressed\n";
        return false;
    }

    return true;
}

int runSelfTest() {
    if (!tangentGenerationSmokeTest() || !textureScannerSmokeTest()) {
        return 1;
    }
    std::cout << "apex_material_probe self-test passed\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--self-test") {
        return runSelfTest();
    }

    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
        std::cerr << "usage: apex_material_probe <model-file-or-texture-directory>\n";
        std::cerr << "       apex_material_probe --self-test\n";
        return argc < 2 ? 2 : 0;
    }

    try {
        const std::filesystem::path inputPath = argv[1];
        viewer::LoadedModel model;

        std::error_code error;
        if (std::filesystem::is_regular_file(inputPath, error)) {
            model = viewer::loadModelOrFallback(inputPath);
        } else {
            model.name = inputPath.filename().string();
        }

        const viewer::ApexMaterialSet materialSet = viewer::scanApexMaterialsForModel(inputPath, model);
        std::cout << viewer::formatApexMaterialLog(materialSet);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "apex_material_probe failed: " << ex.what() << '\n';
        return 1;
    }
}
