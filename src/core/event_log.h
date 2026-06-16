// OhMyDisplay - Windows Event Log correlator for power events (wevtapi).
#pragma once

#include <string>
#include <vector>

namespace omd
{
    struct PowerEvent
    {
        std::string timeUtc;   // ISO-8601 UTC from the event
        std::string provider;  // short provider tag (Kernel-Power / Power-Troubleshooter)
        int eventId = 0;
        std::string summary;   // human-readable line
        std::string wakeSource; // populated for Power-Troubleshooter id 1
    };

    // Query recent power-related System log events, newest first, up to maxEvents.
    std::vector<PowerEvent> QueryPowerTimeline(int maxEvents = 40);
}
