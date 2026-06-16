// OhMyDisplay - report rendering implementation.
#include "doctor/report.h"
#include "core/store.h"
#include "core/paths.h"
#include "core/common.h"
#include "keeper/layout_store.h" // NowUtcIso

#include <sstream>

namespace omd
{
    namespace
    {
        std::string HtmlEscape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (char c : s)
            {
                switch (c)
                {
                case '&':
                    out += "&amp;";
                    break;
                case '<':
                    out += "&lt;";
                    break;
                case '>':
                    out += "&gt;";
                    break;
                case '"':
                    out += "&quot;";
                    break;
                default:
                    out.push_back(c);
                    break;
                }
            }
            return out;
        }

        const char* SeverityClass(Severity s)
        {
            return s == Severity::Warn ? "warn" : "info";
        }

        const char* SeverityLabel(Severity s)
        {
            return s == Severity::Warn ? "WARN" : "INFO";
        }
    }

    std::string BuildReportText(const DoctorReport& report)
    {
        std::ostringstream os;
        os << "OhMyDisplay - Sleep Doctor report\r\n";
        os << "Generated: " << NowUtcIso() << "\r\n";
        os << "========================================\r\n\r\n";

        os << "FINDINGS\r\n";
        for (const auto& f : report.findings)
        {
            os << "  [" << SeverityLabel(f.severity) << "] " << f.symptom << "\r\n";
            os << "      " << f.plain << "\r\n";
            if (!f.action.empty())
            {
                os << "      -> " << f.action << "\r\n";
            }
            os << "\r\n";
        }

        const SleepStudyData& ss = report.data.sleepStudy;
        os << "SLEEP ARCHITECTURE\r\n";
        os << "  Modern Standby: " << (ss.modernStandby ? "yes (S0 low power idle)" : "no (legacy S3)") << "\r\n";
        if (ss.collected)
        {
            os << "  SleepStudy report: " << Utf8FromWide(ss.reportPath) << "\r\n";
            if (ss.avgDripsPercent >= 0.0)
            {
                char pct[16];
                std::snprintf(pct, sizeof(pct), "%.0f%%", ss.avgDripsPercent);
                os << "  DRIPS (approx): " << pct << "\r\n";
            }
        }
        os << "\r\n";

        os << "POWER TIMELINE (recent)\r\n";
        for (const auto& e : report.timeline)
        {
            os << "  " << e.timeUtc << "  " << e.provider << " (" << e.eventId << ")  "
               << e.summary << "\r\n";
        }
        return os.str();
    }

    std::string BuildReportHtml(const DoctorReport& report)
    {
        std::ostringstream os;
        os << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
              "<title>OhMyDisplay - Sleep Doctor</title><style>"
              "body{font-family:Segoe UI,Arial,sans-serif;margin:24px;color:#1b1b1b;background:#fafafa;}"
              "h1{font-size:20px;margin:0 0 4px;}"
              ".sub{color:#666;font-size:12px;margin-bottom:20px;}"
              "table{border-collapse:collapse;width:100%;margin-bottom:24px;background:#fff;}"
              "th,td{border:1px solid #e0e0e0;padding:8px 10px;text-align:left;vertical-align:top;font-size:13px;}"
              "th{background:#f0f3f7;}"
              ".warn{border-left:4px solid #d9822b;}"
              ".info{border-left:4px solid #2f6fed;}"
              ".badge{font-weight:bold;font-size:11px;padding:1px 6px;border-radius:3px;color:#fff;}"
              ".badge.warn{background:#d9822b;}"
              ".badge.info{background:#2f6fed;}"
              ".action{color:#0a6;}"
              "</style></head><body>";

        os << "<h1>Sleep Doctor report</h1>";
        os << "<div class=\"sub\">Generated " << HtmlEscape(NowUtcIso())
           << " &middot; local data only, nothing is sent anywhere</div>";

        os << "<h2 style=\"font-size:15px\">Findings</h2>";
        os << "<table><tr><th>Severity</th><th>Symptom</th><th>What's happening</th><th>Recommended action</th></tr>";
        for (const auto& f : report.findings)
        {
            os << "<tr class=\"" << SeverityClass(f.severity) << "\">"
               << "<td><span class=\"badge " << SeverityClass(f.severity) << "\">"
               << SeverityLabel(f.severity) << "</span></td>"
               << "<td>" << HtmlEscape(f.symptom) << "</td>"
               << "<td>" << HtmlEscape(f.plain) << "</td>"
               << "<td class=\"action\">" << HtmlEscape(f.action) << "</td></tr>";
        }
        os << "</table>";

        const SleepStudyData& ss = report.data.sleepStudy;
        os << "<h2 style=\"font-size:15px\">Sleep architecture</h2>";
        os << "<table><tr><th>Property</th><th>Value</th></tr>";
        os << "<tr><td>Modern Standby</td><td>"
           << (ss.modernStandby ? "Yes (S0 low power idle)" : "No (legacy S3)") << "</td></tr>";
        if (ss.collected)
        {
            if (ss.avgDripsPercent >= 0.0)
            {
                char pct[16];
                std::snprintf(pct, sizeof(pct), "%.0f%%", ss.avgDripsPercent);
                os << "<tr><td>DRIPS (approx)</td><td>" << pct << "</td></tr>";
            }
            // Build a file URL for the official report.
            std::string url = "file:///" + Utf8FromWide(ss.reportPath);
            for (char& c : url)
            {
                if (c == '\\')
                {
                    c = '/';
                }
            }
            os << "<tr><td>Full report</td><td><a href=\"" << HtmlEscape(url)
               << "\">Open SleepStudy report</a></td></tr>";
        }
        os << "</table>";

        os << "<h2 style=\"font-size:15px\">Power timeline</h2>";
        os << "<table><tr><th>Time (UTC)</th><th>Provider</th><th>Event</th><th>Detail</th></tr>";
        for (const auto& e : report.timeline)
        {
            os << "<tr><td>" << HtmlEscape(e.timeUtc) << "</td>"
               << "<td>" << HtmlEscape(e.provider) << "</td>"
               << "<td>" << e.eventId << "</td>"
               << "<td>" << HtmlEscape(e.summary) << "</td></tr>";
        }
        os << "</table>";

        os << "</body></html>";
        return os.str();
    }

    std::wstring WriteHtmlReport(const DoctorReport& report)
    {
        std::wstring path = DataFile(L"report.html");
        if (path.empty())
        {
            return std::wstring();
        }
        std::string html = BuildReportHtml(report);

        // Write directly (not via the atomic JSON store) since this is plain HTML.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return std::wstring();
        }
        DWORD written = 0;
        WriteFile(h, html.data(), static_cast<DWORD>(html.size()), &written, nullptr);
        CloseHandle(h);
        return path;
    }
}
