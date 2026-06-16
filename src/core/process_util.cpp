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
        if (CreatePipe(&readPipe, &writePipe, &sa, 0) == 0)
        {
            OMD_LOGE("run: CreatePipe failed gle=%lu", GetLastError());
            return result;
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

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        std::wstring mutableCmd = commandLine; // CreateProcessW may modify the buffer
        BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (ok == 0)
        {
            OMD_LOGE("run: CreateProcess failed gle=%lu cmd=%ls", GetLastError(), commandLine.c_str());
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return result;
        }
        result.launched = true;

        // Close our copy of the write end so ReadFile sees EOF when the child exits.
        CloseHandle(writePipe);
        writePipe = nullptr;

        std::string raw;
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) != 0 && read > 0)
        {
            raw.append(buf, read);
        }
        CloseHandle(readPipe);

        WaitForSingleObject(pi.hProcess, timeoutMs);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exitCode = code;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // powercfg emits ANSI/OEM text; convert from the console code page to UTF-8.
        std::wstring wide;
        if (!raw.empty())
        {
            UINT cp = GetOEMCP();
            int needed = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
            if (needed > 0)
            {
                wide.resize(static_cast<size_t>(needed));
                MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), needed);
            }
        }
        result.output = Utf8FromWide(wide);
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
