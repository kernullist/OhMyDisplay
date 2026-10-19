// OhMyDisplay - persisted settings model (settings.json).
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace omd
{
    namespace json
    {
        class Value;
    }

    struct DebounceConfig
    {
        DWORD baseMs = 300;
        DWORD perMonitorMs = 250;
        DWORD minMs = 400;
        DWORD maxMs = 1500;
    };

    struct KeeperConfig
    {
        bool enabled = true;
        DebounceConfig debounce;
        int restoreRetries = 3;
        bool elevateForAllWindows = false;
        std::vector<std::string> excludeExe;                    // matched on file leaf, case-insensitive
        std::vector<std::string> excludeTitleSubstr;            // v0 substring match (regex in v1)
    };

    struct DoctorConfig
    {
        bool useWebView2 = true;
        bool collectSleepStudy = false;
    };

    struct HotkeyConfig
    {
        std::string saveNow = "Ctrl+Alt+S";
        std::string restoreNow = "Ctrl+Alt+R";
    };

    struct Settings
    {
        KeeperConfig keeper;
        DoctorConfig doctor;
        HotkeyConfig hotkeys;

        // Load from %LOCALAPPDATA%\OhMyDisplay\settings.json. Missing file is OK
        // (defaults are kept). Returns false only on unexpected error.
        bool Load();

        // Apply a parsed settings.json document over the current values (migrates
        // older versions). Missing keys keep their current values.
        bool FromJson(const json::Value& root);

        // Save current settings atomically.
        bool Save() const;

        static std::wstring FilePath();
    };
}
