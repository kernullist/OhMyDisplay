// OhMyDisplay - restore engine: match saved windows to live windows, apply placement,
// then verify and retry windows that did not land on the right monitor.
#pragma once

#include "keeper/layout_store.h"
#include "keeper/window_capture.h"
#include "core/display_topology.h"
#include "core/settings.h"

#include <vector>

namespace omd
{
    // Score a live window as a candidate for a saved snapshot. Negative means the
    // executable differs (ineligible). Higher is a better match. Exposed for tests.
    int WindowMatchScore(const WindowSnap& snap, const LiveWindow& live);

    // Global greedy assignment: pair each saved window with at most one live window,
    // highest score first. Returns live-index per snap, or -1 if unmatched.
    // Tie-break distance between a saved window and a live candidate: normalized
    // position delta, plus a large penalty for a different monitor.
    double WindowGeometryDistance(const WindowSnap& snap, const LiveWindow& live);

    std::vector<int> AssignRestoreMatches(const std::vector<WindowSnap>& snaps,
                                          const std::vector<LiveWindow>& live);

    // True when a known topology's windows have fled to the wrong monitors (typical
    // collapse). A single window the user dragged is not treated as a collapse, so a
    // stray WM_DISPLAYCHANGE does not yank the whole desktop back.
    bool LayoutLooksCollapsed(const std::vector<WindowSnap>& saved,
                              const std::vector<WindowSnap>& current);

    struct RestoreResult
    {
        int total = 0;
        int restored = 0;
        int skipped = 0;
        int retried = 0;
    };

    class RestoreEngine
    {
    public:
        explicit RestoreEngine(const KeeperConfig& cfg) : cfg_(cfg) {}

        // Restore the snapshot onto the current topology. May sleep briefly between
        // verification passes; intended to run on the keeper worker thread.
        RestoreResult Restore(const StoredTopology& snap, const Topology& topo);

    private:
        const KeeperConfig& cfg_;
    };
}
