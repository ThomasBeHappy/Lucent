# Tests placeholder - will be expanded later
# For now, just create a simple sanity test

add_executable(test_core
    test_core.cpp
)

target_link_libraries(test_core
    PRIVATE
        Lucent::Core
)

add_test(NAME CoreTests COMMAND test_core)

