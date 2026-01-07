cmake_minimum_required(VERSION 3.25)

project(Lucent
    VERSION 0.1.0
    DESCRIPTION "Blender-like 3D editor with Vulkan RT path tracer"
    LANGUAGES CXX CUDA
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
option(LUCENT_ENABLE_OPTIX "Enable OptiX AI Denoiser (requires NVIDIA GPU and OptiX SDK)" ON)

# Find packages via vcpkg
find_package(Vulkan REQUIRED)

# CUDA and OptiX for AI denoising
if(LUCENT_ENABLE_OPTIX)
    find_package(CUDAToolkit REQUIRED)
    
    # Find OptiX SDK (set OPTIX_ROOT or OptiX_INSTALL_DIR environment variable)
    if(DEFINED ENV{OPTIX_ROOT})
        set(OptiX_INSTALL_DIR $ENV{OPTIX_ROOT})
    elseif(DEFINED ENV{OptiX_INSTALL_DIR})
        set(OptiX_INSTALL_DIR $ENV{OptiX_INSTALL_DIR})
    else()
        # Default paths
        if(WIN32)
            file(GLOB OptiX_INSTALL_DIR "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.*")
            if(NOT OptiX_INSTALL_DIR)
                file(GLOB OptiX_INSTALL_DIR "C:/ProgramData/NVIDIA Corporation/OptiX SDK 8.*")
            endif()
            if(NOT OptiX_INSTALL_DIR)
                file(GLOB OptiX_INSTALL_DIR "C:/ProgramData/NVIDIA Corporation/OptiX SDK 7.*")
            endif()
        endif()
    endif()
    
    if(OptiX_INSTALL_DIR)
        list(GET OptiX_INSTALL_DIR 0 OptiX_INSTALL_DIR)  # Take first match
        message(STATUS "Found OptiX SDK: ${OptiX_INSTALL_DIR}")
        set(OPTIX_INCLUDE_DIR "${OptiX_INSTALL_DIR}/include")
        add_compile_definitions(LUCENT_ENABLE_OPTIX=1)
    else()
        message(WARNING "OptiX SDK not found. Set OPTIX_ROOT environment variable. Disabling OptiX.")
        set(LUCENT_ENABLE_OPTIX OFF)
    endif()
endif()
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

