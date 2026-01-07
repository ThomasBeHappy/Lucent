find_package(Stb REQUIRED)

add_library(engine_assets STATIC
    src/Mesh.cpp
    src/Texture.cpp
    src/Material.cpp
)

add_library(Lucent::Assets ALIAS engine_assets)

target_include_directories(engine_assets PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(engine_assets PRIVATE
    ${Stb_INCLUDE_DIR}
)

target_link_libraries(engine_assets PUBLIC
    Lucent::Core
    Lucent::Gfx
    glm::glm
)

target_compile_features(engine_assets PUBLIC cxx_std_20)

