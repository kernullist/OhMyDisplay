// OhMyDisplay - Layout Keeper orchestrator implementation.
#include "keeper/keeper.h"
#include "keeper/window_capture.h"
#include "keeper/monitor_labels.h"
#include "keeper/restore_engine.h"
#include "core/logger.h"

#include <windows.h>
#include <algorithm>

namespace omd
{
    Keeper::Keeper(Settings& settings) : settings_(settings)
    {
    }

    Keeper::~Keeper()
    {
        Shutdown();
    }

    bool Keeper::Init()
    {
        store_.Load();
        debouncer_ = std::make_unique<Debouncer>([this]
        {
            EvaluateAndRestore();
        });
        debouncer_->SetConfig(settings_.keeper.debounce);
        debouncer_->Start();
        OMD_LOGI("keeper: init (enabled=%d, topologies=%zu)",
                 settings_.keeper.enabled ? 1 : 0, store_.Topologies().size());
        return true;
    }

    void Keeper::Shutdown()
    {
        if (debouncer_)
        {
            debouncer_->Stop();
            debouncer_.reset();
        }
    }

    void Keeper::OnTopologyEvent()
    {
        if (!settings_.keeper.enabled || !debouncer_)
        {
            return;
        }
        debouncer_->SetConfig(settings_.keeper.debounce);
        debouncer_->Trigger(DebounceMonitorCount());
    }

    int Keeper::DebounceMonitorCount() const
    {
        // Active monitor count drives the adaptive window; cheap to read here.
        int count = std::max(GetSystemMetrics(SM_CMONITORS), lastMonitorCount_.load());
        return std::max(count, 1);
    }

    Topology Keeper::CurrentResolvedTopology()
    {
        Topology topo;
        if (QueryActiveTopology(topo))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ResolveIdentities(topo, store_.Labels());
        }
        return topo;
    }

    std::vector<GhostMonitor> Keeper::Ghosts()
    {
        return DetectGhostMonitors();
    }

    void Keeper::SetMonitorLabel(const std::string& identity, const std::string& label)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            store_.Labels().SetLabel(identity, label);
            store_.Save();
        }
        OMD_LOGI("keeper: label set id=%s label=%s", identity.c_str(), label.c_str());
    }

    void Keeper::SaveNow()
    {
        Topology topo;
        if (!QueryActiveTopology(topo))
        {
            OMD_LOGE("keeper: SaveNow topology query failed");
            return;
        }
        std::vector<WindowSnap> wins;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ResolveIdentities(topo, store_.Labels());
            wins = CaptureWindows(topo, settings_.keeper);
            store_.Upsert(topo, wins);
            store_.Save();
        }
        if (onNotify)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Saved %zu windows for %d-monitor layout",
                          wins.size(), topo.Count());
            onNotify(buf);
        }
    }

    void Keeper::RestoreNow()
    {
        Topology topo;
        if (!QueryActiveTopology(topo))
        {
            OMD_LOGE("keeper: RestoreNow topology query failed");
            return;
        }
        StoredTopology snapCopy;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ResolveIdentities(topo, store_.Labels());
            const StoredTopology* st = store_.FindBySignature(topo.signature);
            if (st != nullptr)
            {
                snapCopy = *st; // copy so we can restore without holding the lock
                found = true;
            }
        }
        if (!found)
        {
            if (onNotify)
            {
                onNotify("No saved layout for the current displays yet");
            }
            return;
        }
        RestoreEngine engine(settings_.keeper);
        RestoreResult res = engine.Restore(snapCopy, topo);
        if (onNotify)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Restored %d/%d windows (%d retried, %d skipped)",
                          res.restored, res.total, res.retried, res.skipped);
            onNotify(buf);
        }
    }

    void Keeper::EvaluateAndRestore()
    {
        if (!settings_.keeper.enabled)
        {
            return;
        }
        Topology topo;
        if (!QueryActiveTopology(topo))
        {
            return;
        }
        if (!topo.complete)
        {
            // Saving now would persist a bogus signature with the collapsed layout, so
            // wait for Windows to finish enumerating. A display that never reports a
            // device path (some VMs/virtual displays) falls through after the retries.
            const int kMaxIncompleteRetries = 5;
            if (incompleteRetries_ < kMaxIncompleteRetries && debouncer_)
            {
                ++incompleteRetries_;
                OMD_LOGW("keeper: topology incomplete (%d monitors), retry %d",
                         topo.Count(), incompleteRetries_);
                debouncer_->Trigger(DebounceMonitorCount());
                return;
            }
            OMD_LOGW("keeper: topology still incomplete after retries, using it as-is");
        }
        incompleteRetries_ = 0;
        lastMonitorCount_.store(topo.Count());

        bool collision = false;
        StoredTopology snapCopy;
        bool found = false;
        bool autoSaved = false;
        size_t savedCount = 0;
        std::vector<WindowSnap> current;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            collision = ResolveIdentities(topo, store_.Labels());
            current = CaptureWindows(topo, settings_.keeper);
            const StoredTopology* st = store_.FindBySignature(topo.signature);
            if (st != nullptr)
            {
                snapCopy = *st;
                found = true;
            }
            else if (!current.empty())
            {
                // Unknown topology: capture and store so future collapses can restore.
                savedCount = current.size();
                store_.Upsert(topo, current);
                store_.Save();
                autoSaved = true;
            }
        }

        if (collision && HasUnlabeledCollision(topo) && onCollision)
        {
            onCollision("Two identical monitors detected. Please label them to tell them apart.");
        }

        if (autoSaved)
        {
            OMD_LOGI("keeper: new topology auto-saved (%zu windows)", savedCount);
            if (onNotify)
            {
                onNotify("New display layout detected and saved");
            }
            return;
        }

        if (!found)
        {
            return;
        }

        if (LayoutLooksCollapsed(snapCopy.windows, current))
        {
            RestoreEngine engine(settings_.keeper);
            RestoreResult res = engine.Restore(snapCopy, topo);
            if (onNotify && (res.restored > 0 || res.retried > 0))
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "Layout restored: %d/%d windows",
                              res.restored, res.total);
                onNotify(buf);
            }
            return;
        }

        // Known topology, not collapsed: leave the last good snapshot alone.
        // Refreshing here would persist game-fullscreen / DPI transients.
        OMD_LOGI("keeper: topology stable, snapshot kept (%zu live windows)", current.size());
    }
}
