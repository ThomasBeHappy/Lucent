#include "Application.h"
#include "lucent/core/Log.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    // Initialize logging
    lucent::Log::Init();
    
    LUCENT_INFO("Starting Lucent Editor...");
    
    lucent::ApplicationConfig config{};
    config.title = "Lucent Editor";
    config.width = 1280;
    config.height = 720;
    config.vsync = true;
    
#if LUCENT_DEBUG
    config.enableValidation = true;
#else
    config.enableValidation = false;
#endif
    
    lucent::Application app;
    
    if (!app.Init(config)) {
        LUCENT_CRITICAL("Failed to initialize application");
        lucent::Log::Shutdown();
        return 1;
    }
    
    app.Run();
    app.Shutdown();
    
    LUCENT_INFO("Lucent Editor closed");
    lucent::Log::Shutdown();
    
    return 0;
}

