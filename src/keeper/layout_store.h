// OhMyDisplay - layouts.json model and persistence (schemaVersion 2).
#pragma once

#include "keeper/window_capture.h"
#include "keeper/monitor_labels.h"
#include "core/display_topology.h"

#include <string>
#include <vector>

namespace omd
{
    struct StoredMonitor
    {
        std::string id;
        std::string label;
        EdidInfo edid;
        std::string devicePath;
        std::string friendlyName;
    };

    struct StoredTopology
    {
        std::string signature;
        std::vector<StoredMonitor> monitors;
        std::string lastSeenUtc;
        std::vector<WindowSnap> windows;
    };

    class LayoutStore
    {
    public:
        static constexpr int kSchemaVersion = 2;

        bool Load();
        bool Save() const;

        MonitorLabels& Labels()
        {
            return labels_;
        }
        const MonitorLabels& Labels() const
        {
            return labels_;
        }

        const std::vector<StoredTopology>& Topologies() const
        {
            return topologies_;
        }

        StoredTopology* FindBySignature(const std::string& signature);
        const StoredTopology* FindBySignature(const std::string& signature) const;

        // Create or replace the snapshot for the given topology with current windows.
        void Upsert(const Topology& topo, const std::vector<WindowSnap>& windows);

        static std::wstring FilePath();

    private:
        MonitorLabels labels_;
        std::vector<StoredTopology> topologies_;
    };

    // Format the current time as ISO-8601 UTC ("2026-06-14T11:02:33Z").
    std::string NowUtcIso();
}
