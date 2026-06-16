// OhMyDisplay - window capture/restore implementation.
//
// Coordinate model: WINDOWPLACEMENT.rcNormalPosition is in "workspace" coordinates
// whose origin is the primary monitor work-area top-left. We convert to/from screen
// coordinates with that offset, then normalize against the owning monitor work area.
#include "keeper/window_capture.h"
#include "core/common.h"
#include "core/logger.h"

#include <dwmapi.h>
#include <psapi.h>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

namespace omd
{
    namespace
    {
        const DWORD kDwmwaCloaked = 14; // DWMWA_CLOAKED

        RECT PrimaryWorkArea()
        {
            RECT wa{0, 0, 0, 0};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            return wa;
        }

        bool IsCloaked(HWND hwnd)
        {
            BOOL cloaked = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, kDwmwaCloaked, &cloaked, sizeof(cloaked))))
            {
                return cloaked != FALSE;
            }
            return false;
        }

        // Standard alt-tab visibility test plus tool-window/cloak exclusion.
        bool IsAltTabWindow(HWND hwnd)
        {
            bool result = false;
            do
            {
                if (hwnd == GetShellWindow())
                {
                    break;
                }
                if (IsWindowVisible(hwnd) == 0)
                {
                    break;
                }
                if (IsCloaked(hwnd))
                {
                    break;
                }
                LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                if ((ex & WS_EX_TOOLWINDOW) != 0)
                {
                    break;
                }
                // Walk to the last active visible popup of the root owner.
                HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
                HWND walk = nullptr;
                HWND tryHwnd = root;
                while (tryHwnd != walk)
                {
                    walk = tryHwnd;
                    tryHwnd = GetLastActivePopup(walk);
                    if (IsWindowVisible(tryHwnd) != 0)
                    {
                        break;
                    }
                }
                if (walk != hwnd)
                {
                    break;
                }
                result = true;
            } while (false);
            return result;
        }

        std::string ProcessImagePath(DWORD pid)
        {
            std::string out;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            do
            {
                if (h == nullptr)
                {
                    break;
                }
                wchar_t buf[1024];
                DWORD sz = static_cast<DWORD>(std::size(buf));
                if (QueryFullProcessImageNameW(h, 0, buf, &sz) == 0)
                {
                    break;
                }
                out = Utf8FromWide(std::wstring(buf, sz));
            } while (false);
            if (h != nullptr)
            {
                CloseHandle(h);
            }
            return out;
        }

        std::string LeafLower(const std::string& path)
        {
            size_t pos = path.find_last_of("\\/");
            std::string leaf = (pos == std::string::npos) ? path : path.substr(pos + 1);
            return ToLowerAscii(leaf);
        }

        std::string WindowTitleUtf8(HWND hwnd)
        {
            int len = GetWindowTextLengthW(hwnd);
            if (len <= 0)
            {
                return std::string();
            }
            std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
            int got = GetWindowTextW(hwnd, buf.data(), len + 1);
            buf.resize(static_cast<size_t>(got));
            return Utf8FromWide(buf);
        }

        std::string WindowClassUtf8(HWND hwnd)
        {
            wchar_t buf[256];
            int got = GetClassNameW(hwnd, buf, static_cast<int>(std::size(buf)));
            return Utf8FromWide(std::wstring(buf, static_cast<size_t>(got)));
        }

        bool IsExcluded(const KeeperConfig& cfg, const std::string& exeLeaf, const std::string& title)
        {
            for (const auto& e : cfg.excludeExe)
            {
                if (IEquals(e, exeLeaf))
                {
                    return true;
                }
            }
            for (const auto& sub : cfg.excludeTitleSubstr)
            {
                if (!sub.empty() && title.find(sub) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        struct EnumCtx
        {
            const KeeperConfig* cfg;
            std::vector<HWND> hwnds;
        };

        BOOL CALLBACK CollectProc(HWND hwnd, LPARAM lparam)
        {
            auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
            if (IsAltTabWindow(hwnd))
            {
                ctx->hwnds.push_back(hwnd);
            }
            return TRUE;
        }

        // Convert a window's restore (normal) geometry to a screen-space RECT, robust
        // to current minimize/maximize state.
        bool NormalScreenRect(HWND hwnd, int& showCmd, RECT& outScreen)
        {
            WINDOWPLACEMENT wp;
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd, &wp) == 0)
            {
                return false;
            }
            showCmd = static_cast<int>(wp.showCmd);
            RECT wa = PrimaryWorkArea();
            outScreen = wp.rcNormalPosition;
            OffsetRect(&outScreen, wa.left, wa.top); // workspace -> screen
            return true;
        }
    }

    NormRect NormalizeToWork(const RECT& screen, const RECT& work)
    {
        NormRect n;
        double waW = static_cast<double>(work.right - work.left);
        double waH = static_cast<double>(work.bottom - work.top);
        if (waW > 0.0 && waH > 0.0)
        {
            n.x = (screen.left - work.left) / waW;
            n.y = (screen.top - work.top) / waH;
            n.w = (screen.right - screen.left) / waW;
            n.h = (screen.bottom - screen.top) / waH;
        }
        return n;
    }

    RECT DenormalizeFromWork(const NormRect& n, const RECT& work)
    {
        double waW = static_cast<double>(work.right - work.left);
        double waH = static_cast<double>(work.bottom - work.top);
        RECT r;
        r.left = work.left + static_cast<LONG>(n.x * waW + 0.5);
        r.top = work.top + static_cast<LONG>(n.y * waH + 0.5);
        r.right = r.left + static_cast<LONG>(n.w * waW + 0.5);
        r.bottom = r.top + static_cast<LONG>(n.h * waH + 0.5);
        return r;
    }

    std::vector<LiveWindow> EnumerateLiveWindows(const KeeperConfig& cfg)
    {
        EnumCtx ctx;
        ctx.cfg = &cfg;
        EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&ctx));

        std::vector<LiveWindow> out;
        out.reserve(ctx.hwnds.size());
        for (HWND hwnd : ctx.hwnds)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            std::string exe = ProcessImagePath(pid);
            std::string leaf = LeafLower(exe);
            std::string title = WindowTitleUtf8(hwnd);
            if (IsExcluded(cfg, leaf, title))
            {
                continue;
            }
            LiveWindow lw;
            lw.hwnd = hwnd;
            lw.pid = pid;
            lw.exe = exe;
            lw.exeLeaf = leaf;
            lw.title = title;
            lw.className = WindowClassUtf8(hwnd);
            out.push_back(std::move(lw));
        }
        return out;
    }

    std::vector<WindowSnap> CaptureWindows(const Topology& topo, const KeeperConfig& cfg)
    {
        std::vector<WindowSnap> out;
        std::vector<LiveWindow> live = EnumerateLiveWindows(cfg);

        for (const auto& lw : live)
        {
            int showCmd = SW_SHOWNORMAL;
            RECT normalScreen{};
            if (!NormalScreenRect(lw.hwnd, showCmd, normalScreen))
            {
                continue;
            }

            // Owning monitor for the normal rect (handles off-screen min/max correctly).
            HMONITOR hm = MonitorFromRect(&normalScreen, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hm, &mi) == 0)
            {
                continue;
            }
            std::string gdi = Utf8FromWide(mi.szDevice);
            const MonitorInfo* m = topo.FindByGdiName(gdi);
            std::string monId = (m != nullptr) ? m->id : std::string();

            if ((mi.rcWork.right - mi.rcWork.left) <= 0 || (mi.rcWork.bottom - mi.rcWork.top) <= 0)
            {
                continue;
            }
            NormRect n = NormalizeToWork(normalScreen, mi.rcWork);

            WindowSnap s;
            s.exe = lw.exe;
            s.exeLeaf = lw.exeLeaf;
            s.title = lw.title;
            s.className = lw.className;
            s.monitorId = monId;
            s.showCmd = showCmd;
            s.nx = n.x;
            s.ny = n.y;
            s.nw = n.w;
            s.nh = n.h;
            s.hwnd = lw.hwnd;
            s.pid = lw.pid;
            out.push_back(std::move(s));
        }
        OMD_LOGI("capture: %zu windows for topology %s", out.size(), topo.signature.c_str());
        return out;
    }

    bool ApplyPlacement(HWND hwnd, const WindowSnap& snap, const Topology& topo)
    {
        bool ok = false;
        do
        {
            // Resolve target monitor work area from the saved identity.
            RECT wa{};
            const MonitorInfo* m = topo.FindById(snap.monitorId);
            if (m != nullptr)
            {
                wa = m->rcWork;
            }
            else
            {
                // Fallback: keep the window on the primary work area.
                wa = PrimaryWorkArea();
            }
            if ((wa.right - wa.left) <= 0 || (wa.bottom - wa.top) <= 0)
            {
                break;
            }

            RECT screen = DenormalizeFromWork(NormRect{snap.nx, snap.ny, snap.nw, snap.nh}, wa);

            // Convert screen -> workspace for rcNormalPosition.
            RECT primary = PrimaryWorkArea();
            RECT normalWs = screen;
            OffsetRect(&normalWs, -primary.left, -primary.top);

            WINDOWPLACEMENT wp;
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd, &wp) == 0)
            {
                break;
            }
            wp.rcNormalPosition = normalWs;
            // Preserve minimized/maximized intent; SW_SHOWMINNOACTIVE avoids stealing focus.
            if (snap.showCmd == SW_SHOWMINIMIZED || snap.showCmd == SW_MINIMIZE
                || snap.showCmd == SW_SHOWMINNOACTIVE)
            {
                wp.showCmd = SW_SHOWMINNOACTIVE;
            }
            else if (snap.showCmd == SW_SHOWMAXIMIZED || snap.showCmd == SW_MAXIMIZE)
            {
                wp.showCmd = SW_SHOWMAXIMIZED;
            }
            else
            {
                wp.showCmd = SW_SHOWNOACTIVATE;
            }

            if (SetWindowPlacement(hwnd, &wp) == 0)
            {
                DWORD gle = GetLastError();
                // ERROR_ACCESS_DENIED indicates UIPI (elevated target window).
                OMD_LOGW("restore: SetWindowPlacement failed gle=%lu hwnd=%p", gle, hwnd);
                break;
            }
            ok = true;
        } while (false);
        return ok;
    }

    bool GetWindowScreenRect(HWND hwnd, RECT& outScreenRect)
    {
        return GetWindowRect(hwnd, &outScreenRect) != 0;
    }
}
