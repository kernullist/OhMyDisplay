// OhMyDisplay - elevated helper IPC implementation.
#include "core/elevated_ipc.h"
#include "core/process_util.h"
#include "core/paths.h"
#include "core/store.h"
#include "core/common.h"
#include "core/logger.h"

#include <windows.h>
#include <shellapi.h>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace omd
{
    namespace
    {
        bool WriteAll(HANDLE pipe, const void* data, DWORD len)
        {
            const char* p = static_cast<const char*>(data);
            DWORD total = 0;
            while (total < len)
            {
                DWORD wrote = 0;
                if (WriteFile(pipe, p + total, len - total, &wrote, nullptr) == 0 || wrote == 0)
                {
                    return false;
                }
                total += wrote;
            }
            return true;
        }

        bool ReadAll(HANDLE pipe, void* data, DWORD len)
        {
            char* p = static_cast<char*>(data);
            DWORD total = 0;
            while (total < len)
            {
                DWORD got = 0;
                if (ReadFile(pipe, p + total, len - total, &got, nullptr) == 0 || got == 0)
                {
                    return false;
                }
                total += got;
            }
            return true;
        }

        std::wstring SameLeaf(const std::wstring& path)
        {
            size_t pos = path.find_last_of(L"\\/");
            return pos == std::wstring::npos ? path : path.substr(pos + 1);
        }

        // Build the JSON response for a single request (runs on the helper side).
        json::Value HandleRequest(const json::Value& req)
        {
            json::Value resp = json::Value::MakeObject();
            std::string cmd = req["cmd"].AsStringOr("");
            if (cmd == "collect")
            {
                ProcessResult requests = RunCapture(L"powercfg /requests");
                ProcessResult waketimers = RunCapture(L"powercfg /waketimers");
                resp.Set("ok", json::Value(true));
                resp.Set("requests", json::Value(requests.launched ? requests.output : std::string()));
                resp.Set("waketimers", json::Value(waketimers.launched ? waketimers.output : std::string()));
            }
            else if (cmd == "devicedisablewake")
            {
                std::string device = req["device"].AsStringOr("");
                if (device.empty())
                {
                    resp.Set("ok", json::Value(false));
                    resp.Set("error", json::Value(std::string("missing device")));
                }
                else
                {
                    std::wstring c = L"powercfg /devicedisablewake \"" + WideFromUtf8(device) + L"\"";
                    ProcessResult r = RunCapture(c);
                    resp.Set("ok", json::Value(r.launched));
                    resp.Set("exitCode", json::Value(static_cast<int>(r.exitCode)));
                }
            }
            else if (cmd == "sleepstudy")
            {
                // Generate the official SleepStudy HTML report into shared app data;
                // the main process reads it back by path.
                std::wstring out = DataFile(L"sleepstudy.html");
                std::wstring c = L"powercfg /sleepstudy /output \"" + out + L"\"";
                ProcessResult r = RunCapture(c, 60000);
                bool produced = r.launched && r.exitCode == 0;
                resp.Set("ok", json::Value(produced));
                resp.Set("exitCode", json::Value(static_cast<int>(r.exitCode)));
                resp.Set("reportPath", json::Value(Utf8FromWide(out)));
            }
            else
            {
                resp.Set("ok", json::Value(false));
                resp.Set("error", json::Value(std::string("unknown cmd")));
            }
            return resp;
        }
    }

    bool IpcWriteMessage(HANDLE pipe, const std::string& payload)
    {
        uint32_t len = static_cast<uint32_t>(payload.size());
        unsigned char hdr[4];
        hdr[0] = static_cast<unsigned char>(len & 0xFF);
        hdr[1] = static_cast<unsigned char>((len >> 8) & 0xFF);
        hdr[2] = static_cast<unsigned char>((len >> 16) & 0xFF);
        hdr[3] = static_cast<unsigned char>((len >> 24) & 0xFF);
        if (!WriteAll(pipe, hdr, 4))
        {
            return false;
        }
        if (len == 0)
        {
            return true;
        }
        return WriteAll(pipe, payload.data(), len);
    }

    bool IpcReadMessage(HANDLE pipe, std::string& payload, DWORD maxBytes)
    {
        unsigned char hdr[4];
        if (!ReadAll(pipe, hdr, 4))
        {
            return false;
        }
        uint32_t len = static_cast<uint32_t>(hdr[0]) |
                       (static_cast<uint32_t>(hdr[1]) << 8) |
                       (static_cast<uint32_t>(hdr[2]) << 16) |
                       (static_cast<uint32_t>(hdr[3]) << 24);
        if (len > maxBytes)
        {
            return false;
        }
        payload.resize(len);
        if (len == 0)
        {
            return true;
        }
        return ReadAll(pipe, payload.data(), len);
    }

    bool ElevatedRequest(const json::Value& request, json::Value& response)
    {
        bool ok = false;
        HANDLE pipe = INVALID_HANDLE_VALUE;
        HANDLE helperProc = nullptr;
        do
        {
            std::wstring helper = ElevatedHelperPath();
            if (helper.empty())
            {
                break;
            }

            // Unique pipe name: pid + tick + QPC (no rand() seed required).
            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            wchar_t name[128];
            swprintf_s(name, L"\\\\.\\pipe\\OhMyDisplay.%lu.%llu.%lld",
                       GetCurrentProcessId(),
                       GetTickCount64(),
                       static_cast<long long>(qpc.QuadPart));
            std::wstring pipeName = name;

            pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    1, 64 * 1024, 64 * 1024, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                OMD_LOGE("ipc: CreateNamedPipe failed gle=%lu", GetLastError());
                break;
            }

            // Launch the elevated helper as the client (UAC prompt here).
            std::wstring params = L"ipc \"" + pipeName + L"\"";
            SHELLEXECUTEINFOW sei;
            ZeroMemory(&sei, sizeof(sei));
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"runas";
            sei.lpFile = helper.c_str();
            sei.lpParameters = params.c_str();
            sei.nShow = SW_HIDE;
            if (ShellExecuteExW(&sei) == 0 || sei.hProcess == nullptr)
            {
                OMD_LOGW("ipc: elevation declined/failed gle=%lu", GetLastError());
                break;
            }
            helperProc = sei.hProcess;

            // Wait for the helper to connect.
            if (ConnectNamedPipe(pipe, nullptr) == 0 && GetLastError() != ERROR_PIPE_CONNECTED)
            {
                OMD_LOGE("ipc: ConnectNamedPipe failed gle=%lu", GetLastError());
                break;
            }

            // Verify the peer is our elevated helper by image path.
            ULONG clientPid = 0;
            if (GetNamedPipeClientProcessId(pipe, &clientPid) != 0)
            {
                std::wstring clientPath = ProcessImagePathById(clientPid);
                if (!clientPath.empty() && _wcsicmp(clientPath.c_str(), helper.c_str()) != 0)
                {
                    OMD_LOGE("ipc: client image mismatch (%ls)", clientPath.c_str());
                    break;
                }
            }

            if (!IpcWriteMessage(pipe, request.Dump(false)))
            {
                break;
            }
            std::string respText;
            if (!IpcReadMessage(pipe, respText))
            {
                break;
            }
            if (!json::Value::Parse(respText, response, nullptr))
            {
                break;
            }
            ok = true;
        } while (false);

        if (pipe != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
        if (helperProc != nullptr)
        {
            WaitForSingleObject(helperProc, 30000);
            CloseHandle(helperProc);
        }
        return ok;
    }

    int ElevatedServe(const std::wstring& pipeName)
    {
        int rc = 1;
        HANDLE pipe = INVALID_HANDLE_VALUE;
        do
        {
            if (pipeName.empty())
            {
                break;
            }
            // Connect to the server pipe (retry while busy).
            for (int attempt = 0; attempt < 10; ++attempt)
            {
                pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE)
                {
                    break;
                }
                if (GetLastError() != ERROR_PIPE_BUSY)
                {
                    break;
                }
                WaitNamedPipeW(pipeName.c_str(), 3000);
            }
            if (pipe == INVALID_HANDLE_VALUE)
            {
                OMD_LOGE("ipc: helper could not open pipe gle=%lu", GetLastError());
                break;
            }

            // Verify the server is our main exe sitting next to us.
            ULONG serverPid = 0;
            if (GetNamedPipeServerProcessId(pipe, &serverPid) != 0)
            {
                std::wstring serverPath = ProcessImagePathById(serverPid);
                std::wstring expected = ExeDir() + L"\\OhMyDisplay.exe";
                if (!serverPath.empty() && _wcsicmp(serverPath.c_str(), expected.c_str()) != 0)
                {
                    OMD_LOGE("ipc: server image mismatch (%ls)", serverPath.c_str());
                    break;
                }
            }

            std::string reqText;
            if (!IpcReadMessage(pipe, reqText))
            {
                break;
            }
            json::Value req;
            if (!json::Value::Parse(reqText, req, nullptr))
            {
                break;
            }
            json::Value resp = HandleRequest(req);
            if (!IpcWriteMessage(pipe, resp.Dump(false)))
            {
                break;
            }
            rc = 0;
        } while (false);

        if (pipe != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(pipe);
            CloseHandle(pipe);
        }
        return rc;
    }
}
