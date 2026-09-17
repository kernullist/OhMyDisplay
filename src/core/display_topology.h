// OhMyDisplay - display topology and stable monitor identity via the CCD API.
// A topology is the set of active monitors; its signature ignores coordinates so a
// post-resume layout collapse still matches the same topology.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace omd
{
    struct EdidInfo
    {
        std::string mfg;       // 3-letter PNP id, e.g. "DEL"
        uint32_t product = 0;  // EDID product code
        std::string serial;    // optional serial string (often empty via CCD)
    };

    struct MonitorInfo
    {
        std::string id;            // stable identity "m_xxxxxxxxxxxxxxxx"
        std::string baseId;        // identity before any user discriminator
        std::string label;         // user-friendly label (filled by MonitorLabels)
        std::string friendlyName;  // CCD monitorFriendlyDeviceName (UTF-8)
        EdidInfo edid;
        std::string devicePath;    // CCD monitorDevicePath (UTF-8)
        std::string gdiName;       // GDI device name, e.g. "\\.\DISPLAY1"
        uint32_t connectorKey = 0; // adapterId+sourceId hash, for tie-break
        RECT rcMonitor{};
        RECT rcWork{};
        UINT dpi = 96;
        bool primary = false;
    };

    struct Topology
    {
        std::vector<MonitorInfo> monitors; // sorted by id for determinism
        std::string signature;             // hash over sorted identities

        int Count() const
        {
            return static_cast<int>(monitors.size());
        }
        bool Empty() const
        {
            return monitors.empty();
        }
        const MonitorInfo* FindById(const std::string& id) const;
        const MonitorInfo* FindByGdiName(const std::string& gdi) const;
    };

    // QueryDisplayConfig with a short retry on ERROR_INSUFFICIENT_BUFFER /
    // ERROR_GEN_FAILURE, which are common while monitors are hot-plugging.
    bool QueryDisplayConfigRetry(UINT32 flags,
                                 std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                                 std::vector<DISPLAYCONFIG_MODE_INFO>& modes);

    // Query the current active topology. Returns false only on hard CCD failure.
    bool QueryActiveTopology(Topology& out);

    // Compute the signature for a set of monitor identities (sorted, hashed).
    std::string ComputeSignature(const std::vector<std::string>& ids);

    // Map a window's owning HMONITOR to a monitor identity in the topology.
    // Returns empty string if no match.
    std::string MonitorIdForWindow(const Topology& topo, HWND hwnd);
}
