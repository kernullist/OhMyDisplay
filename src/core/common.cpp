// OhMyDisplay - common helper implementations.
#include "core/common.h"

#include <algorithm>
#include <cctype>

namespace omd
{
    std::string Utf8FromWide(std::wstring_view w)
    {
        std::string out;
        do
        {
            if (w.empty())
            {
                break;
            }
            int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                             nullptr, 0, nullptr, nullptr);
            if (needed <= 0)
            {
                break;
            }
            out.resize(static_cast<size_t>(needed));
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                out.data(), needed, nullptr, nullptr);
        } while (false);
        return out;
    }

    std::wstring WideFromUtf8(std::string_view s)
    {
        std::wstring out;
        do
        {
            if (s.empty())
            {
                break;
            }
            int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                             nullptr, 0);
            if (needed <= 0)
            {
                break;
            }
            out.resize(static_cast<size_t>(needed));
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                out.data(), needed);
        } while (false);
        return out;
    }

    std::string HashHex(std::string_view bytes)
    {
        // FNV-1a 64-bit.
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : bytes)
        {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        char buf[17];
        const char* digits = "0123456789abcdef";
        for (int i = 15; i >= 0; --i)
        {
            buf[i] = digits[h & 0xF];
            h >>= 4;
        }
        buf[16] = '\0';
        return std::string(buf);
    }

    std::string_view Trim(std::string_view s)
    {
        size_t start = 0;
        size_t end = s.size();
        while (start < end && std::isspace(static_cast<unsigned char>(s[start])) != 0)
        {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0)
        {
            --end;
        }
        return s.substr(start, end - start);
    }

    bool IEquals(std::string_view a, std::string_view b)
    {
        bool equal = false;
        do
        {
            if (a.size() != b.size())
            {
                break;
            }
            equal = true;
            for (size_t i = 0; i < a.size(); ++i)
            {
                int ca = std::tolower(static_cast<unsigned char>(a[i]));
                int cb = std::tolower(static_cast<unsigned char>(b[i]));
                if (ca != cb)
                {
                    equal = false;
                    break;
                }
            }
        } while (false);
        return equal;
    }

    std::string ToLowerAscii(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c)
                       {
                           return static_cast<char>(std::tolower(c));
                       });
        return out;
    }
}
