// OhMyDisplay - powercfg collectors and section-based parsers.
#pragma once

#include <string>
#include <vector>

namespace omd
{
    struct PowerRequest
    {
        std::string category; // DISPLAY / SYSTEM / AWAYMODE / EXECUTION / PERFBOOST / ...
        std::string tag;      // PROCESS / DRIVER / SERVICE / ... (without brackets)
        std::string value;    // requester path or name
        std::string reason;   // following description line, if any
    };

    struct WakeTimer
    {
        std::string owner;  // "[SERVICE] \Device\...\svchost.exe"
        std::string reason; // reason text
        std::string expires;
    };

    // Modern Standby (S0 low power idle) vs legacy S3. SleepStudy/DRIPS only applies to
    // Modern Standby systems (FR-B2, design SS11 graceful degrade).
    struct SleepStudyData
    {
        bool modernStandby = false; // S0 low power idle enabled (registry CsEnabled)
        bool collected = false;     // a SleepStudy report was generated this run
        std::wstring reportPath;    // path to the official HTML report, if collected
        double avgDripsPercent = -1.0; // best-effort summary (-1 = unknown)
    };

    struct DoctorData
    {
        std::vector<PowerRequest> requests;
        bool requestsNeedAdmin = false;

        std::string lastWakeType;
        std::string lastWakeName; // friendly name or instance path
        bool lastWakeEmpty = false;

        std::vector<WakeTimer> waketimers;
        bool waketimersNeedAdmin = false;

        std::vector<std::string> wakeArmedDevices;

        SleepStudyData sleepStudy;
    };

    // True if Modern Standby (Connected Standby) is enabled. Locale-independent: reads
    // HKLM\SYSTEM\CurrentControlSet\Control\Power\CsEnabled.
    bool IsModernStandbyEnabled();

    // Best-effort average DRIPS percentage from a SleepStudy HTML report (-1 if not
    // found). The official report is the source of truth; this is only a one-line hint.
    double ParseDripsFromReport(const std::string& reportText);

    // Collect all non-admin data directly. Admin-only sections (requests, waketimers)
    // are attempted; on access-denied their *NeedAdmin flag is set so the shell can
    // offer to re-collect via the elevated helper.
    DoctorData CollectPowerData();

    // Parsers exposed for unit testing against fixtures.
    std::vector<PowerRequest> ParseRequests(const std::string& text, bool& needAdmin);
    std::vector<WakeTimer> ParseWakeTimers(const std::string& text, bool& needAdmin);
    void ParseLastWake(const std::string& text, std::string& type, std::string& name, bool& empty);
    std::vector<std::string> ParseWakeArmed(const std::string& text);
}
