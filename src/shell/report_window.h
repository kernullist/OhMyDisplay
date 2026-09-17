// OhMyDisplay - embedded Sleep Doctor report window.
//
// Renders the report with WebView2 when the SDK is compiled in and the Evergreen
// runtime is present; otherwise falls back to a read-only native text view. Runs on
// its own thread with its own COM apartment and message loop, so callers (worker
// threads) can fire-and-forget.
#pragma once

#include "doctor/rules_engine.h"

namespace omd
{
    // Show the report window. Returns immediately; the window owns its thread.
    // preferWebView2 is honored when the SDK was compiled in; otherwise the native
    // text view is used. DeviceDisableWake findings get one-click buttons.
    void ShowReportWindow(const DoctorReport& report, bool preferWebView2 = true);

    // Close every live report window and wait until their threads exit. Call from
    // the main shutdown path so we do not tear down CRT while a report STA is live.
    void CloseReportWindows();
}
