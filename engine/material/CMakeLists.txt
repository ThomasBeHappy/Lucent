add_library(engine_material STATIC
    src/MaterialGraph.cpp
    src/MaterialCompiler.cpp
    src/MaterialAsset.cpp
    src/MaterialIR.cpp
    src/MaterialGraphEval.cpp
)

add_library(Lucent::Material ALIAS engine_material)

target_include_directories(engine_material PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine_material PUBLIC
    Lucent::Core
    Lucent::Gfx
    Lucent::Assets
    glm::glm
    unofficial::shaderc::shaderc
)

target_compile_features(engine_material PUBLIC cxx_std_20)

