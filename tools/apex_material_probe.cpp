#include "ApexMaterial.h"
#include "Model.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

bool degenerateUvTangentFallbackSmokeTest() {
    viewer::LoadedModel model;
    model.name = "tangent_self_test_degenerate_uv";
    model.vertices = {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
    };
    model.indices = {0, 1, 2};

    viewer::generateTangents(model);

    for (const viewer::Vertex& vertex : model.vertices) {
        if (!nearlyEqual(vertex.tangent.x, 0.0f) ||
            !nearlyEqual(vertex.tangent.y, 0.0f) ||
            !nearlyEqual(vertex.tangent.z, 0.0f) ||
            !nearlyEqual(vertex.tangent.w, 0.0f)) {
            std::cerr << "self-test failed: degenerate UV did not preserve invalid tangent fallback marker\n";
            return false;
        }
    }

    return true;
}

bool mirroredUvTangentSmokeTest() {
    viewer::LoadedModel model;
    model.name = "tangent_self_test_mirrored_uv";
    model.vertices = {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
    };
    model.indices = {0, 1, 2, 0, 2, 3};

    viewer::generateTangents(model);

    if (model.vertices.size() != 6) {
        std::cerr << "self-test failed: mirrored UV seam was not split into distinct handedness vertices\n";
        return false;
    }

    for (std::size_t corner = 0; corner < model.indices.size(); ++corner) {
        const std::uint32_t vertexIndex = model.indices[corner];
        if (vertexIndex >= model.vertices.size() || std::fabs(model.vertices[vertexIndex].tangent.w) < 0.5f) {
            std::cerr << "self-test failed: mirrored UV split produced an invalid tangent\n";
            return false;
        }
    }

    for (std::size_t corner = 0; corner < 3; ++corner) {
        if (model.vertices[model.indices[corner]].tangent.w < 0.5f) {
            std::cerr << "self-test failed: first mirrored UV triangle did not keep positive handedness\n";
            return false;
        }
    }
    for (std::size_t corner = 3; corner < 6; ++corner) {
        if (model.vertices[model.indices[corner]].tangent.w > -0.5f) {
            std::cerr << "self-test failed: second mirrored UV triangle did not keep negative handedness\n";
            return false;
        }
    }

    return true;
}

