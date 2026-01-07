add_library(engine_core STATIC
    src/Log.cpp
    src/Assert.cpp
)

target_include_directories(engine_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine_core
    PUBLIC
        spdlog::spdlog
)

# Alias for cleaner target names
add_library(Lucent::Core ALIAS engine_core)

