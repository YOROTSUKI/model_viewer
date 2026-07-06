#include "ApexMaterial.h"
#include "Model.h"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: apex_material_probe <model-file-or-texture-directory>\n";
        return 2;
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
