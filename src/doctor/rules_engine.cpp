// OhMyDisplay - diagnostic rule engine implementation.
#include "doctor/rules_engine.h"
#include "core/common.h"

#include <algorithm>

namespace omd
{
    namespace
    {
        std::string LeafName(const std::string& path)
        {
            size_t pos = path.find_last_of("\\/");
            return pos == std::string::npos ? path : path.substr(pos + 1);
        }

        bool MentionsAudio(const PowerRequest& r)
        {
            std::string low = ToLowerAscii(r.value + " " + r.reason);
            return low.find("audio") != std::string::npos ||
                   low.find("sound") != std::string::npos ||
                   low.find("media") != std::string::npos;
        }

        bool LooksLikeNic(const std::string& s)
        {
            std::string low = ToLowerAscii(s);
            return low.find("ethernet") != std::string::npos ||
                   low.find("network") != std::string::npos ||
                   low.find("nic") != std::string::npos ||
                   low.find("realtek") != std::string::npos ||
                   low.find("intel(r) ethernet") != std::string::npos ||
                   low.find("gbe") != std::string::npos ||
                   low.find("wi-fi") != std::string::npos ||
                   low.find("wireless") != std::string::npos;
        }
    }

    std::vector<Finding> EvaluateRules(const DoctorData& data, const std::vector<PowerEvent>& timeline)
    {
        std::vector<Finding> findings;

        // Rule: process/driver holding the system awake. Any entry in /requests is an
        // active request by definition, so we do not gate on the (localizable) category
        // name; we only use it to tell "display stays on" from "won't sleep".
        for (const auto& r : data.requests)
        {
            std::string catLow = ToLowerAscii(r.category);
            bool displayish = catLow.find("display") != std::string::npos;

            Finding f;
            f.severity = Severity::Warn;
            std::string who = r.value.empty() ? r.tag : LeafName(r.value);

            if (IEquals(r.tag, "PROCESS"))
            {
                f.symptom = displayish ? "Display stays on" : "Won't sleep";
                f.plain = who + " is holding the " + (displayish ? "display" : "system") + " awake";
                if (!r.reason.empty())
                {
                    f.plain += " (" + r.reason + ")";
                }
                f.action = "Close " + who + ", or override this request from its settings";
                f.actionKind = ActionKind::Advice;
            }
            else if (IEquals(r.tag, "DRIVER"))
            {
                f.symptom = "Won't sleep";
                if (MentionsAudio(r))
                {
                    f.plain = "An audio driver reports an active stream, blocking sleep";
                    f.action = "Stop the app playing audio (browser tab, media player)";
                }
                else
                {
                    f.plain = "A driver request is blocking sleep: " + who;
                    f.action = "Check the related device/app";
                }
                f.actionKind = ActionKind::Advice;
            }
            else
            {
                f.symptom = "Won't sleep";
                f.plain = r.category + " request by [" + r.tag + "] " + who;
                f.action = "Review this requester";
                f.actionKind = ActionKind::Advice;
            }
            findings.push_back(std::move(f));
        }

        // Rule: last wake caused by a network device -> likely Wake-on-LAN.
        if (!data.lastWakeName.empty() && LooksLikeNic(data.lastWakeName))
        {
            Finding f;
            f.severity = Severity::Warn;
            f.symptom = "Keeps waking up";
            f.plain = "Last wake was caused by " + data.lastWakeName +
                      " (likely Wake-on-LAN / magic packet)";
            f.action = "Disable wake for this network adapter";
            f.actionKind = ActionKind::DeviceDisableWake;
            f.actionArg = data.lastWakeName;
            findings.push_back(std::move(f));
        }
        else if (!data.lastWakeName.empty())
        {
            Finding f;
            f.severity = Severity::Info;
            f.symptom = "Last wake";
            f.plain = "Last wake source: " + data.lastWakeName +
                      (data.lastWakeType.empty() ? "" : (" [" + data.lastWakeType + "]"));
            f.action = "If unexpected, disable wake for this device";
            f.actionKind = ActionKind::DeviceDisableWake;
            f.actionArg = data.lastWakeName;
            findings.push_back(std::move(f));
        }
        else if (data.lastWakeEmpty)
        {
            // /lastwake was blank: rely on the event-log timeline for a wake reason.
            for (const auto& ev : timeline)
            {
                if (ev.provider == "Power-Troubleshooter" && !ev.wakeSource.empty())
                {
                    Finding f;
                    f.severity = Severity::Info;
                    f.symptom = "Last wake (from event log)";
                    f.plain = "Wake source recorded in the event log: " + ev.wakeSource;
                    f.action = "If this is a device, consider disabling its wake capability";
                    if (LooksLikeNic(ev.wakeSource))
                    {
                        f.actionKind = ActionKind::DeviceDisableWake;
                        f.actionArg = ev.wakeSource;
                    }
                    findings.push_back(std::move(f));
                    break;
                }
            }
        }

        // Rule: wake timers armed.
        for (const auto& w : data.waketimers)
        {
            Finding f;
            f.severity = Severity::Warn;
            f.symptom = "Keeps waking up";
            f.plain = "A wake timer is set by " + w.owner +
                      (w.reason.empty() ? "" : (" - " + w.reason));
            f.action = "Remove the wake permission on this scheduled task";
            f.actionKind = ActionKind::Advice;
            findings.push_back(std::move(f));
        }

        // Informational: armed wake devices.
        if (!data.wakeArmedDevices.empty())
        {
            Finding f;
            f.severity = Severity::Info;
            f.symptom = "Wake-capable devices";
            std::string list;
            for (size_t i = 0; i < data.wakeArmedDevices.size(); ++i)
            {
                if (i != 0)
                {
                    list += ", ";
                }
                list += data.wakeArmedDevices[i];
            }
            f.plain = "These devices are allowed to wake the PC: " + list;
            f.action = "Disable wake on any you do not want waking the PC";
            f.actionKind = ActionKind::Advice;
            findings.push_back(std::move(f));
        }

        // Sleep architecture (S3 vs Modern Standby) and DRIPS, if collected.
        {
            const SleepStudyData& ss = data.sleepStudy;
            if (ss.modernStandby)
            {
                if (ss.collected && ss.avgDripsPercent >= 0.0)
                {
                    Finding f;
                    char pct[16];
                    std::snprintf(pct, sizeof(pct), "%.0f%%", ss.avgDripsPercent);
                    if (ss.avgDripsPercent < 70.0)
                    {
                        f.severity = Severity::Warn;
                        f.symptom = "Battery drain (Modern Standby)";
                        f.plain = std::string("Low deep-idle residency (DRIPS ") + pct +
                                  "); the PC is not reaching its lowest-power state in sleep";
                        f.action = "Open the SleepStudy report to see the offending components";
                    }
                    else
                    {
                        f.severity = Severity::Info;
                        f.symptom = "Modern Standby";
                        f.plain = std::string("Deep-idle residency looks healthy (DRIPS ") + pct + ")";
                        f.action = "Nothing to do";
                    }
                    findings.push_back(std::move(f));
                }
                else
                {
                    Finding f;
                    f.severity = Severity::Info;
                    f.symptom = "Modern Standby";
                    f.plain = "This PC uses Modern Standby (S0 low power idle)";
                    f.action = ss.collected ? "SleepStudy report generated"
                                            : "Enable SleepStudy collection and re-run (admin) for DRIPS detail";
                    findings.push_back(std::move(f));
                }
            }
            else
            {
                Finding f;
                f.severity = Severity::Info;
                f.symptom = "Sleep architecture";
                f.plain = "This PC uses legacy sleep (S3); Modern Standby / SleepStudy does not apply";
                f.action = "Nothing to do";
                findings.push_back(std::move(f));
            }
        }

        // Elevation hints when admin sections were blocked.
        if (data.requestsNeedAdmin || data.waketimersNeedAdmin)
        {
            Finding f;
            f.severity = Severity::Info;
            f.symptom = "Needs elevation";
            f.plain = "Some checks (power requests / wake timers) require administrator rights";
            f.action = "Re-run with elevation to collect the full report";
            f.actionKind = ActionKind::Advice;
            findings.push_back(std::move(f));
        }

        if (findings.empty())
        {
            Finding f;
            f.severity = Severity::Info;
            f.symptom = "All clear";
            f.plain = "No sleep blockers or unexpected wake sources were detected";
            f.action = "Nothing to do";
            f.actionKind = ActionKind::None;
            findings.push_back(std::move(f));
        }

        return findings;
    }
}
