// OhMyDisplay - phantom/ghost monitor detection (FR-A5).
//
// A ghost is a display target the system still remembers (has a device path) but is
// not currently active. We detect and report these so the user can clean them up via
// Device Manager; automated removal needs device-node operations and is out of v0.
#pragma once

#include <string>
#include <vector>

namespace omd
{
    struct GhostMonitor
    {
        std::string friendlyName;
        std::string devicePath;
    };

    // Detect inactive-but-known display targets using CCD QDC_ALL_PATHS.
    std::vector<GhostMonitor> DetectGhostMonitors();
}
