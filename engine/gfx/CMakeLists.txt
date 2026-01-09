set(ENGINE_GFX_SOURCES
    src/VulkanContext.cpp
    src/Device.cpp
    src/Swapchain.cpp
    src/Buffer.cpp
    src/Image.cpp
    src/stb_image_impl.cpp
    src/DescriptorAllocator.cpp
    src/PipelineBuilder.cpp
    src/Renderer.cpp
    src/DebugUtils.cpp
    src/RenderCapabilities.cpp
    src/TracerCompute.cpp
    src/TracerRayKHR.cpp
    src/FinalRender.cpp
    src/EnvironmentMap.cpp
    src/EnvironmentMapLibrary.cpp
)

# Add OptiX denoiser if enabled
if(LUCENT_ENABLE_OPTIX)
    list(APPEND ENGINE_GFX_SOURCES src/OptiXDenoiser.cpp)
endif()

add_library(engine_gfx STATIC ${ENGINE_GFX_SOURCES})

find_package(Stb REQUIRED)

target_include_directories(engine_gfx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${Stb_INCLUDE_DIR}
)

target_link_libraries(engine_gfx
    PUBLIC
        Lucent::Core
        Vulkan::Vulkan
        glfw
        glm::glm
        unofficial::shaderc::shaderc
)

# Add CUDA/OptiX if enabled
if(LUCENT_ENABLE_OPTIX)
    target_include_directories(engine_gfx PUBLIC ${OPTIX_INCLUDE_DIR})
    target_include_directories(engine_gfx PUBLIC ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(engine_gfx
        PUBLIC
            CUDA::cudart
            CUDA::cuda_driver
    )
endif()

# Alias for cleaner target names
add_library(Lucent::Gfx ALIAS engine_gfx)
