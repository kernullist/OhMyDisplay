// OhMyDisplay - Layout Keeper orchestrator. Wires topology query, identity resolution,
// capture, persistence, adaptive debounce and restore into one facade for the shell.
#pragma once

#include "keeper/layout_store.h"
#include "keeper/debounce.h"
#include "keeper/ghost_monitor.h"
#include "core/display_topology.h"
#include "core/settings.h"

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

        Settings& settings_;
        LayoutStore store_;
        std::unique_ptr<Debouncer> debouncer_;
        std::mutex mutex_;
    };
}
