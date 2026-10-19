// OhMyDisplay - top-level window enumeration, capture and placement restore.
#pragma once

#include "core/display_topology.h"
#include "core/settings.h"

#include <windows.h>
#include <string>
#include <vector>

namespace omd
{
    // A captured window. Coordinates are normalized to the owning monitor work area
    // (0..1) so they survive resolution changes. The "normal" rect is the restored
    // geometry even when the window is currently minimized or maximized.
    struct WindowSnap
    {
        std::string exe;       // full image path (UTF-8)
        std::string exeLeaf;   // file name only, lowercase (UTF-8)
        std::string title;     // window title (UTF-8)
        std::string className; // window class (UTF-8)
        std::string monitorId; // owning monitor identity
        int showCmd = SW_SHOWNORMAL;
        double nx = 0.0;       // normalized left within work area
        double ny = 0.0;       // normalized top
        double nw = 0.0;       // normalized width
        double nh = 0.0;       // normalized height

        // Runtime-only fields (not persisted).
        HWND hwnd = nullptr;
        DWORD pid = 0;
    };

    // A live manageable window discovered at restore time.
    struct LiveWindow
    {
        HWND hwnd = nullptr;
        DWORD pid = 0;
        std::string exe;
        std::string exeLeaf;
        std::string title;
        std::string className;
        bool used = false;

        // Current geometry, same model as WindowSnap. Only used to break ties between
        // otherwise identical candidates (e.g. several Sticky Notes with one title).
        bool hasGeometry = false;
        std::string monitorId;
        double nx = 0.0;
        double ny = 0.0;
    };

    // Normalized rectangle (fractions of a monitor work area). Pure value type so the
    // coordinate math can be unit-tested without live windows/monitors.
    struct NormRect
    {
        double x = 0.0;
        double y = 0.0;
        double w = 0.0;
        double h = 0.0;
    };

    // Convert a screen-space rect to fractions of the given work area.
    NormRect NormalizeToWork(const RECT& screen, const RECT& work);

    // Convert normalized fractions back to a screen-space rect on the given work area.
    RECT DenormalizeFromWork(const NormRect& n, const RECT& work);

    // True if the window is one the keeper saves/restores: what the taskbar/alt-tab
    // shows, including windows on other virtual desktops.
    bool IsManageableWindow(HWND hwnd);

    // Enumerate manageable top-level windows in the current topology and capture them.
    std::vector<WindowSnap> CaptureWindows(const Topology& topo, const KeeperConfig& cfg);

    // Enumerate live manageable windows (for restore-time matching).
    std::vector<LiveWindow> EnumerateLiveWindows(const KeeperConfig& cfg);

    // Fill each live window's current monitor + normalized position from the topology.
    void FillLiveGeometry(std::vector<LiveWindow>& live, const Topology& topo);

    // Apply a captured snapshot to a live window, placing it on the resolved monitor.
    // Returns false on UIPI denial or invalid target.
    bool ApplyPlacement(HWND hwnd, const WindowSnap& snap, const Topology& topo);

    // Read the current placement of a window to verify a restore succeeded.
    // Fills outScreenRect with the window's current bounding rect (screen coords).
    bool GetWindowScreenRect(HWND hwnd, RECT& outScreenRect);
}
