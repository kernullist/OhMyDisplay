// OhMyDisplay - Sleep Doctor orchestrator: collect, evaluate, render, and run the
// small set of safe, reversible actions through the elevated helper.
#pragma once

#include "doctor/rules_engine.h"
#include "core/settings.h"

#include <string>

namespace omd
{
    class Doctor
    {
    public:
        explicit Doctor(Settings& settings) : settings_(settings) {}

        // Collect data + event timeline and evaluate rules. When elevated is true,
        // admin-only sections are gathered through the elevated helper (one UAC prompt).
        DoctorReport Run(bool elevated = false);

        // Run, write the HTML report, and open it in the default browser.
        // Returns the report path (empty on failure).
        std::wstring GenerateAndOpenReport(bool elevated = false);

        // Safe action: disable wake capability for a device via the elevated helper.
        // Returns true if the helper reported success.
        bool ApplyDisableWake(const std::string& deviceName);

        // Re-collect admin-only sections through the elevated helper over the named
        // pipe. Returns true on success.
        bool CollectElevated(DoctorData& mergeInto);

        // Generate + summarize the Modern Standby SleepStudy report via the elevated
        // helper. Only meaningful when Modern Standby is enabled. Returns true if a
        // report was produced.
        bool CollectSleepStudyElevated(SleepStudyData& out);

    private:
        Settings& settings_;
    };
}
