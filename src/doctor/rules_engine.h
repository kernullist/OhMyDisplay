// OhMyDisplay - diagnostic rule engine: turns collected data into plain-language
// findings with recommended actions (FR-B4).
#pragma once

#include "doctor/powercfg_collector.h"
#include "core/event_log.h"

#include <string>
#include <vector>

namespace omd
{
    enum class Severity
    {
        Info,
        Warn
    };

    enum class ActionKind
    {
        None,
        Advice,            // text-only guidance
        DeviceDisableWake  // safe, reversible: powercfg /devicedisablewake "<name>"
    };

    struct Finding
    {
        Severity severity = Severity::Info;
        std::string symptom; // short category, e.g. "Won't sleep"
        std::string plain;   // human explanation
        std::string action;  // recommended action text
        ActionKind actionKind = ActionKind::None;
        std::string actionArg; // device name for DeviceDisableWake
    };

    struct DoctorReport
    {
        std::vector<Finding> findings;
        DoctorData data;
        std::vector<PowerEvent> timeline;
    };

    // Evaluate findings from collected data plus the event timeline.
    std::vector<Finding> EvaluateRules(const DoctorData& data, const std::vector<PowerEvent>& timeline);
}
