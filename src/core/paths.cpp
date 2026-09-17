// OhMyDisplay - path helper implementations.
#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")

namespace omd
{
    namespace
    {
        std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
        {
            std::wstring out = a;
            if (!out.empty() && out.back() != L'\\')
            {
                out.push_back(L'\\');
            }
            out += b;
            return out;
        }

        bool EnsureDir(const std::wstring& dir)
        {
            bool ok = false;
            do
            {
                if (dir.empty())
                {
                    break;
                }
                DWORD attr = GetFileAttributesW(dir.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    ok = true;
                    break;
                }
                if (CreateDirectoryW(dir.c_str(), nullptr) != 0)
                {
                    ok = true;
                    break;
                }
                if (GetLastError() == ERROR_ALREADY_EXISTS)
                {
                    ok = true;
                }
            } while (false);
            return ok;
        }
    }

    std::wstring AppDataDir()
    {
        std::wstring result;
        do
        {
            PWSTR raw = nullptr;
            HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
            if (FAILED(hr) || raw == nullptr)
            {
                if (raw != nullptr)
                {
                    CoTaskMemFree(raw);
                }
                break;
            }
            std::wstring base(raw);
            CoTaskMemFree(raw);
            std::wstring dir = JoinPath(base, L"OhMyDisplay");
            if (!EnsureDir(dir))
            {
                break;
            }
            result = dir;
        } while (false);
        return result;
    }

    std::wstring LogDir()
    {
        std::wstring result;
        do
        {
            std::wstring base = AppDataDir();
            if (base.empty())
            {
                break;
            }
            std::wstring dir = JoinPath(base, L"logs");
            if (!EnsureDir(dir))
            {
                break;
            }
            result = dir;
        } while (false);
        return result;
    }

    std::wstring DataFile(const std::wstring& name)
    {
        std::wstring base = AppDataDir();
        if (base.empty())
        {
            return std::wstring();
        }
        return JoinPath(base, name);
    }

    std::wstring ExeDir()
    {
        std::wstring result;
        do
        {
            wchar_t buf[32768];
            DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
            if (n == 0 || n >= ARRAYSIZE(buf))
            {
                break;
            }
            std::wstring full(buf, n);
            size_t pos = full.find_last_of(L'\\');
            if (pos == std::wstring::npos)
            {
                break;
            }
            result = full.substr(0, pos);
        } while (false);
        return result;
    }

    std::wstring ElevatedHelperPath()
    {
        std::wstring dir = ExeDir();
        if (dir.empty())
        {
            return std::wstring();
        }
        return JoinPath(dir, L"OhMyDisplay.Elevated.exe");
    }
}
