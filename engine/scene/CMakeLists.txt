add_library(engine_scene STATIC
    src/Scene.cpp
    src/Entity.cpp
    src/Components.cpp
    src/EditorCamera.cpp
)

target_include_directories(engine_scene
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine_scene
    PUBLIC
        Lucent::Core
        Lucent::Gfx
        glm::glm
)

# Alias for cleaner target names
add_library(Lucent::Scene ALIAS engine_scene)

