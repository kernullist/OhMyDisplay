// OhMyDisplay - restore engine implementation.
#include "keeper/restore_engine.h"
#include "core/common.h"
#include "core/logger.h"

#include <vector>
#include <algorithm>
#include <cmath>

namespace omd
{
    int WindowMatchScore(const WindowSnap& snap, const LiveWindow& live)
    {
        if (!IEquals(snap.exeLeaf, live.exeLeaf))
        {
            return -1;
        }
        int score = 1; // same exe baseline
        // Same HWND in this session (survives monitor sleep) is the strongest signal.
        // Also require pid: HWND values are recycled after the original window dies.
        if (snap.hwnd != nullptr && snap.hwnd == live.hwnd &&
            snap.pid != 0 && snap.pid == live.pid)
        {
            score += 1000;
        }
        if (!snap.className.empty() && snap.className == live.className)
        {
            score += 50;
        }
        if (!snap.title.empty() && snap.title == live.title)
        {
            score += 100;
        }
        else if (!snap.title.empty() && !live.title.empty())
        {
            if (live.title.find(snap.title) != std::string::npos ||
                snap.title.find(live.title) != std::string::npos)
            {
                score += 20;
            }
        }
        return score;
    }

    double WindowGeometryDistance(const WindowSnap& snap, const LiveWindow& live)
    {
        // Unknown geometry sorts after any known one but keeps candidates eligible.
        const double kUnknown = 1.0e6;
        const double kOtherMonitor = 1.0e3;
        if (!live.hasGeometry)
        {
            return kUnknown;
        }
        double dx = snap.nx - live.nx;
        double dy = snap.ny - live.ny;
        double d = std::sqrt(dx * dx + dy * dy);
        if (snap.monitorId != live.monitorId)
        {
            d += kOtherMonitor;
        }
        return d;
    }

