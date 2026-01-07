#include <lucent/core/Log.h>
#include <iostream>

int main() {
    lucent::Log::Init();
    LUCENT_INFO("Core test passed!");
    return 0;
}

