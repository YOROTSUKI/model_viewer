cmake_minimum_required(VERSION 3.24)

project(vulkan_model_viewer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)

include(FetchContent)

find_package(glfw3 CONFIG QUIET)
if(NOT glfw3_FOUND)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.9
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan glfw)

find_program(GLSLC_EXECUTABLE
    NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/Bin"
)
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK and make sure VULKAN_SDK is set.")
endif()

set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")

set(SHADER_SOURCES
    "${SHADER_SOURCE_DIR}/mesh.vert"
    "${SHADER_SOURCE_DIR}/mesh.frag"
    "${SHADER_SOURCE_DIR}/mesh_transparent.frag"
    "${SHADER_SOURCE_DIR}/composite.vert"
    "${SHADER_SOURCE_DIR}/composite.frag"
)

set(SHADER_OUTPUTS)
foreach(shader ${SHADER_SOURCES})
    get_filename_component(shader_name "${shader}" NAME)
    set(output "${SHADER_OUTPUT_DIR}/${shader_name}.spv")
    list(APPEND SHADER_OUTPUTS "${output}")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${GLSLC_EXECUTABLE}" "${shader}" -o "${output}"
        DEPENDS "${shader}"
        VERBATIM
    )
endforeach()

add_custom_target(vulkan_model_viewer_shaders DEPENDS ${SHADER_OUTPUTS})

add_executable(vulkan_model_viewer
    src/ApexMaterial.cpp
    src/ApexMaterial.h
    src/Application.cpp
    src/Application.h
    src/CastImporter.cpp
    src/CastImporter.h
    src/main.cpp
    src/ViewerMath.h
    src/Model.cpp
    src/Model.h
    src/VulkanRenderer.cpp
    src/VulkanRenderer.h
)

add_dependencies(vulkan_model_viewer vulkan_model_viewer_shaders)

target_include_directories(vulkan_model_viewer PRIVATE src)
target_link_libraries(vulkan_model_viewer PRIVATE Vulkan::Vulkan glfw imgui)
if(WIN32)
    target_link_libraries(vulkan_model_viewer PRIVATE comdlg32 user32 shell32 ole32 windowscodecs)
endif()

target_compile_definitions(vulkan_model_viewer PRIVATE
    SHADER_DIR="${SHADER_OUTPUT_DIR}"
    NOMINMAX
)

add_executable(cast_import_probe
    tools/cast_import_probe.cpp
    src/CastImporter.cpp
    src/CastImporter.h
    src/Model.cpp
    src/Model.h
    src/ViewerMath.h
)

target_include_directories(cast_import_probe PRIVATE src)

add_executable(apex_material_probe
    tools/apex_material_probe.cpp
    src/ApexMaterial.cpp
    src/ApexMaterial.h
    src/CastImporter.cpp
    src/CastImporter.h
    src/Model.cpp
    src/Model.h
    src/ViewerMath.h
)

target_include_directories(apex_material_probe PRIVATE src)

if(MSVC)
    target_compile_options(vulkan_model_viewer PRIVATE /W4 /permissive-)
    target_compile_options(cast_import_probe PRIVATE /W4 /permissive-)
    target_compile_options(apex_material_probe PRIVATE /W4 /permissive-)
else()
    target_compile_options(vulkan_model_viewer PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(cast_import_probe PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(apex_material_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()
