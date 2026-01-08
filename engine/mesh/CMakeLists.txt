set(ENGINE_MESH_SOURCES
    src/EditableMesh.cpp
    src/Triangulator.cpp
    src/MeshOps.cpp
)

add_library(engine_mesh STATIC ${ENGINE_MESH_SOURCES})

target_include_directories(engine_mesh
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine_mesh
    PUBLIC
        Lucent::Core
        glm::glm
)

# Alias for cleaner target names
add_library(Lucent::Mesh ALIAS engine_mesh)
