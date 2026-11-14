#include "common/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <system_error>

namespace lotro_uie {

namespace {

std::mutex g_logMutex;
FILE* g_logFile = nullptr;
std::filesystem::path g_logPath;
std::atomic<int> g_logLevel{static_cast<int>(LogLevel::Info)};

const char* LevelTag(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO ";
}

} // namespace

bool Logger::Init(const std::filesystem::path& logFilePath, LogLevel level) {
    g_logLevel.store(static_cast<int>(level), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logPath.clear();

    if (logFilePath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(logFilePath.parent_path(), ec);
    }

    g_logFile = _wfsopen(logFilePath.c_str(), L"w", _SH_DENYWR);
    if (!g_logFile) {
        OutputDebugStringA("[LOTRO-UIE] Failed to open log file\n");
        return false;
    }

    g_logPath = logFilePath;
    std::fprintf(g_logFile, "=== LOTRO-UIE Log Started ===\n");
    std::fflush(g_logFile);
    OutputDebugStringA("[LOTRO-UIE] Log started\n");
    return true;
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        std::fprintf(g_logFile, "=== LOTRO-UIE Log Closed ===\n");
        std::fflush(g_logFile);
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logPath.clear();
}

void Logger::SetLevel(LogLevel level) {
    g_logLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel Logger::GetLevel() {
    return static_cast<LogLevel>(g_logLevel.load(std::memory_order_relaxed));
}

std::filesystem::path Logger::GetLogPath() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logPath;
}

void Logger::Log(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) < g_logLevel.load(std::memory_order_relaxed)) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const DWORD tid = GetCurrentThreadId();

    char msg[2048]{};
    va_list args;
    va_start(args, fmt);
    const int formatted = std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (formatted < 0) {
        std::snprintf(msg, sizeof(msg), "<log format error>");
    }

    bool emitDebugString = (level >= LogLevel::Warn);
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile) {
            std::fprintf(
                g_logFile,
                "%02u:%02u:%02u.%03u [T:%5lu] [%s] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                static_cast<unsigned long>(tid),
                LevelTag(level),
                msg
            );
            if (level >= LogLevel::Info) {
                std::fflush(g_logFile);
            }
        } else {
            emitDebugString = true;
        }
    }

    if (emitDebugString) {
        char dbg[2160];
        std::snprintf(dbg, sizeof(dbg), "[LOTRO-UIE] [%s] %s\n", LevelTag(level), msg);
        OutputDebugStringA(dbg);
    }
}

} // namespace lotro_uie
