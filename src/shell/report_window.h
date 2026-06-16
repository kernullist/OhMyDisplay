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
    void ShowReportWindow(const DoctorReport& report);
}
