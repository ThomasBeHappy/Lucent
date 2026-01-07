add_library(engine_gfx STATIC
    src/VulkanContext.cpp
    src/Device.cpp
    src/Swapchain.cpp
    src/Buffer.cpp
    src/Image.cpp
    src/DescriptorAllocator.cpp
    src/PipelineBuilder.cpp
    src/Renderer.cpp
    src/DebugUtils.cpp
)

target_include_directories(engine_gfx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine_gfx
    PUBLIC
        Lucent::Core
        Vulkan::Vulkan
        glfw
        glm::glm
        unofficial::shaderc::shaderc
)

# Alias for cleaner target names
add_library(Lucent::Gfx ALIAS engine_gfx)

