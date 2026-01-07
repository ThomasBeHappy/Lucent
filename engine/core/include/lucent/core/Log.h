#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <memory>

namespace lucent {

class Log {
public:
    static void Init();
    static void Shutdown();
    
    static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
    static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }

private:
    static std::shared_ptr<spdlog::logger> s_CoreLogger;
    static std::shared_ptr<spdlog::logger> s_ClientLogger;
};

} // namespace lucent

// Core logging macros
#define LUCENT_CORE_TRACE(...)    ::lucent::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define LUCENT_CORE_DEBUG(...)    ::lucent::Log::GetCoreLogger()->debug(__VA_ARGS__)
#define LUCENT_CORE_INFO(...)     ::lucent::Log::GetCoreLogger()->info(__VA_ARGS__)
#define LUCENT_CORE_WARN(...)     ::lucent::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define LUCENT_CORE_ERROR(...)    ::lucent::Log::GetCoreLogger()->error(__VA_ARGS__)
#define LUCENT_CORE_CRITICAL(...) ::lucent::Log::GetCoreLogger()->critical(__VA_ARGS__)

// Client/application logging macros
#define LUCENT_TRACE(...)    ::lucent::Log::GetClientLogger()->trace(__VA_ARGS__)
#define LUCENT_LOG_DEBUG(...)    ::lucent::Log::GetClientLogger()->debug(__VA_ARGS__)
#define LUCENT_INFO(...)     ::lucent::Log::GetClientLogger()->info(__VA_ARGS__)
#define LUCENT_WARN(...)     ::lucent::Log::GetClientLogger()->warn(__VA_ARGS__)
#define LUCENT_ERROR(...)    ::lucent::Log::GetClientLogger()->error(__VA_ARGS__)
#define LUCENT_CRITICAL(...) ::lucent::Log::GetClientLogger()->critical(__VA_ARGS__)

