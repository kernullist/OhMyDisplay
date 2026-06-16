// OhMyDisplay - embedded report window implementation.
#include "shell/report_window.h"
#include "doctor/report.h"
#include "core/paths.h"
#include "core/common.h"
#include "core/logger.h"

#include <windows.h>
#include <thread>
#include <string>

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

        struct ReportCtx
        {
            std::wstring html;
            std::wstring text;
            HWND fallbackEdit = nullptr;
            bool fallbackBuilt = false;
#ifdef OMD_WITH_WEBVIEW2
            ICoreWebView2Controller* controller = nullptr;
#endif
        };

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
                0, 0, rc.right, rc.bottom, hwnd,
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
#ifdef OMD_WITH_WEBVIEW2
            if (ctx->controller != nullptr)
            {
                ctx->controller->put_Bounds(rc);
                return;
            }
#endif
            if (ctx->fallbackEdit != nullptr)
            {
                MoveWindow(ctx->fallbackEdit, 0, 0, rc.right, rc.bottom, TRUE);
            }
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
            case WM_SIZE:
                if (ctx != nullptr)
                {
                    OnResize(hwnd, ctx);
                }
                return 0;
            case WM_DESTROY:
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
        bool TryStartWebView2(HWND hwnd, ReportCtx* ctx)
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
                        if (FAILED(result) || env == nullptr)
                        {
                            BuildFallback(hwnd, ctx);
                            return S_OK;
                        }
                        env->CreateCoreWebView2Controller(hwnd,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [hwnd, ctx](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT
                                {
                                    if (FAILED(r2) || controller == nullptr)
                                    {
                                        BuildFallback(hwnd, ctx);
                                        return S_OK;
                                    }
                                    ctx->controller = controller;
                                    controller->AddRef();
                                    RECT rc;
                                    GetClientRect(hwnd, &rc);
                                    controller->put_Bounds(rc);
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

        void ReportThread(DoctorReport report)
        {
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

            ReportCtx ctx;
            ctx.html = WideFromUtf8(BuildReportHtml(report));
            ctx.text = WideFromUtf8(BuildReportText(report));

            RECT wa{0, 0, 1000, 740};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int w = 1000;
            int h = 740;
            int x = wa.left + ((wa.right - wa.left) - w) / 2;
            int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

            HWND hwnd = CreateWindowExW(0, kClass, L"OhMyDisplay - Sleep Doctor",
                                        WS_OVERLAPPEDWINDOW, x, y, w, h,
                                        nullptr, nullptr, hInst, &ctx);
            if (hwnd == nullptr)
            {
                CoUninitialize();
                return;
            }
            ShowWindow(hwnd, SW_SHOW);

            bool usingWebView = false;
#ifdef OMD_WITH_WEBVIEW2
            usingWebView = TryStartWebView2(hwnd, &ctx);
            if (!usingWebView)
            {
                OMD_LOGW("report: WebView2 env start failed, using text fallback");
            }
#endif
            if (!usingWebView)
            {
                BuildFallback(hwnd, &ctx);
            }

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

#ifdef OMD_WITH_WEBVIEW2
            if (ctx.controller != nullptr)
            {
                ctx.controller->Close();
                ctx.controller->Release();
                ctx.controller = nullptr;
            }
#endif
            CoUninitialize();
        }
    }

    void ShowReportWindow(const DoctorReport& report)
    {
        std::thread(ReportThread, report).detach();
    }
}
