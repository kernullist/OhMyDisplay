// OhMyDisplay - lightweight rotating logger. ASCII output only.
#pragma once

#include <string>
#include <string_view>

namespace omd
{
    enum class LogLevel
    {
        Error = 0,
        Warn = 1,
        Info = 2,
        Debug = 3
    };

    class Logger
    {
    public:
        static Logger& Instance();

        // Initialize the log file under %LOCALAPPDATA%\OhMyDisplay\logs.
        // tag distinguishes processes (e.g. "main" or "elevated").
        void Init(const std::wstring& tag, LogLevel minLevel = LogLevel::Info);

        void SetMinLevel(LogLevel level)
        {
            minLevel_ = level;
        }

        // Write a preformatted ASCII message. Adds timestamp + level prefix.
        void Write(LogLevel level, std::string_view msg);

        // printf-style convenience (ASCII format strings only).
        void Logf(LogLevel level, const char* fmt, ...);

    private:
        Logger() = default;
        void RotateIfNeeded();

        std::wstring path_;
        LogLevel minLevel_ = LogLevel::Info;
        bool initialized_ = false;
        void* mutex_ = nullptr; // CRITICAL_SECTION* lazily allocated
    };
}

// Convenience macros.
#define OMD_LOGE(...) ::omd::Logger::Instance().Logf(::omd::LogLevel::Error, __VA_ARGS__)
#define OMD_LOGW(...) ::omd::Logger::Instance().Logf(::omd::LogLevel::Warn, __VA_ARGS__)
#define OMD_LOGI(...) ::omd::Logger::Instance().Logf(::omd::LogLevel::Info, __VA_ARGS__)
#define OMD_LOGD(...) ::omd::Logger::Instance().Logf(::omd::LogLevel::Debug, __VA_ARGS__)
