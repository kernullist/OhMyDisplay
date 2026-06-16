// OhMyDisplay elevated helper (OhMyDisplay.Elevated.exe).
//
// requireAdministrator. The main process spawns this only when an admin action is
// needed, passing a verb on the command line. Supported verbs:
//
//   ipc <pipeName>               Primary path: connect to main's named pipe and serve
//                                one request (collect / devicedisablewake).
//   collect <outfile>            Legacy: run admin-only powercfg queries and write their
//                                raw text into a JSON file the main process reads back.
//   devicedisablewake <name>     Legacy: safe, reversible powercfg /devicedisablewake.
//
// Exit code 0 means success; non-zero indicates failure.
#include "core/process_util.h"
#include "core/elevated_ipc.h"
#include "core/store.h"
#include "core/json.h"
#include "core/common.h"
#include "core/logger.h"

#include <windows.h>
#include <string>

using namespace omd;

namespace
{
    int DoCollect(const std::wstring& outFile)
    {
        int rc = 1;
        do
        {
            if (outFile.empty())
            {
                break;
            }
            ProcessResult requests = RunCapture(L"powercfg /requests");
            ProcessResult waketimers = RunCapture(L"powercfg /waketimers");

            json::Value root = json::Value::MakeObject();
            root.Set("requests", json::Value(requests.launched ? requests.output : std::string()));
            root.Set("waketimers", json::Value(waketimers.launched ? waketimers.output : std::string()));

            if (!Store::Save(outFile, root))
            {
                OMD_LOGE("helper: failed to write collect output %ls", outFile.c_str());
                break;
            }
            OMD_LOGI("helper: collect written to %ls", outFile.c_str());
            rc = 0;
        } while (false);
        return rc;
    }

    int DoDisableWake(const std::wstring& deviceName)
    {
        int rc = 1;
        do
        {
            if (deviceName.empty())
            {
                break;
            }
            // Quote the device name; it commonly contains spaces.
            std::wstring cmd = L"powercfg /devicedisablewake \"" + deviceName + L"\"";
            ProcessResult res = RunCapture(cmd);
            if (!res.launched)
            {
                break;
            }
            OMD_LOGI("helper: devicedisablewake exit=%lu name=%ls", res.exitCode, deviceName.c_str());
            rc = static_cast<int>(res.exitCode);
        } while (false);
        return rc;
    }
}

int wmain(int argc, wchar_t** argv)
{
    Logger::Instance().Init(L"elevated", LogLevel::Info);

    if (argc < 2)
    {
        OMD_LOGW("helper: no verb provided");
        return 2;
    }

    std::wstring verb = argv[1];
    int rc = 2;
    do
    {
        if (verb == L"ipc" && argc >= 3)
        {
            rc = ElevatedServe(argv[2]);
            break;
        }
        if (verb == L"collect" && argc >= 3)
        {
            rc = DoCollect(argv[2]);
            break;
        }
        if (verb == L"devicedisablewake" && argc >= 3)
        {
            rc = DoDisableWake(argv[2]);
            break;
        }
        OMD_LOGW("helper: unknown or malformed verb '%ls'", verb.c_str());
    } while (false);

    return rc;
}