    std::vector<int> AssignRestoreMatches(const std::vector<WindowSnap>& snaps,
                                          const std::vector<LiveWindow>& live)
    {
        struct Pair
        {
            int snapIndex;
            int liveIndex;
            int score;
            double dist;
        };
        std::vector<Pair> pairs;
        for (size_t si = 0; si < snaps.size(); ++si)
        {
            for (size_t li = 0; li < live.size(); ++li)
            {
                int s = WindowMatchScore(snaps[si], live[li]);
                if (s > 0)
                {
                    pairs.push_back(Pair{static_cast<int>(si), static_cast<int>(li), s,
                                         WindowGeometryDistance(snaps[si], live[li])});
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](const Pair& a, const Pair& b)
                  {
                      if (a.score != b.score)
                      {
                          return a.score > b.score;
                      }
                      // Equal evidence (same exe/class/title): pair the closest windows
                      // first, then fall back to capture order vs enumeration (z) order.
                      if (a.dist != b.dist)
                      {
                          return a.dist < b.dist;
                      }
                      if (a.snapIndex != b.snapIndex)
                      {
                          return a.snapIndex < b.snapIndex;
                      }
                      return a.liveIndex < b.liveIndex;
                  });

        std::vector<int> assigned(snaps.size(), -1);
        std::vector<char> liveUsed(live.size(), 0);
        for (const auto& p : pairs)
        {
            if (assigned[static_cast<size_t>(p.snapIndex)] >= 0)
            {
                continue;
            }
            if (liveUsed[static_cast<size_t>(p.liveIndex)] != 0)
            {
                continue;
            }
            assigned[static_cast<size_t>(p.snapIndex)] = p.liveIndex;
            liveUsed[static_cast<size_t>(p.liveIndex)] = 1;
        }
        return assigned;
    }

    namespace
    {
        // A capture viewed as live candidates, so captures can be matched to snapshots.
        std::vector<LiveWindow> CaptureAsLive(const std::vector<WindowSnap>& current)
        {
            std::vector<LiveWindow> live;
            live.reserve(current.size());
            for (const auto& c : current)
            {
                LiveWindow lw;
                lw.hwnd = c.hwnd;
                lw.pid = c.pid;
                lw.exeLeaf = c.exeLeaf;
                lw.title = c.title;
                lw.className = c.className;
                lw.hasGeometry = true;
                lw.monitorId = c.monitorId;
                lw.nx = c.nx;
                lw.ny = c.ny;
                live.push_back(std::move(lw));
            }
            return live;
        }
    }

    int CountMisplacedWindows(const std::vector<WindowSnap>& saved,
                              const std::vector<WindowSnap>& current, int& outMatched)
    {
        outMatched = 0;
        int wrong = 0;
        std::vector<int> match = AssignRestoreMatches(saved, CaptureAsLive(current));
        for (size_t i = 0; i < saved.size(); ++i)
        {
            int li = match[i];
            if (li < 0)
            {
                continue;
            }
            ++outMatched;
            const std::string& want = saved[i].monitorId;
            const std::string& got = current[static_cast<size_t>(li)].monitorId;
            if (!want.empty() && !got.empty() && want != got)
            {
                ++wrong;
            }
        }
        return wrong;
    }

    bool LayoutLooksCollapsed(const std::vector<WindowSnap>& saved,
                              const std::vector<WindowSnap>& current)
    {
        int matched = 0;
        int wrong = CountMisplacedWindows(saved, current, matched);
        // At least half the matched windows fled to a different monitor.
        return matched > 0 && wrong * 2 >= matched;
    }

    std::vector<WindowSnap> UnmatchedWindows(const std::vector<WindowSnap>& saved,
                                             const std::vector<WindowSnap>& current)
    {
        std::vector<int> match = AssignRestoreMatches(saved, CaptureAsLive(current));
        std::vector<char> taken(current.size(), 0);
        for (int li : match)
        {
            if (li >= 0)
            {
                taken[static_cast<size_t>(li)] = 1;
            }
        }
        std::vector<WindowSnap> out;
        for (size_t i = 0; i < current.size(); ++i)
        {
            if (taken[i] == 0)
            {
                out.push_back(current[i]);
            }
        }
        return out;
    }

    namespace
    {
        // Resolve the GDI device name of the monitor a window currently sits on.
        std::string CurrentMonitorGdi(HWND hwnd)
        {
            HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi;
            mi.cbSize = sizeof(mi);
            if (hm != nullptr && GetMonitorInfoW(hm, &mi) != 0)
            {
                return Utf8FromWide(mi.szDevice);
            }
            return std::string();
        }

        // Did the window land on the target monitor? Minimized windows are accepted
        // as-is (their on-screen rect is meaningless until restored by the user).
        bool VerifyPlacement(HWND hwnd, const WindowSnap& snap, const Topology& topo)
        {
            if (snap.showCmd == SW_SHOWMINNOACTIVE || snap.showCmd == SW_SHOWMINIMIZED ||
                snap.showCmd == SW_MINIMIZE)
            {
                return true;
            }
            const MonitorInfo* m = topo.FindById(snap.monitorId);
            if (m == nullptr)
            {
                return true; // unknown target; nothing better to verify against
            }
            return CurrentMonitorGdi(hwnd) == m->gdiName;
        }

        // Enough state to tell "app moved itself back" from "wrong target" next time.
        void LogGiveUp(HWND hwnd, const WindowSnap& snap, const Topology& topo)
        {
            const MonitorInfo* m = topo.FindById(snap.monitorId);
            RECT r{};
            GetWindowRect(hwnd, &r);
            LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            OMD_LOGW("restore: gave up on %s '%s' want=%s got=%s rect=(%ld,%ld,%ld,%ld) "
                     "iconic=%d zoomed=%d style=%08llx",
                     snap.exeLeaf.c_str(), snap.title.c_str(),
                     (m != nullptr) ? m->gdiName.c_str() : "?", CurrentMonitorGdi(hwnd).c_str(),
                     r.left, r.top, r.right, r.bottom, IsIconic(hwnd) != 0 ? 1 : 0,
                     IsZoomed(hwnd) != 0 ? 1 : 0, static_cast<unsigned long long>(style));
        }
    }

    RestoreResult RestoreEngine::Restore(const StoredTopology& snap, const Topology& topo)
    {
        RestoreResult res;
        res.total = static_cast<int>(snap.windows.size());

        std::vector<LiveWindow> live = EnumerateLiveWindows(cfg_);
        FillLiveGeometry(live, topo);

        struct Pending
        {
            const WindowSnap* snap;
            HWND hwnd;
        };
        std::vector<Pending> pending;

        // Highest-score unique assignment so a weak first snap cannot steal a better match.
        std::vector<int> assigned = AssignRestoreMatches(snap.windows, live);
        for (size_t i = 0; i < snap.windows.size(); ++i)
        {
            int idx = assigned[i];
            if (idx < 0)
            {
                res.skipped++;
                OMD_LOGD("restore: no live match for %s '%s'",
                         snap.windows[i].exeLeaf.c_str(), snap.windows[i].title.c_str());
                continue;
            }
            HWND hwnd = live[static_cast<size_t>(idx)].hwnd;
            if (ApplyPlacement(hwnd, snap.windows[i], topo))
            {
                pending.push_back(Pending{&snap.windows[i], hwnd});
            }
            else
            {
                res.skipped++;
            }
        }

        // One shared settle wait, then retry only the windows that did not land.
        int retries = cfg_.restoreRetries;
        if (retries < 0)
        {
            retries = 0;
        }
        if (!pending.empty())
        {
            Sleep(200);
        }
        for (int attempt = 0; attempt <= retries; ++attempt)
        {
            std::vector<Pending> failed;
            for (const auto& p : pending)
            {
                if (VerifyPlacement(p.hwnd, *p.snap, topo))
                {
                    res.restored++;
                }
                else
                {
                    failed.push_back(p);
                }
            }
            if (failed.empty())
            {
                break;
            }
            if (attempt == retries)
            {
                res.skipped += static_cast<int>(failed.size());
                for (const auto& p : failed)
                {
                    LogGiveUp(p.hwnd, *p.snap, topo);
                }
                break;
            }
            res.retried += static_cast<int>(failed.size());
            for (const auto& p : failed)
            {
                ApplyPlacement(p.hwnd, *p.snap, topo);
            }
            pending = std::move(failed);
            Sleep(200);
        }

        OMD_LOGI("restore: total=%d restored=%d skipped=%d retried=%d",
                 res.total, res.restored, res.skipped, res.retried);
        return res;
    }
}
