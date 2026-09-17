// OhMyDisplay - embedded report window implementation.
#include "shell/report_window.h"
#include "doctor/report.h"
#include "doctor/doctor.h"
#include "core/paths.h"
#include "core/common.h"
#include "core/logger.h"

#include <windows.h>
#include <objbase.h>
#include <thread>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <chrono>

#ifdef OMD_WITH_WEBVIEW2
// The SDK headers are noisy under /W4; silence them locally.
#pragma warning(push, 0)
#include <wrl.h>
#include <WebView2.h>
#pragma warning(pop)
#endif

namespace omd
{
    namespace
    {
        const wchar_t* kClass = L"OhMyDisplay.ReportWindow";
        const int kActionBarH = 40;
        const int kActionIdBase = 3000;

        std::mutex g_reportMu;
        std::condition_variable g_reportCv;
        std::vector<HWND> g_reportHwnds;
        int g_reportLive = 0;

        void TrackReportThread(int delta)
        {
            std::lock_guard<std::mutex> lock(g_reportMu);
            g_reportLive += delta;
            if (g_reportLive <= 0)
            {
                g_reportLive = 0;
                g_reportCv.notify_all();
            }
        }

        void RegisterReportHwnd(HWND hwnd)
        {
            std::lock_guard<std::mutex> lock(g_reportMu);
            g_reportHwnds.push_back(hwnd);
        }

        void UnregisterReportHwnd(HWND hwnd)
        {
            std::lock_guard<std::mutex> lock(g_reportMu);
            g_reportHwnds.erase(std::remove(g_reportHwnds.begin(), g_reportHwnds.end(), hwnd),
                                g_reportHwnds.end());
        }

        struct ReportCtx
        {
            HWND hwnd = nullptr;
            std::wstring html;
            std::wstring text;
            HWND fallbackEdit = nullptr;
            bool fallbackBuilt = false;
            bool preferWebView2 = true;
            std::atomic<bool> alive{true};
            std::vector<std::string> wakeDevices; // one-click DeviceDisableWake args
#ifdef OMD_WITH_WEBVIEW2
            ICoreWebView2Controller* controller = nullptr;
#endif
        };

        int ContentHeight(HWND hwnd, ReportCtx* ctx)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int bar = ctx->wakeDevices.empty() ? 0 : kActionBarH;
            int h = rc.bottom - bar;
            return h < 0 ? 0 : h;
        }

        void BuildFallback(HWND hwnd, ReportCtx* ctx)
        {
            if (ctx->fallbackBuilt)
            {
                return;
            }
            ctx->fallbackBuilt = true;
            RECT rc;
            GetClientRect(hwnd, &rc);
            ctx->fallbackEdit = CreateWindowExW(0, L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                    ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                0, 0, rc.right, ContentHeight(hwnd, ctx), hwnd,
                nullptr, GetModuleHandleW(nullptr), nullptr);
            if (ctx->fallbackEdit != nullptr)
            {
                HFONT mono = reinterpret_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));
                SendMessageW(ctx->fallbackEdit, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
                SetWindowTextW(ctx->fallbackEdit, ctx->text.c_str());
            }
        }

        void OnResize(HWND hwnd, ReportCtx* ctx)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int contentH = ContentHeight(hwnd, ctx);
#ifdef OMD_WITH_WEBVIEW2
            if (ctx->controller != nullptr)
            {
                RECT bounds{0, 0, rc.right, contentH};
                ctx->controller->put_Bounds(bounds);
            }
