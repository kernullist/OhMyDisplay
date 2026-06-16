// OhMyDisplay - filesystem path helpers. All data lives under %LOCALAPPDATA%\OhMyDisplay.
#pragma once

#include <string>

namespace omd
{
    // Returns %LOCALAPPDATA%\OhMyDisplay (created if missing). Empty on failure.
    std::wstring AppDataDir();

    // Returns the logs subdirectory (created if missing).
    std::wstring LogDir();

    // Full path to a file directly under the app data dir.
    std::wstring DataFile(const std::wstring& name);

    // Full path of the running executable's directory.
    std::wstring ExeDir();

    // Full path to the elevated helper next to the main exe.
    std::wstring ElevatedHelperPath();
}
