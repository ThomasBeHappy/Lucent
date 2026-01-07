cmake_minimum_required(VERSION 3.25)

project(Lucent
    VERSION 0.1.0
    DESCRIPTION "Blender-like 3D editor with Vulkan RT path tracer"
    LANGUAGES CXX
)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(LUCENT_ENABLE_VALIDATION "Enable Vulkan validation layers in debug builds" ON)
option(LUCENT_BUILD_TESTS "Build unit tests" ON)

# Find packages via vcpkg
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)

# Global compile definitions
add_compile_definitions(
    $<$<CONFIG:Debug>:LUCENT_DEBUG=1>
    $<$<CONFIG:Debug>:LUCENT_ENABLE_VALIDATION=1>
    $<$<NOT:$<CONFIG:Debug>>:LUCENT_DEBUG=0>
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLFW_INCLUDE_VULKAN
    VK_USE_PLATFORM_WIN32_KHR
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# Warning flags
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Subdirectories
add_subdirectory(engine/core)
add_subdirectory(engine/gfx)
add_subdirectory(engine/scene)
add_subdirectory(engine/assets)
add_subdirectory(engine/material)
add_subdirectory(app/editor)

if(LUCENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

