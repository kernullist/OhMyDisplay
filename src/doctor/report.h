// OhMyDisplay - Sleep Doctor report rendering (HTML with a plain-text fallback).
#pragma once

#include "doctor/rules_engine.h"
#include <string>

namespace omd
{
    // Render a self-contained HTML report (no external assets).
    std::string BuildReportHtml(const DoctorReport& report);

    // Render a plain-text report for logs / WebView2-unavailable fallback.
    std::string BuildReportText(const DoctorReport& report);

    // Write the HTML report to %LOCALAPPDATA%\OhMyDisplay\report.html.
    // Returns the file path, or empty on failure.
    std::wstring WriteHtmlReport(const DoctorReport& report);
}
