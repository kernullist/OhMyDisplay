// OhMyDisplay - monitor labels and identity collision resolution.
//
// Identity is normally unique because the base id already folds in the device path.
// When two panels still collide (identical model + path material), we append a
// deterministic connector-based discriminator so ids stay distinct, and report the
// collision so the shell can prompt the user to label them (FR-A7).
#pragma once

#include "core/display_topology.h"
#include "core/json.h"

#include <string>
#include <map>

namespace omd
{
    class MonitorLabels
    {
    public:
        void SetLabel(const std::string& identity, const std::string& label);
        std::string GetLabel(const std::string& identity) const;
        void Remove(const std::string& identity);
        const std::map<std::string, std::string>& All() const
        {
            return labels_;
        }

        void LoadFrom(const json::Value& labelsObj);
        json::Value ToJson() const;

    private:
        std::map<std::string, std::string> labels_;
    };

    // Finalize monitor identities for matching: disambiguate collisions, assign labels,
    // and recompute the topology signature. Returns true if a same-model collision was
    // detected (the shell should offer the label UI).
    bool ResolveIdentities(Topology& topo, const MonitorLabels& labels);

    // True when at least one same-model group still has an empty user label.
    // Used so we do not pop the label editor on every topology burst.
    bool HasUnlabeledCollision(const Topology& topo);
}
