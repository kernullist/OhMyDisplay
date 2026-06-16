// OhMyDisplay - rotating logger implementation.
#include "core/logger.h"
#include "core/paths.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace omd
{
    namespace
    {
        const long long kMaxLogBytes = 1024 * 1024; // 1 MB before rotation.

        const char* LevelTag(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Error:
                return "ERROR";
            case LogLevel::Warn:
                return "WARN ";
            case LogLevel::Info:
                return "INFO ";
            case LogLevel::Debug:
                return "DEBUG";
            }
            return "?????";
        }
    }

    Logger& Logger::Instance()
    {
        static Logger inst;
        return inst;
    }

    void Logger::Init(const std::wstring& tag, LogLevel minLevel)
    {
        do
        {
            if (mutex_ == nullptr)
            {
                CRITICAL_SECTION* cs = new CRITICAL_SECTION();
                InitializeCriticalSection(cs);
                mutex_ = cs;
            }
            minLevel_ = minLevel;
            std::wstring dir = LogDir();
            if (dir.empty())
            {
                break;
            }
            path_ = dir + L"\\OhMyDisplay." + tag + L".log";
            initialized_ = true;
        } while (false);
    }

    void Logger::RotateIfNeeded()
    {
        do
        {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fad) == 0)
            {
                break;
            }
            LARGE_INTEGER sz;
            sz.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
            sz.LowPart = fad.nFileSizeLow;
            if (sz.QuadPart < kMaxLogBytes)
            {
                break;
            }
            std::wstring old = path_ + L".1";
            DeleteFileW(old.c_str());
            MoveFileW(path_.c_str(), old.c_str());
        } while (false);
    }

    void Logger::Write(LogLevel level, std::string_view msg)
    {
        do
        {
            if (!initialized_)
            {
                break;
            }
            if (static_cast<int>(level) > static_cast<int>(minLevel_))
            {
                break;
            }
            CRITICAL_SECTION* cs = static_cast<CRITICAL_SECTION*>(mutex_);
            if (cs != nullptr)
            {
                EnterCriticalSection(cs);
            }

            RotateIfNeeded();

            SYSTEMTIME st;
            GetLocalTime(&st);
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix), "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                          st.wMilliseconds, LevelTag(level));

            std::string line(prefix);
            line.append(msg);
            line.push_back('\r');
            line.push_back('\n');

            HANDLE h = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
                CloseHandle(h);
            }

            // Mirror to debugger output for live diagnosis.
            OutputDebugStringA(line.c_str());

            if (cs != nullptr)
            {
                LeaveCriticalSection(cs);
            }
        } while (false);
    }

    void Logger::Logf(LogLevel level, const char* fmt, ...)
    {
        do
        {
            if (!initialized_ || fmt == nullptr)
            {
                break;
            }
            if (static_cast<int>(level) > static_cast<int>(minLevel_))
            {
                break;
            }
            va_list args;
            va_start(args, fmt);
            va_list copy;
            va_copy(copy, args);
            int needed = std::vsnprintf(nullptr, 0, fmt, copy);
            va_end(copy);
            if (needed < 0)
            {
                va_end(args);
                break;
            }
            std::vector<char> buf(static_cast<size_t>(needed) + 1);
            std::vsnprintf(buf.data(), buf.size(), fmt, args);
            va_end(args);
            Write(level, std::string_view(buf.data(), static_cast<size_t>(needed)));
        } while (false);
    }
}
