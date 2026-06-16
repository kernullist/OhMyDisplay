// OhMyDisplay - Sleep Doctor orchestrator implementation.
#include "doctor/doctor.h"
#include "doctor/powercfg_collector.h"
#include "doctor/report.h"
#include "core/event_log.h"
#include "core/process_util.h"
#include "core/elevated_ipc.h"
#include "core/paths.h"
#include "core/store.h"
#include "core/json.h"
#include "core/common.h"
#include "core/logger.h"

#include <windows.h>
#include <shellapi.h>

namespace omd
{
    DoctorReport Doctor::Run(bool elevated)
    {
        DoctorReport report;
        report.data = CollectPowerData();
        if (elevated && (report.data.requestsNeedAdmin || report.data.waketimersNeedAdmin))
        {
            // Fill the admin-only sections via the elevated helper over named-pipe IPC.
            CollectElevated(report.data);
        }
        // SleepStudy only applies to Modern Standby systems and is opt-in (slower, admin).
        if (elevated && report.data.sleepStudy.modernStandby && settings_.doctor.collectSleepStudy)
        {
            CollectSleepStudyElevated(report.data.sleepStudy);
        }
        report.timeline = QueryPowerTimeline(40);
        report.findings = EvaluateRules(report.data, report.timeline);
        return report;
    }

    std::wstring Doctor::GenerateAndOpenReport(bool elevated)
    {
        DoctorReport report = Run(elevated);

        // Persist the plain-text version to the log directory for support bundles.
        OMD_LOGI("doctor: %d findings, %zu timeline events",
                 static_cast<int>(report.findings.size()), report.timeline.size());

        std::wstring path = WriteHtmlReport(report);
        if (!path.empty())
        {
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return path;
    }

    bool Doctor::ApplyDisableWake(const std::string& deviceName)
    {
        bool ok = false;
        do
        {
            if (deviceName.empty())
            {
                break;
            }
            json::Value req = json::Value::MakeObject();
            req.Set("cmd", json::Value(std::string("devicedisablewake")));
            req.Set("device", json::Value(deviceName));

            json::Value resp;
            if (!ElevatedRequest(req, resp))
            {
                OMD_LOGW("doctor: devicedisablewake IPC declined/failed");
                break;
            }
            bool respOk = resp["ok"].AsBool(false);
            int exitCode = resp["exitCode"].AsInt(-1);
            OMD_LOGI("doctor: devicedisablewake '%s' ok=%d exit=%d", deviceName.c_str(), respOk ? 1 : 0, exitCode);
            ok = respOk && exitCode == 0;
        } while (false);
        return ok;
    }

    bool Doctor::CollectSleepStudyElevated(SleepStudyData& out)
    {
        bool ok = false;
        do
        {
            json::Value req = json::Value::MakeObject();
            req.Set("cmd", json::Value(std::string("sleepstudy")));

            json::Value resp;
            if (!ElevatedRequest(req, resp))
            {
                OMD_LOGW("doctor: sleepstudy IPC declined/failed");
                break;
            }
            if (!resp["ok"].AsBool(false))
            {
                break;
            }
            out.collected = true;
            out.reportPath = WideFromUtf8(resp["reportPath"].AsStringOr(""));

            // Best-effort DRIPS summary from the generated report file.
            if (!out.reportPath.empty())
            {
                std::string text;
                HANDLE h = CreateFileW(out.reportPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    LARGE_INTEGER sz;
                    if (GetFileSizeEx(h, &sz) != 0 && sz.QuadPart > 0 && sz.QuadPart < (16ll << 20))
                    {
                        text.resize(static_cast<size_t>(sz.QuadPart));
                        DWORD readBytes = 0;
                        ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &readBytes, nullptr);
                        text.resize(readBytes);
                    }
                    CloseHandle(h);
                }
                if (!text.empty())
                {
                    out.avgDripsPercent = ParseDripsFromReport(text);
                }
            }
            OMD_LOGI("doctor: sleepstudy collected path=%ls drips=%.1f",
                     out.reportPath.c_str(), out.avgDripsPercent);
            ok = true;
        } while (false);
        return ok;
    }

    bool Doctor::CollectElevated(DoctorData& mergeInto)
    {
        bool ok = false;
        do
        {
            json::Value req = json::Value::MakeObject();
            req.Set("cmd", json::Value(std::string("collect")));

            json::Value resp;
            if (!ElevatedRequest(req, resp))
            {
                OMD_LOGW("doctor: elevated collect IPC declined/failed");
                break;
            }
            if (!resp["ok"].AsBool(false))
            {
                break;
            }
            std::string reqText = resp["requests"].AsStringOr("");
            std::string wtText = resp["waketimers"].AsStringOr("");

            if (!reqText.empty())
            {
                bool needAdmin = false;
                mergeInto.requests = ParseRequests(reqText, needAdmin);
                mergeInto.requestsNeedAdmin = needAdmin;
            }
            if (!wtText.empty())
            {
                bool needAdmin = false;
                mergeInto.waketimers = ParseWakeTimers(wtText, needAdmin);
                mergeInto.waketimersNeedAdmin = needAdmin;
            }
            ok = true;
        } while (false);
        return ok;
    }
}
