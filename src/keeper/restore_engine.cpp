// OhMyDisplay - restore engine implementation.
#include "keeper/restore_engine.h"
#include "core/common.h"
#include "core/logger.h"

#include <vector>
#include <algorithm>

namespace omd
{
    int WindowMatchScore(const WindowSnap& snap, const LiveWindow& live)
    {
        if (!IEquals(snap.exeLeaf, live.exeLeaf))
        {
            return -1;
        }
        int score = 1; // same exe baseline
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
    }

    RestoreResult RestoreEngine::Restore(const StoredTopology& snap, const Topology& topo)
    {
        RestoreResult res;
        res.total = static_cast<int>(snap.windows.size());

        std::vector<LiveWindow> live = EnumerateLiveWindows(cfg_);

        // First pass: greedy best-match assignment.
        struct Pending
        {
            const WindowSnap* snap;
            HWND hwnd;
        };
        std::vector<Pending> pending;

        for (const auto& w : snap.windows)
        {
            int bestScore = 0;
            int bestIdx = -1;
            for (size_t i = 0; i < live.size(); ++i)
            {
                if (live[i].used)
                {
                    continue;
                }
                int s = WindowMatchScore(w, live[i]);
                if (s > bestScore)
                {
                    bestScore = s;
                    bestIdx = static_cast<int>(i);
                }
            }
            if (bestIdx < 0)
            {
                res.skipped++;
                OMD_LOGD("restore: no live match for %s '%s'", w.exeLeaf.c_str(), w.title.c_str());
                continue;
            }
            live[static_cast<size_t>(bestIdx)].used = true;
            if (ApplyPlacement(live[static_cast<size_t>(bestIdx)].hwnd, w, topo))
            {
                pending.push_back(Pending{&w, live[static_cast<size_t>(bestIdx)].hwnd});
            }
            else
            {
                res.skipped++;
            }
        }

        // Verification + retry passes for windows that fought back.
        int retries = cfg_.restoreRetries;
        for (auto& p : pending)
        {
            bool good = false;
            for (int attempt = 0; attempt <= retries; ++attempt)
            {
                Sleep(200);
                if (VerifyPlacement(p.hwnd, *p.snap, topo))
                {
                    good = true;
                    break;
                }
                if (attempt < retries)
                {
                    res.retried++;
                    ApplyPlacement(p.hwnd, *p.snap, topo);
                }
            }
            if (good)
            {
                res.restored++;
            }
            else
            {
                res.skipped++;
                OMD_LOGW("restore: gave up on %s '%s'", p.snap->exeLeaf.c_str(), p.snap->title.c_str());
            }
        }

        OMD_LOGI("restore: total=%d restored=%d skipped=%d retried=%d",
                 res.total, res.restored, res.skipped, res.retried);
        return res;
    }
}
