// OhMyDisplay - monitor labels and identity resolution implementation.
#include "keeper/monitor_labels.h"
#include "core/common.h"
#include "core/logger.h"

#include <map>
#include <vector>
#include <algorithm>

namespace omd
{
    void MonitorLabels::SetLabel(const std::string& identity, const std::string& label)
    {
        if (label.empty())
        {
            labels_.erase(identity);
        }
        else
        {
            labels_[identity] = label;
        }
    }

    std::string MonitorLabels::GetLabel(const std::string& identity) const
    {
        auto it = labels_.find(identity);
        return it != labels_.end() ? it->second : std::string();
    }

    void MonitorLabels::Remove(const std::string& identity)
    {
        labels_.erase(identity);
    }

    void MonitorLabels::LoadFrom(const json::Value& labelsObj)
    {
        labels_.clear();
        if (!labelsObj.IsObject())
        {
            return;
        }
        for (const auto& m : labelsObj.Members())
        {
            if (m.second.IsString())
            {
                labels_[m.first] = m.second.AsString();
            }
        }
    }

    json::Value MonitorLabels::ToJson() const
    {
        json::Value obj = json::Value::MakeObject();
        for (const auto& kv : labels_)
        {
            obj.Set(kv.first, json::Value(kv.second));
        }
        return obj;
    }

    bool ResolveIdentities(Topology& topo, const MonitorLabels& labels)
    {
        bool collision = false;

        // Group monitors by base identity to find same-model collisions.
        std::map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < topo.monitors.size(); ++i)
        {
            groups[topo.monitors[i].baseId].push_back(i);
        }

        std::vector<std::string> finalIds;
        finalIds.reserve(topo.monitors.size());

        for (auto& g : groups)
        {
            if (g.second.size() > 1)
            {
                collision = true;
                OMD_LOGW("labels: same-model collision baseId=%s count=%zu",
                         g.first.c_str(), g.second.size());
                // Append a deterministic connector discriminator to each member.
                for (size_t idx : g.second)
                {
                    MonitorInfo& m = topo.monitors[idx];
                    char disc[16];
                    std::snprintf(disc, sizeof(disc), "_%08x", m.connectorKey);
                    m.id = m.baseId + disc;
                }
            }
            else
            {
                MonitorInfo& m = topo.monitors[g.second.front()];
                m.id = m.baseId;
            }
        }

        // Assign labels and collect final ids.
        for (auto& m : topo.monitors)
        {
            std::string lbl = labels.GetLabel(m.id);
            if (lbl.empty())
            {
                lbl = labels.GetLabel(m.baseId);
            }
            m.label = lbl;
            finalIds.push_back(m.id);
        }

        std::sort(topo.monitors.begin(), topo.monitors.end(),
                  [](const MonitorInfo& a, const MonitorInfo& b)
                  {
                      return a.id < b.id;
                  });
        topo.signature = ComputeSignature(finalIds);
        return collision;
    }

    bool HasUnlabeledCollision(const Topology& topo)
    {
        bool needs = false;
        do
        {
            std::map<std::string, int> counts;
            for (const auto& m : topo.monitors)
            {
                counts[m.baseId]++;
            }
            for (const auto& m : topo.monitors)
            {
                if (counts[m.baseId] > 1 && m.label.empty())
                {
                    needs = true;
                    break;
                }
            }
        } while (false);
        return needs;
    }
}