#endif
            if (ctx->fallbackEdit != nullptr)
            {
                MoveWindow(ctx->fallbackEdit, 0, 0, rc.right, contentH, TRUE);
            }
            // Reposition action buttons along the bottom.
            int barTop = contentH;
            int x = 8;
            for (size_t i = 0; i < ctx->wakeDevices.size(); ++i)
            {
                HWND btn = GetDlgItem(hwnd, kActionIdBase + static_cast<int>(i));
                if (btn != nullptr)
                {
                    MoveWindow(btn, x, barTop + 6, 280, 28, TRUE);
                    x += 288;
                }
            }
        }

        void ApplyDisableWakeClicked(HWND owner, ReportCtx* ctx, size_t index)
        {
            if (index >= ctx->wakeDevices.size())
            {
                return;
            }
            // Run on this STA thread so Exit can wait for the report loop (UAC +
            // MessageBox) instead of detaching a thread that outlives teardown.
            std::string device = ctx->wakeDevices[index];
            bool ok = ApplyDeviceDisableWake(device);
            std::wstring msg = ok
                ? (L"Wake disabled for:\n" + WideFromUtf8(device))
                : (L"Could not disable wake for:\n" + WideFromUtf8(device) +
                   L"\n\nThe UAC prompt may have been declined.");
            MessageBoxW(owner, msg.c_str(), L"OhMyDisplay",
                        MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
        }

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_NCCREATE)
            {
                auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
            auto* ctx = reinterpret_cast<ReportCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg)
            {
            case WM_COMMAND:
                if (ctx != nullptr && ctx->alive.load())
                {
                    int id = LOWORD(wp);
                    if (id >= kActionIdBase &&
                        id < kActionIdBase + static_cast<int>(ctx->wakeDevices.size()))
                    {
                        ApplyDisableWakeClicked(hwnd, ctx, static_cast<size_t>(id - kActionIdBase));
                        return 0;
                    }
                }
                break;
            case WM_SIZE:
                if (ctx != nullptr && ctx->alive.load())
                {
                    OnResize(hwnd, ctx);
                }
                return 0;
            case WM_DESTROY:
                UnregisterReportHwnd(hwnd);
                if (ctx != nullptr)
                {
                    ctx->alive.store(false);
                    ctx->hwnd = nullptr;
#ifdef OMD_WITH_WEBVIEW2
                    if (ctx->controller != nullptr)
                    {
                        ctx->controller->Close();
                        ctx->controller->Release();
                        ctx->controller = nullptr;
                    }
#endif
                }
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

#ifdef OMD_WITH_WEBVIEW2
        // Returns true if environment creation was kicked off successfully; the async
        // handlers either populate the WebView2 or build the text fallback on failure.
        bool TryStartWebView2(HWND hwnd, std::shared_ptr<ReportCtx> ctx)
        {
            using namespace Microsoft::WRL;
            std::wstring userData = AppDataDir();
            if (!userData.empty())
            {
                userData += L"\\webview2";
            }
            HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
                nullptr, userData.empty() ? nullptr : userData.c_str(), nullptr,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [hwnd, ctx](HRESULT result, ICoreWebView2Environment* env) -> HRESULT
                    {
                        if (!ctx->alive.load() || ctx->hwnd == nullptr)
                        {
                            return S_OK;
                        }
                        if (FAILED(result) || env == nullptr)
                        {
                            BuildFallback(hwnd, ctx.get());
                            return S_OK;
                        }
                        env->CreateCoreWebView2Controller(hwnd,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [hwnd, ctx](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT
                                {
                                    if (!ctx->alive.load() || ctx->hwnd == nullptr)
                                    {
                                        if (controller != nullptr)
                                        {
                                            controller->Close();
                                        }
                                        return S_OK;
                                    }
                                    if (FAILED(r2) || controller == nullptr)
                                    {
                                        BuildFallback(hwnd, ctx.get());
                                        return S_OK;
                                    }
                                    ctx->controller = controller;
                                    controller->AddRef();
                                    RECT rc;
                                    GetClientRect(hwnd, &rc);
                                    RECT bounds{0, 0, rc.right, ContentHeight(hwnd, ctx.get())};
                                    controller->put_Bounds(bounds);
                                    ICoreWebView2* webview = nullptr;
                                    controller->get_CoreWebView2(&webview);
                                    if (webview != nullptr)
                                    {
                                        webview->NavigateToString(ctx->html.c_str());
                                        webview->Release();
                                    }
                                    return S_OK;
                                }).Get());
                        return S_OK;
                    }).Get());
            return SUCCEEDED(hr);
        }
