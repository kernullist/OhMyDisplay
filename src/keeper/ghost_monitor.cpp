// OhMyDisplay - ghost monitor detection implementation.
#include "keeper/ghost_monitor.h"
#include "core/common.h"
#include "core/logger.h"

#include <windows.h>
#include <vector>

namespace omd
{
    std::vector<GhostMonitor> DetectGhostMonitors()
    {
        std::vector<GhostMonitor> ghosts;
        do
        {
            UINT32 numPaths = 0;
            UINT32 numModes = 0;
            // QDC_ALL_PATHS returns active and inactive (remembered) paths.
            LONG rc = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes);
            if (rc != ERROR_SUCCESS)
            {
                break;
            }
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
            rc = QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths.data(),
                                    &numModes, modes.data(), nullptr);
            if (rc != ERROR_SUCCESS)
            {
                break;
            }
            paths.resize(numPaths);

            for (const auto& p : paths)
            {
                bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
                if (active)
                {
                    continue;
                }
                // Inactive path: query target name; a known device path with the
                // target currently unavailable is a ghost.
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
                ZeroMemory(&tgt, sizeof(tgt));
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof(tgt);
                tgt.header.adapterId = p.targetInfo.adapterId;
                tgt.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
                {
                    continue;
                }
                // Skip targets that report as currently available (likely just off,
                // not a phantom). targetAvailable is set for connected targets.
                if (tgt.flags.friendlyNameFromEdid == 0 && tgt.monitorDevicePath[0] == L'\0')
                {
                    continue;
                }
                if (p.targetInfo.targetAvailable != FALSE)
                {
                    continue;
                }
                GhostMonitor g;
                g.friendlyName = Utf8FromWide(tgt.monitorFriendlyDeviceName);
                g.devicePath = Utf8FromWide(tgt.monitorDevicePath);
                if (g.friendlyName.empty() && g.devicePath.empty())
                {
                    continue;
                }
                ghosts.push_back(std::move(g));
            }
        } while (false);

        if (!ghosts.empty())
        {
            OMD_LOGI("ghost: detected %zu phantom monitor target(s)", ghosts.size());
        }
        return ghosts;
    }
}
