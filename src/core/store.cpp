// OhMyDisplay - atomic JSON store implementation.
#include "core/store.h"
#include "core/logger.h"

#include <windows.h>
#include <vector>

namespace omd
{
    bool Store::ReadAllText(const std::wstring& path, std::string& out)
    {
        bool ok = false;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        do
        {
            if (h == INVALID_HANDLE_VALUE)
            {
                break;
            }
            LARGE_INTEGER sz;
            if (GetFileSizeEx(h, &sz) == 0)
            {
                break;
            }
            if (sz.QuadPart <= 0 || sz.QuadPart > (64ll * 1024 * 1024))
            {
                // Empty or implausibly large; treat as not loadable.
                break;
            }
            out.resize(static_cast<size_t>(sz.QuadPart));
            DWORD read = 0;
            if (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) == 0)
            {
                break;
            }
            out.resize(read);
            // Strip a UTF-8 BOM if present.
            if (out.size() >= 3 &&
                static_cast<unsigned char>(out[0]) == 0xEF &&
                static_cast<unsigned char>(out[1]) == 0xBB &&
                static_cast<unsigned char>(out[2]) == 0xBF)
            {
                out.erase(0, 3);
            }
            ok = true;
        } while (false);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
        }
        return ok;
    }

    bool Store::WriteAllText(const std::wstring& path, const std::string& text)
    {
        bool ok = false;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        do
        {
            if (h == INVALID_HANDLE_VALUE)
            {
                break;
            }
            DWORD written = 0;
            if (WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) == 0)
            {
                break;
            }
            if (written != text.size())
            {
                break;
            }
            FlushFileBuffers(h);
            ok = true;
        } while (false);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
        }
        return ok;
    }

    bool Store::AtomicReplace(const std::wstring& tmp, const std::wstring& target)
    {
        bool ok = false;
        do
        {
            // If the target does not exist yet, a plain move is sufficient.
            DWORD attr = GetFileAttributesW(target.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
            {
                if (MoveFileExW(tmp.c_str(), target.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
                {
                    ok = true;
                }
                break;
            }
            // ReplaceFile preserves attributes/ACLs and is atomic on the same volume.
            if (ReplaceFileW(target.c_str(), tmp.c_str(), nullptr,
                             REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != 0)
            {
                ok = true;
                break;
            }
            // Fall back to MoveFileEx if ReplaceFile is unavailable for this target.
            if (MoveFileExW(tmp.c_str(), target.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
            {
                ok = true;
            }
        } while (false);
        return ok;
    }

    bool Store::Load(const std::wstring& path, json::Value& out)
    {
        bool ok = false;
        do
        {
            std::string text;
            if (!ReadAllText(path, text))
            {
                break;
            }
            std::string err;
            if (!json::Value::Parse(text, out, &err))
            {
                OMD_LOGE("store: parse failed for %ls: %s", path.c_str(), err.c_str());
                // Back up the corrupt file so the next run can regenerate cleanly.
                std::wstring bak = path + L".bak";
                DeleteFileW(bak.c_str());
                MoveFileW(path.c_str(), bak.c_str());
                break;
            }
            ok = true;
        } while (false);
        return ok;
    }

    bool Store::Save(const std::wstring& path, const json::Value& value)
    {
        bool ok = false;
        do
        {
            std::string text = value.Dump(true);
            std::wstring tmp = path + L".tmp";
            if (!WriteAllText(tmp, text))
            {
                OMD_LOGE("store: temp write failed for %ls", path.c_str());
                break;
            }
            if (!AtomicReplace(tmp, path))
            {
                OMD_LOGE("store: atomic replace failed for %ls", path.c_str());
                DeleteFileW(tmp.c_str());
                break;
            }
            ok = true;
        } while (false);
        return ok;
    }
}
