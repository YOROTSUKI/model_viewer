#include "CastImporter.h"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cast_import_probe <file.cast>\n";
        return 2;
    }

    try {
        viewer::LoadedModel model = viewer::loadCastModel(std::filesystem::path(argv[1]));
        std::cout << "name=" << model.name << '\n';
        std::cout << "vertices=" << model.vertices.size() << '\n';
        std::cout << "triangles=" << model.indices.size() / 3 << '\n';
        std::cout << "bones=" << model.skeleton.size() << '\n';
        std::cout << "animations=" << model.animations.size() << '\n';
        for (const viewer::AnimationClip& clip : model.animations) {
            std::cout << "animation=" << clip.name
                      << " framerate=" << clip.framerate
                      << " curves=" << clip.curves.size()
                      << " notifications=" << clip.notifications.size()
                      << '\n';
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
