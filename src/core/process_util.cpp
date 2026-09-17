// OhMyDisplay - process execution helpers.
#include "core/process_util.h"
#include "core/common.h"
#include "core/logger.h"

#include <shellapi.h>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace omd
{
    ProcessResult RunCapture(const std::wstring& commandLine, DWORD timeoutMs)
    {
        ProcessResult result;

        SECURITY_ATTRIBUTES sa;
        ZeroMemory(&sa, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        do
        {
            if (CreatePipe(&readPipe, &writePipe, &sa, 0) == 0)
            {
                OMD_LOGE("run: CreatePipe failed gle=%lu", GetLastError());
                break;
            }
            // The child must not inherit the read end.
            SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = writePipe;
            si.hStdError = writePipe;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

            std::wstring mutableCmd = commandLine; // CreateProcessW may modify the buffer
            if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) == 0)
            {
                OMD_LOGE("run: CreateProcess failed gle=%lu cmd=%ls", GetLastError(), commandLine.c_str());
                break;
            }
            result.launched = true;

            // Close our copy of the write end so ReadFile sees EOF when the child exits.
            CloseHandle(writePipe);
            writePipe = nullptr;

            // Drain stdout while waiting. Waiting first can deadlock when the pipe fills;
            // reading first made timeoutMs a no-op because ReadFile blocked until exit.
            // timeoutMs == 0 means "use the 15s default"; INFINITE means wait forever.
            std::string raw;
            const size_t kMaxOutput = 16u * 1024u * 1024u;
            DWORD waitMs = timeoutMs;
            if (waitMs == 0)
            {
                waitMs = 15000;
            }
            const ULONGLONG start = GetTickCount64();
            const ULONGLONG budget = (waitMs == INFINITE)
                                         ? ~0ull
                                         : static_cast<ULONGLONG>(waitMs);
            bool timedOut = false;
            bool childDone = false;
            while (!childDone)
            {
                if (!timedOut && (GetTickCount64() - start) >= budget)
                {
                    timedOut = true;
                    TerminateProcess(pi.hProcess, 1);
                }

                DWORD avail = 0;
                if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) != 0 && avail > 0)
                {
                    char buf[4096];
                    DWORD toRead = (avail > sizeof(buf)) ? static_cast<DWORD>(sizeof(buf)) : avail;
                    DWORD got = 0;
                    if (ReadFile(readPipe, buf, toRead, &got, nullptr) != 0 && got > 0)
                    {
                        if (raw.size() < kMaxOutput)
                        {
                            size_t room = kMaxOutput - raw.size();
                            raw.append(buf, (static_cast<size_t>(got) > room) ? room : static_cast<size_t>(got));
                        }
                        if (raw.size() >= kMaxOutput)
                        {
                            timedOut = true;
                            TerminateProcess(pi.hProcess, 1);
                            // Fall through to wait; do not spin-read the rest of the pipe.
                        }
                        else
                        {
                            continue;
                        }
                    }
                }

                DWORD wait = WaitForSingleObject(pi.hProcess, timedOut ? 0 : 50);
                if (wait == WAIT_OBJECT_0)
                {
                    // Final drain.
                    DWORD leftover = 0;
                    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &leftover, nullptr) != 0 &&
                           leftover > 0)
                    {
                        if (raw.size() >= kMaxOutput)
                        {
                            break;
                        }
                        char buf[4096];
                        DWORD toRead = (leftover > sizeof(buf)) ? static_cast<DWORD>(sizeof(buf)) : leftover;
                        DWORD got = 0;
                        if (ReadFile(readPipe, buf, toRead, &got, nullptr) == 0 || got == 0)
                        {
                            break;
                        }
                        size_t room = kMaxOutput - raw.size();
                        raw.append(buf, (static_cast<size_t>(got) > room) ? room : static_cast<size_t>(got));
                    }
                    childDone = true;
                }
                else if (timedOut)
                {
                    WaitForSingleObject(pi.hProcess, 2000);
                    childDone = true;
                }
            }

            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            if (code == STILL_ACTIVE)
            {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
                code = 1;
            }
            result.exitCode = code;

            // powercfg emits ANSI/OEM text; convert from the console code page to UTF-8.
            std::wstring wide;
            if (!raw.empty())
            {
                UINT cp = GetOEMCP();
                int needed = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()),
                                                 nullptr, 0);
                if (needed > 0)
                {
                    wide.resize(static_cast<size_t>(needed));
                    MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()),
                                        wide.data(), needed);
                }
            }
            result.output = Utf8FromWide(wide);
        } while (false);

        if (writePipe != nullptr)
        {
            CloseHandle(writePipe);
        }
        if (readPipe != nullptr)
        {
            CloseHandle(readPipe);
        }
        if (pi.hProcess != nullptr)
        {
            CloseHandle(pi.hProcess);
        }
        if (pi.hThread != nullptr)
        {
            CloseHandle(pi.hThread);
        }
        return result;
    }

    std::wstring ProcessImagePathById(DWORD pid)
    {
        std::wstring out;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        do
        {
            if (h == nullptr)
            {
                break;
            }
            wchar_t buf[1024];
            DWORD sz = ARRAYSIZE(buf);
            if (QueryFullProcessImageNameW(h, 0, buf, &sz) != 0)
            {
                out.assign(buf, sz);
            }
        } while (false);
        if (h != nullptr)
        {
            CloseHandle(h);
        }
        return out;
    }

    int RunElevated(const std::wstring& exePath, const std::wstring& parameters, DWORD timeoutMs)
    {
        int rc = -1;
        SHELLEXECUTEINFOW sei;
        ZeroMemory(&sei, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = exePath.c_str();
        sei.lpParameters = parameters.c_str();
        sei.nShow = SW_HIDE;

        do
        {
            if (ShellExecuteExW(&sei) == 0)
            {
                OMD_LOGW("elevate: ShellExecuteEx failed gle=%lu (user declined?)", GetLastError());
                break;
            }
            if (sei.hProcess == nullptr)
            {
                break;
            }
            WaitForSingleObject(sei.hProcess, timeoutMs);
            DWORD code = 0;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            rc = static_cast<int>(code);
        } while (false);
        return rc;
    }
}
