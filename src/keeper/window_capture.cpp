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
#include <shobjidl.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uuid.lib")

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

        DWORD CloakedValue(HWND hwnd)
        {
            DWORD cloaked = 0;
            if (FAILED(DwmGetWindowAttribute(hwnd, kDwmwaCloaked, &cloaked, sizeof(cloaked))))
            {
                cloaked = 0;
            }
            return cloaked;
        }

        // Windows on another virtual desktop are shell-cloaked (DWM_CLOAKED_SHELL) and
        // would otherwise be dropped. Suspended UWP frames are shell-cloaked too, but
        // they report no desktop id and claim to be on the current desktop.
        bool IsOnOtherVirtualDesktop(HWND hwnd, IVirtualDesktopManager* vdm)
        {
            bool result = false;
            do
            {
                if (vdm == nullptr)
                {
                    break;
                }
                BOOL onCurrent = TRUE;
                if (FAILED(vdm->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent)) || onCurrent != FALSE)
                {
                    break;
                }
                GUID desk = GUID_NULL;
                if (FAILED(vdm->GetWindowDesktopId(hwnd, &desk)) || IsEqualGUID(desk, GUID_NULL))
                {
                    break;
                }
                result = true;
            } while (false);
            return result;
        }

        // Taskbar/alt-tab visibility test. WS_EX_APPWINDOW forces a window onto the
        // taskbar even when it is a tool, no-activate or owned window.
        bool IsManageableWindowImpl(HWND hwnd, IVirtualDesktopManager* vdm)
        {
            bool result = false;
            do
            {
                if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow())
                {
                    break;
                }
                if (IsWindowVisible(hwnd) == 0)
                {
                    break;
                }
                LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
                if (!appWindow && (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0)
                {
                    break;
                }
                DWORD cloaked = CloakedValue(hwnd);
                // Owned windows that followed their owner to another desktop report
                // DWM_CLOAKED_INHERITED, so only app-cloaking is a hard reject.
                if (cloaked != 0 &&
                    ((cloaked & DWM_CLOAKED_APP) != 0 || !IsOnOtherVirtualDesktop(hwnd, vdm)))
                {
                    break;
                }
                if (IsIconic(hwnd) == 0)
                {
                    RECT r{};
                    if (GetWindowRect(hwnd, &r) == 0 || IsRectEmpty(&r))
                    {
                        break; // zero-size helper/overlay
                    }
                }
                if (appWindow)
                {
                    result = true;
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

        // Per-call COM scope + virtual desktop manager. Enumeration runs on the UI
        // thread and on the debounce worker, so COM may or may not be initialized.
        class VirtualDesktopScope
        {
        public:
            VirtualDesktopScope()
            {
                HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                uninit_ = SUCCEEDED(hr); // RPC_E_CHANGED_MODE: already STA, still usable
                if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                                            IID_PPV_ARGS(&vdm_))))
                {
                    vdm_ = nullptr;
                }
            }
            ~VirtualDesktopScope()
            {
                if (vdm_ != nullptr)
                {
                    vdm_->Release();
                }
                if (uninit_)
                {
                    CoUninitialize();
                }
            }
            VirtualDesktopScope(const VirtualDesktopScope&) = delete;
            VirtualDesktopScope& operator=(const VirtualDesktopScope&) = delete;
            IVirtualDesktopManager* Get() const
            {
                return vdm_;
            }

        private:
            IVirtualDesktopManager* vdm_ = nullptr;
            bool uninit_ = false;
        };

        struct EnumCtx
        {
            const KeeperConfig* cfg;
            IVirtualDesktopManager* vdm;
            std::vector<HWND> hwnds;
        };

        BOOL CALLBACK CollectProc(HWND hwnd, LPARAM lparam)
        {
            auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
            if (IsManageableWindowImpl(hwnd, ctx->vdm))
            {
                ctx->hwnds.push_back(hwnd);
            }
            return TRUE;
        }

        // WINDOWPLACEMENT is in workspace coordinates, except for tool windows which
        // use screen coordinates.
        POINT WorkspaceOffset(HWND hwnd)
        {
            POINT off{0, 0};
            if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0)
            {
                RECT wa = PrimaryWorkArea();
                off.x = wa.left;
                off.y = wa.top;
            }
            return off;
        }

        // Snapped (arranged) windows keep their pre-snap rect in rcNormalPosition.
        bool IsArranged(HWND hwnd)
        {
            using IsWindowArrangedFn = BOOL(WINAPI*)(HWND);
            static IsWindowArrangedFn fn = reinterpret_cast<IsWindowArrangedFn>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowArranged"));
            return fn != nullptr && fn(hwnd) != FALSE;
        }

        // Convert a window's restore (normal) geometry to a screen-space RECT, robust
        // to current minimize/maximize state. A normal-state window reports its real
        // on-screen rect so snapped windows keep their snapped geometry.
        bool NormalScreenRect(HWND hwnd, int& showCmd, RECT& outScreen)
        {
            WINDOWPLACEMENT wp;
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd, &wp) == 0)
            {
                return false;
            }
            showCmd = static_cast<int>(wp.showCmd);
            if (IsIconic(hwnd) == 0 && IsZoomed(hwnd) == 0 && IsArranged(hwnd) &&
                GetWindowRect(hwnd, &outScreen) != 0)
            {
                showCmd = SW_SHOWNORMAL;
                return true;
            }
            POINT off = WorkspaceOffset(hwnd);
            outScreen = wp.rcNormalPosition;
            OffsetRect(&outScreen, off.x, off.y); // workspace -> screen
            return true;
        }
    }

    bool IsManageableWindow(HWND hwnd)
    {
        VirtualDesktopScope vds;
        return IsManageableWindowImpl(hwnd, vds.Get());
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
        // lround, not +0.5 truncation: offsets go negative for snapped/maximized
        // windows whose invisible resize border hangs past the work area.
        r.left = work.left + static_cast<LONG>(std::lround(n.x * waW));
        r.top = work.top + static_cast<LONG>(std::lround(n.y * waH));
        r.right = r.left + static_cast<LONG>(std::lround(n.w * waW));
        r.bottom = r.top + static_cast<LONG>(std::lround(n.h * waH));
        return r;
    }

    std::vector<LiveWindow> EnumerateLiveWindows(const KeeperConfig& cfg)
    {
        VirtualDesktopScope vds;
        EnumCtx ctx;
        ctx.cfg = &cfg;
        ctx.vdm = vds.Get();
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

    namespace
    {
        // Owning monitor + normalized restore geometry of a live window.
        bool ComputeGeometry(HWND hwnd, const Topology& topo, int& showCmd, std::string& monId, NormRect& n)
        {
            RECT normalScreen{};
            if (!NormalScreenRect(hwnd, showCmd, normalScreen))
            {
                return false;
            }

            // Owning monitor for the normal rect (handles off-screen min/max correctly).
            HMONITOR hm = MonitorFromRect(&normalScreen, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hm, &mi) == 0)
            {
                return false;
            }
            if ((mi.rcWork.right - mi.rcWork.left) <= 0 || (mi.rcWork.bottom - mi.rcWork.top) <= 0)
            {
                return false;
            }
            const MonitorInfo* m = topo.FindByGdiName(Utf8FromWide(mi.szDevice));
            monId = (m != nullptr) ? m->id : std::string();
            n = NormalizeToWork(normalScreen, mi.rcWork);
            return true;
        }
    }

    void FillLiveGeometry(std::vector<LiveWindow>& live, const Topology& topo)
    {
        for (auto& lw : live)
        {
            int showCmd = SW_SHOWNORMAL;
            NormRect n;
            lw.hasGeometry = ComputeGeometry(lw.hwnd, topo, showCmd, lw.monitorId, n);
            lw.nx = n.x;
            lw.ny = n.y;
        }
    }

    std::vector<WindowSnap> CaptureWindows(const Topology& topo, const KeeperConfig& cfg)
    {
        std::vector<WindowSnap> out;
        std::vector<LiveWindow> live = EnumerateLiveWindows(cfg);

        for (const auto& lw : live)
        {
            int showCmd = SW_SHOWNORMAL;
            std::string monId;
            NormRect n;
            if (!ComputeGeometry(lw.hwnd, topo, showCmd, monId, n))
            {
                continue;
            }

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
            POINT off = WorkspaceOffset(hwnd);
            RECT normalWs = screen;
            OffsetRect(&normalWs, -off.x, -off.y);

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
            // A snapped window stays arranged at its old spot after SetWindowPlacement;
            // move its live rect explicitly.
            if (wp.showCmd == SW_SHOWNOACTIVATE && IsArranged(hwnd))
            {
                SetWindowPos(hwnd, nullptr, screen.left, screen.top,
                             screen.right - screen.left, screen.bottom - screen.top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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
