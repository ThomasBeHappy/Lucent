# Shader compilation script
# Called during build to compile GLSL -> SPIR-V

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/compiled)

file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Find glslc compiler
find_program(GLSLC_EXECUTABLE glslc HINTS $ENV{VULKAN_SDK}/Bin)

if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found! Make sure Vulkan SDK is installed.")
endif()

# Collect all shader files
file(GLOB_RECURSE SHADER_SOURCES
    "${SHADER_SOURCE_DIR}/*.vert"
    "${SHADER_SOURCE_DIR}/*.frag"
    "${SHADER_SOURCE_DIR}/*.comp"
    "${SHADER_SOURCE_DIR}/*.rgen"
    "${SHADER_SOURCE_DIR}/*.rmiss"
    "${SHADER_SOURCE_DIR}/*.rchit"
    "${SHADER_SOURCE_DIR}/*.rahit"
    "${SHADER_SOURCE_DIR}/*.rint"
)

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv)
    
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC_EXECUTABLE} -o ${SHADER_OUTPUT} ${SHADER}
            --target-env=vulkan1.2
            -g  # Debug info for RenderDoc
        # Depend on this CMakeLists too so changing compile flags forces a recompile
        DEPENDS ${SHADER} ${CMAKE_CURRENT_LIST_FILE}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    
    list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()

add_custom_target(shaders ALL DEPENDS ${SHADER_OUTPUTS})