bool debugViewParserSmokeTest() {
    struct ExpectedDebugView {
        const char* text;
        viewer::ApexMaterialDebugView view;
        std::uint32_t value;
    };

    constexpr std::array<ExpectedDebugView, 25> expected = {{
        {"Final Lit", viewer::ApexMaterialDebugView::FinalLit, 0},
        {"Base Color", viewer::ApexMaterialDebugView::BaseColor, 1},
        {"Normal", viewer::ApexMaterialDebugView::Normal, 2},
        {"Tangent", viewer::ApexMaterialDebugView::Tangent, 3},
        {"Roughness", viewer::ApexMaterialDebugView::Roughness, 4},
        {"Specular/F0", viewer::ApexMaterialDebugView::SpecularF0, 5},
        {"AO", viewer::ApexMaterialDebugView::AmbientOcclusion, 6},
        {"Cavity", viewer::ApexMaterialDebugView::Cavity, 7},
        {"Coverage", viewer::ApexMaterialDebugView::OpacityCoverage, 8},
        {"Opacity/Coverage", viewer::ApexMaterialDebugView::OpacityCoverage, 8},
        {"Anisotropy Direction", viewer::ApexMaterialDebugView::AnisotropyDirection, 9},
        {"Emissive", viewer::ApexMaterialDebugView::Emissive, 10},
        {"Thickness", viewer::ApexMaterialDebugView::ScatterThickness, 11},
        {"Scatter/Thickness", viewer::ApexMaterialDebugView::ScatterThickness, 11},
        {"Tangent Validity", viewer::ApexMaterialDebugView::TangentValidity, 12},
        {"Transmittance", viewer::ApexMaterialDebugView::Transmittance, 13},
        {"transmission", viewer::ApexMaterialDebugView::Transmittance, 13},
        {"Mean Free Path", viewer::ApexMaterialDebugView::MeanFreePath, 14},
        {"mfp", viewer::ApexMaterialDebugView::MeanFreePath, 14},
        {"Medium Thickness", viewer::ApexMaterialDebugView::MediumThickness, 15},
        {"medium_depth", viewer::ApexMaterialDebugView::MediumThickness, 15},
        {"Closure Count", viewer::ApexMaterialDebugView::ClosureCount, 16},
        {"Layer Count", viewer::ApexMaterialDebugView::ClosureCount, 16},
        {"Layered Transmittance", viewer::ApexMaterialDebugView::LayeredTransmittance, 17},
        {"layer_transmission", viewer::ApexMaterialDebugView::LayeredTransmittance, 17},
    }};

    for (const ExpectedDebugView& item : expected) {
        const auto parsed = viewer::apexMaterialDebugViewFromString(item.text);
        if (!parsed.has_value() || *parsed != item.view) {
            std::cerr << "self-test failed: debug view parser rejected '" << item.text << "'\n";
            return false;
        }
        if (static_cast<std::uint32_t>(item.view) != item.value) {
            std::cerr << "self-test failed: debug view enum value drifted for '" << item.text << "'\n";
            return false;
        }
    }

    if (static_cast<std::uint32_t>(viewer::ApexMaterialDebugView::Count) != 18) {
        std::cerr << "self-test failed: debug view count drifted without updating shader constants test\n";
        return false;
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

bool startupParameterModeSmokeTest() {
    namespace fs = std::filesystem;

    const fs::path root =
        fs::temp_directory_path() /
        ("apex_material_probe_startup_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path sidecarAnchor = root / "model.cast";
    const fs::path sidecarPath = root / "model.apexmat.json";
    const fs::path texturePath = root / "T_body_sknp_col.png";

    auto cleanup = [&root]() {
        std::error_code cleanupError;
        fs::remove_all(root, cleanupError);
    };

    auto fail = [&cleanup](const char* message) {
        std::cerr << "self-test failed: " << message << '\n';
        cleanup();
        return false;
    };

    std::error_code error;
    fs::create_directories(root, error);
    if (error) {
        return fail("could not create startup parameter temp directory");
    }
    {
        std::ofstream texture(texturePath, std::ios::binary);
        if (!texture) {
            return fail("could not create startup parameter temp texture");
        }
    }

    auto writeSidecar = [&sidecarPath](std::string_view startupParameters) {
        std::ofstream output(sidecarPath);
        if (!output) {
            return false;
        }
        output << "{\n";
        if (!startupParameters.empty()) {
            output << "  \"startupParameters\": \"" << startupParameters << "\",\n";
        }
        output << "  \"enableApexMaterialMode\": false,\n";
        output << "  \"roughnessMultiplier\": 0.12,\n";
        output << "  \"alphaCutoff\": 0.75,\n";
        output << "  \"subsurfaceStrength\": 0.44,\n";
        output << "  \"substrateMaxClosureCount\": 3\n";
        output << "}\n";
        return static_cast<bool>(output);
    };

    viewer::LoadedModel model;
    model.name = "model";

    if (!writeSidecar("")) {
        return fail("could not write default startup sidecar");
    }
    viewer::ApexMaterialSet defaultStartupSet = viewer::scanApexMaterialsInDirectory(root, model, sidecarAnchor);
    const viewer::ApexMaterialParameters defaultParameters;
    if (defaultStartupSet.loadSavedMaterialParameters) {
        return fail("sidecar without startupParameters should not load saved material parameters");
    }
    if (!nearlyEqual(defaultStartupSet.parameters.roughnessMultiplier, defaultParameters.roughnessMultiplier) ||
        !nearlyEqual(defaultStartupSet.parameters.alphaCutoff, defaultParameters.alphaCutoff) ||
        !nearlyEqual(defaultStartupSet.parameters.subsurfaceStrength, defaultParameters.subsurfaceStrength)) {
        return fail("default startup mode reused saved material parameters");
    }
    if (defaultStartupSet.parameters.substrateMaxClosureCount != defaultParameters.substrateMaxClosureCount) {
        return fail("default startup mode reused saved substrate closure budget");
    }
    if (!defaultStartupSet.parameters.enableApexMaterialMode) {
        return fail("default startup mode did not enable Apex mode for detected textures");
    }

    if (!writeSidecar("Saved")) {
        return fail("could not write saved startup sidecar");
    }
    viewer::ApexMaterialSet savedStartupSet = viewer::scanApexMaterialsInDirectory(root, model, sidecarAnchor);
    if (!savedStartupSet.loadSavedMaterialParameters) {
        return fail("Saved startup mode did not opt into saved material parameters");
    }
    if (!nearlyEqual(savedStartupSet.parameters.roughnessMultiplier, 0.12f) ||
        !nearlyEqual(savedStartupSet.parameters.alphaCutoff, 0.75f) ||
        !nearlyEqual(savedStartupSet.parameters.subsurfaceStrength, 0.44f)) {
        return fail("Saved startup mode did not reuse saved material parameters");
    }
    if (savedStartupSet.parameters.substrateMaxClosureCount != 3) {
        return fail("Saved startup mode did not reuse saved substrate closure budget");
    }
    if (savedStartupSet.parameters.enableApexMaterialMode) {
        return fail("Saved startup mode ignored saved Apex mode flag");
    }

    cleanup();
    return true;
}

int runSelfTest() {
    if (!tangentGenerationSmokeTest() ||
        !degenerateUvTangentFallbackSmokeTest() ||
        !mirroredUvTangentSmokeTest() ||
        !textureScannerSmokeTest() ||
        !startupParameterModeSmokeTest() ||
        !debugViewParserSmokeTest()) {
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
