// OhMyDisplay - common helpers shared across all modules.
// ASCII-only comments and logs per project style. Single-exit do-while(false) preferred.
#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <cstdint>

namespace omd
{
    // Convert a UTF-16 wide string to UTF-8.
    std::string Utf8FromWide(std::wstring_view w);

    // Convert a UTF-8 string to UTF-16 wide.
    std::wstring WideFromUtf8(std::string_view s);

    // FNV-1a 64-bit hash over raw bytes, returned as lowercase hex.
    std::string HashHex(std::string_view bytes);

    // Trim ASCII whitespace from both ends.
    std::string_view Trim(std::string_view s);

    // Case-insensitive ASCII equality.
    bool IEquals(std::string_view a, std::string_view b);

    // Lowercase a copy (ASCII only).
    std::string ToLowerAscii(std::string_view s);

    // RAII wrapper that frees a HANDLE on scope exit.
    class ScopedHandle
    {
    public:
        ScopedHandle() = default;
        explicit ScopedHandle(HANDLE h) : h_(h) {}
        ~ScopedHandle()
        {
            Reset();
        }

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        ScopedHandle(ScopedHandle&& other) noexcept : h_(other.h_)
        {
            other.h_ = nullptr;
        }
        ScopedHandle& operator=(ScopedHandle&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                h_ = other.h_;
                other.h_ = nullptr;
            }
            return *this;
        }

        HANDLE Get() const
        {
            return h_;
        }
        bool Valid() const
        {
            return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
        }
        void Reset(HANDLE h = nullptr)
        {
            if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(h_);
            }
            h_ = h;
        }
        HANDLE Release()
        {
            HANDLE t = h_;
            h_ = nullptr;
            return t;
        }

    private:
        HANDLE h_ = nullptr;
    };
}
