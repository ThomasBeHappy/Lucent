#include "lucent/core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace lucent {

std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

void Log::Init() {
    // Create sinks
    std::vector<spdlog::sink_ptr> sinks;
    
    // Console sink with colors
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("%^[%T] [%n] %v%$");
    sinks.push_back(consoleSink);
    
    // File sink (optional - creates lucent.log)
    try {
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("lucent.log", true);
        fileSink->set_pattern("[%T] [%l] [%n] %v");
        sinks.push_back(fileSink);
    } catch (const spdlog::spdlog_ex&) {
        // Failed to create log file, continue with console only
        // Warning will be logged once logger is created
    }
    
    // Core logger
    s_CoreLogger = std::make_shared<spdlog::logger>("LUCENT", sinks.begin(), sinks.end());
    spdlog::register_logger(s_CoreLogger);
    s_CoreLogger->set_level(spdlog::level::trace);
    s_CoreLogger->flush_on(spdlog::level::trace);
    
    // Client logger
    s_ClientLogger = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());
    spdlog::register_logger(s_ClientLogger);
    s_ClientLogger->set_level(spdlog::level::trace);
    s_ClientLogger->flush_on(spdlog::level::trace);
    
    LUCENT_CORE_INFO("Lucent Engine initialized");
}

void Log::Shutdown() {
    LUCENT_CORE_INFO("Shutting down logging");
    spdlog::shutdown();
}

} // namespace lucent

