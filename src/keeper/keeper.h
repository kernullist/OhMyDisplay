// OhMyDisplay - Layout Keeper orchestrator. Wires topology query, identity resolution,
// capture, persistence, adaptive debounce and restore into one facade for the shell.
#pragma once

#include "keeper/layout_store.h"
#include "keeper/debounce.h"
#include "keeper/ghost_monitor.h"
#include "core/display_topology.h"
#include "core/settings.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace omd
{
    class Keeper
    {
    public:
        using NotifyFn = std::function<void(const std::string&)>;

        explicit Keeper(Settings& settings);
        ~Keeper();

        bool Init();
        void Shutdown();

        // From the UI thread on DisplayChanged / PowerResume: starts adaptive debounce.
        void OnTopologyEvent();

        // From the UI thread on DisplayStateOff / PowerSuspend. Returns true on the
        // first off signal of a cycle (both power GUIDs fire); the caller then runs
        // CapturePreSleep off the UI thread.
        bool OnDisplayOff();
        // From the UI thread on DisplayStateOn / PowerResume.
        void OnDisplayOn();

        // Replace the current topology's snapshot with the layout the user had right
        // before the displays went dark. Worker thread.
        void CapturePreSleep();

        // Hotkey / menu actions.
        void SaveNow();
        void RestoreNow();

        // Query the current topology with identities resolved (for the label UI).
        Topology CurrentResolvedTopology();
        std::vector<GhostMonitor> Ghosts();

        // Persist a user label and re-evaluate.
        void SetMonitorLabel(const std::string& identity, const std::string& label);

        LayoutStore& Store()
        {
            return store_;
        }

        // Shell hooks.
        NotifyFn onNotify;     // informational balloon text
        NotifyFn onCollision;  // same-model collision -> shell opens the label UI

    private:
        void EvaluateAndRestore(); // debounce callback (worker thread)
        int DebounceMonitorCount() const;

        Settings& settings_;
        LayoutStore store_;
        std::unique_ptr<Debouncer> debouncer_;
        std::mutex mutex_;
        // Monitor count of the last settled topology. During wake the live count dips
        // (3 -> 1 -> 3); sizing the debounce on the dip fires before the rest return.
        std::atomic<int> lastMonitorCount_{0};
        int incompleteRetries_ = 0; // worker thread only
        std::atomic<bool> displayOff_{false};
        // Topology whose snapshot was taken at the last display-off and not yet
        // evaluated after wake. Guarded by mutex_.
        std::string preSleepSignature_;
    };
}
