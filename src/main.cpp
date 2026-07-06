#include "Application.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
        std::filesystem::path modelPath;
        bool smokeTest = false;
        if (argc > 1 && std::string_view(argv[1]) == "--smoke-test") {
            smokeTest = true;
            if (argc > 2) {
                modelPath = std::filesystem::path(argv[2]);
            }
        } else if (argc > 1) {
            modelPath = std::filesystem::path(argv[1]);
        }

        {
            viewer::Application app(modelPath);
            if (smokeTest) {
                app.runSmokeTest();
            } else {
                app.run();
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
        return 1;
    }
}
