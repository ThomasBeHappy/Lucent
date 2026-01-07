#pragma once

#include "lucent/core/Log.h"
#include <cstdlib>
#include <filesystem>

#ifdef _MSC_VER
    #define LUCENT_DEBUG_BREAK() __debugbreak()
#else
    #define LUCENT_DEBUG_BREAK() __builtin_trap()
#endif

namespace lucent {

// Extract just the filename from a path
inline const char* ExtractFilename(const char* path) {
    const char* file = path;
    while (*path) {
        if (*path == '/' || *path == '\\') {
            file = path + 1;
        }
        ++path;
    }
    return file;
}

} // namespace lucent

// Core assertions (always enabled in debug, can be disabled in release)
#if LUCENT_DEBUG
    #define LUCENT_ASSERT(condition, ...)                                          \
        do {                                                                        \
            if (!(condition)) {                                                     \
                LUCENT_CORE_CRITICAL("Assertion failed: {}", #condition);           \
                LUCENT_CORE_CRITICAL("  File: {}:{}", ::lucent::ExtractFilename(__FILE__), __LINE__); \
                LUCENT_CORE_CRITICAL("  " __VA_ARGS__);                             \
                LUCENT_DEBUG_BREAK();                                               \
            }                                                                       \
        } while (false)
    
    #define LUCENT_CORE_ASSERT(condition, ...)                                     \
        do {                                                                        \
            if (!(condition)) {                                                     \
                LUCENT_CORE_CRITICAL("Core assertion failed: {}", #condition);      \
                LUCENT_CORE_CRITICAL("  File: {}:{}", ::lucent::ExtractFilename(__FILE__), __LINE__); \
                LUCENT_CORE_CRITICAL("  " __VA_ARGS__);                             \
                LUCENT_DEBUG_BREAK();                                               \
            }                                                                       \
        } while (false)
#else
    #define LUCENT_ASSERT(condition, ...) ((void)0)
    #define LUCENT_CORE_ASSERT(condition, ...) ((void)0)
#endif

// Verify is like assert but always evaluates the condition
#define LUCENT_VERIFY(condition, ...)                                              \
    do {                                                                            \
        if (!(condition)) {                                                         \
            LUCENT_CORE_CRITICAL("Verification failed: {}", #condition);            \
            LUCENT_CORE_CRITICAL("  File: {}:{}", ::lucent::ExtractFilename(__FILE__), __LINE__); \
            LUCENT_CORE_CRITICAL("  " __VA_ARGS__);                                 \
            LUCENT_DEBUG_BREAK();                                                   \
        }                                                                           \
    } while (false)

// Fatal error - always terminates
#define LUCENT_FATAL(...)                                                          \
    do {                                                                            \
        LUCENT_CORE_CRITICAL("FATAL ERROR");                                        \
        LUCENT_CORE_CRITICAL("  File: {}:{}", ::lucent::ExtractFilename(__FILE__), __LINE__); \
        LUCENT_CORE_CRITICAL("  " __VA_ARGS__);                                     \
        std::abort();                                                               \
    } while (false)

