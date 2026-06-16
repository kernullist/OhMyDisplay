// OhMyDisplay - run a child process and capture its combined stdout/stderr.
#pragma once

#include <windows.h>
#include <string>

namespace omd
{
    struct ProcessResult
    {
        bool launched = false;
        DWORD exitCode = 0;
        std::string output; // UTF-8, stdout+stderr merged
    };

    // Run a command line (e.g. L"powercfg /requests") capturing output. The process
    // runs hidden with no console window. timeoutMs caps the wait.
    ProcessResult RunCapture(const std::wstring& commandLine, DWORD timeoutMs = 15000);

    // Launch the elevated helper (ShellExecuteEx runas) and wait for it. Returns the
    // helper exit code, or -1 if the user declined the UAC prompt / launch failed.
    int RunElevated(const std::wstring& exePath, const std::wstring& parameters, DWORD timeoutMs = 60000);

    // Full image path of a process id (empty on failure). Used for IPC peer checks.
    std::wstring ProcessImagePathById(DWORD pid);
}