#endif

        void ReportThread(DoctorReport report, bool preferWebView2)
        {
            // Always drop the live count, including on CreateWindow failure or unwind.
            struct LiveGuard
            {
                ~LiveGuard()
                {
                    TrackReportThread(-1);
                }
            } liveGuard;

            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

            HINSTANCE hInst = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc;
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &WndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);

            std::shared_ptr<ReportCtx> ctx = std::make_shared<ReportCtx>();
            ctx->html = WideFromUtf8(BuildReportHtml(report));
            ctx->text = WideFromUtf8(BuildReportText(report));
            ctx->preferWebView2 = preferWebView2;
            for (const auto& f : report.findings)
            {
                if (f.actionKind == ActionKind::DeviceDisableWake && !f.actionArg.empty())
                {
                    ctx->wakeDevices.push_back(f.actionArg);
                }
            }

            RECT wa{0, 0, 1000, 740};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int w = 1000;
            int h = 740;
            int x = wa.left + ((wa.right - wa.left) - w) / 2;
            int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

            HWND hwnd = CreateWindowExW(0, kClass, L"OhMyDisplay - Sleep Doctor",
                                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h,
                                        nullptr, nullptr, hInst, ctx.get());
            if (hwnd == nullptr)
            {
                CoUninitialize();
                return;
            }
            ctx->hwnd = hwnd;
            RegisterReportHwnd(hwnd);

            if (!ctx->wakeDevices.empty())
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int barTop = rc.bottom - kActionBarH;
                int bx = 8;
                for (size_t i = 0; i < ctx->wakeDevices.size() && i < 3; ++i)
                {
                    std::wstring label = L"Disable wake: " + WideFromUtf8(ctx->wakeDevices[i]);
                    if (label.size() > 36)
                    {
                        label = label.substr(0, 33) + L"...";
                    }
                    HWND btn = CreateWindowExW(0, L"BUTTON", label.c_str(),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               bx, barTop + 6, 280, 28, hwnd,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(
                                                   kActionIdBase + static_cast<int>(i))),
                                               hInst, nullptr);
                    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                    SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                    bx += 288;
                }
                // Cap buttons at 3 so the bar stays usable; extras stay as text in the report.
                if (ctx->wakeDevices.size() > 3)
                {
                    ctx->wakeDevices.resize(3);
                }
            }

            ShowWindow(hwnd, SW_SHOW);

            bool usingWebView = false;
#ifdef OMD_WITH_WEBVIEW2
            if (preferWebView2)
            {
                usingWebView = TryStartWebView2(hwnd, ctx);
                if (!usingWebView)
                {
                    OMD_LOGW("report: WebView2 env start failed, using text fallback");
                }
            }
#endif
            if (!usingWebView)
            {
                BuildFallback(hwnd, ctx.get());
            }

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                if (!IsDialogMessageW(hwnd, &msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            ctx.reset();
            CoUninitialize();
        }
    }

    void ShowReportWindow(const DoctorReport& report, bool preferWebView2)
    {
        // Count the thread before detach so Exit cannot observe live==0 in the
        // gap between detach and the worker's first line.
        TrackReportThread(1);
        try
        {
            std::thread(ReportThread, report, preferWebView2).detach();
        }
        catch (...)
        {
            TrackReportThread(-1);
        }
    }

    void CloseReportWindows()
    {
        // The worker may not have created its HWND yet. Keep posting WM_CLOSE to
        // whatever is registered until the live count drops to zero.
        while (true)
        {
            std::vector<HWND> hwnds;
            {
                std::lock_guard<std::mutex> lock(g_reportMu);
                if (g_reportLive == 0)
                {
                    return;
                }
                hwnds = g_reportHwnds;
            }
            for (HWND h : hwnds)
            {
                if (h != nullptr)
                {
                    PostMessageW(h, WM_CLOSE, 0, 0);
                }
            }
            std::unique_lock<std::mutex> lock(g_reportMu);
            if (g_reportCv.wait_for(lock, std::chrono::milliseconds(50),
                                    [] { return g_reportLive == 0; }))
            {
                return;
            }
        }
    }
}
