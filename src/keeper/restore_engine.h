// OhMyDisplay - restore engine: match saved windows to live windows, apply placement,
// then verify and retry windows that did not land on the right monitor.
#pragma once

#include "keeper/layout_store.h"
#include "keeper/window_capture.h"
#include "core/display_topology.h"
#include "core/settings.h"

namespace omd
{
    // Score a live window as a candidate for a saved snapshot. Negative means the
    // executable differs (ineligible). Higher is a better match. Exposed for tests.
    int WindowMatchScore(const WindowSnap& snap, const LiveWindow& live);

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
