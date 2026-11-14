#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace lotro_uie {

enum class LogLevel : int {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

class Logger {
public:
    static bool Init(const std::filesystem::path& logFilePath, LogLevel level);
    static void Shutdown();
    static void SetLevel(LogLevel level);
    static LogLevel GetLevel();
    static std::filesystem::path GetLogPath();

    static void Log(LogLevel level, const char* fmt, ...);
};

} // namespace lotro_uie

#define UIE_LOG_DEBUG(...) ::lotro_uie::Logger::Log(::lotro_uie::LogLevel::Debug, __VA_ARGS__)
#define UIE_LOG_INFO(...)  ::lotro_uie::Logger::Log(::lotro_uie::LogLevel::Info, __VA_ARGS__)
#define UIE_LOG_WARN(...)  ::lotro_uie::Logger::Log(::lotro_uie::LogLevel::Warn, __VA_ARGS__)
#define UIE_LOG_ERROR(...) ::lotro_uie::Logger::Log(::lotro_uie::LogLevel::Error, __VA_ARGS__)
