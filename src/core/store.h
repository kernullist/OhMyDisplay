// OhMyDisplay - atomic JSON file store. Writes go to a temp file then atomically
// replace the target to avoid partial corruption.
#pragma once

#include "core/json.h"
#include <string>

namespace omd
{
    class Store
    {
    public:
        // Read and parse a JSON file into out. Returns false if missing/corrupt.
        // On corruption the bad file is backed up to <path>.bak.
        static bool Load(const std::wstring& path, json::Value& out);

        // Serialize value and write atomically (temp + ReplaceFile/MoveFileEx).
        static bool Save(const std::wstring& path, const json::Value& value);

    private:
        static bool ReadAllText(const std::wstring& path, std::string& out);
        static bool WriteAllText(const std::wstring& path, const std::string& text);
        static bool AtomicReplace(const std::wstring& tmp, const std::wstring& target);
    };
}
