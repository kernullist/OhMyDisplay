// OhMyDisplay - powercfg collector + parser implementation.
//
// Parsing uses a line tokenizer keyed off section headers rather than brittle regex,
// so minor wording changes between Windows builds do not break it.
#include "doctor/powercfg_collector.h"
#include "core/process_util.h"
#include "core/common.h"
#include "core/logger.h"

#include <sstream>
#include <algorithm>

namespace omd
{
    namespace
    {
        std::vector<std::string> SplitLines(const std::string& text)
        {
            std::vector<std::string> lines;
            std::string cur;
            for (char c : text)
            {
                if (c == '\n')
                {
                    if (!cur.empty() && cur.back() == '\r')
                    {
                        cur.pop_back();
                    }
                    lines.push_back(cur);
                    cur.clear();
                }
                else
                {
                    cur.push_back(c);
                }
            }
            if (!cur.empty())
            {
                lines.push_back(cur);
            }
            return lines;
        }

        bool LooksLikeAccessDenied(const std::string& text)
        {
            std::string low = ToLowerAscii(text);
            return low.find("do not have permission") != std::string::npos ||
                   low.find("requires administrator") != std::string::npos ||
                   low.find("access is denied") != std::string::npos ||
                   low.find("elevated") != std::string::npos;
        }

        // A section header is an all-caps word ending in ':' (e.g. "SYSTEM:").
        bool IsSectionHeader(const std::string& line, std::string& nameOut)
        {
            std::string_view t = Trim(line);
            if (t.size() < 2 || t.back() != ':')
            {
                return false;
            }
            std::string_view name = t.substr(0, t.size() - 1);
            for (char c : name)
            {
                if (!((c >= 'A' && c <= 'Z') || c == '_'))
                {
                    return false;
                }
            }
            nameOut.assign(name);
            return true;
        }

        // Parse a "[TAG] value" request line.
        bool ParseBracketLine(const std::string& line, std::string& tag, std::string& value)
        {
            std::string_view t = Trim(line);
            if (t.empty() || t.front() != '[')
            {
                return false;
            }
            size_t close = t.find(']');
            if (close == std::string_view::npos)
            {
                return false;
            }
            tag.assign(t.substr(1, close - 1));
            std::string_view rest = Trim(t.substr(close + 1));
            value.assign(rest);
            return true;
        }
    }

    std::vector<PowerRequest> ParseRequests(const std::string& text, bool& needAdmin)
    {
        std::vector<PowerRequest> out;
        needAdmin = false;
        if (LooksLikeAccessDenied(text))
        {
            needAdmin = true;
            return out;
        }

        std::vector<std::string> lines = SplitLines(text);
        std::string section;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string hdr;
            if (IsSectionHeader(lines[i], hdr))
            {
                section = hdr;
                continue;
            }
            std::string_view t = Trim(lines[i]);
            if (t.empty() || IEquals(t, "None."))
            {
                continue;
            }
            std::string tag;
            std::string value;
            if (ParseBracketLine(lines[i], tag, value))
            {
                PowerRequest r;
                r.category = section;
                r.tag = tag;
                r.value = value;
                // The next non-empty, non-header line is usually the reason.
                if (i + 1 < lines.size())
                {
                    std::string_view next = Trim(lines[i + 1]);
                    std::string dummy;
                    if (!next.empty() && next.front() != '[' && !IsSectionHeader(lines[i + 1], dummy))
                    {
                        r.reason.assign(next);
                    }
                }
                out.push_back(std::move(r));
            }
        }
        return out;
    }

    std::vector<WakeTimer> ParseWakeTimers(const std::string& text, bool& needAdmin)
    {
        std::vector<WakeTimer> out;
        needAdmin = false;
        if (LooksLikeAccessDenied(text))
        {
            needAdmin = true;
            return out;
        }
        std::string low = ToLowerAscii(text);
        if (low.find("no active wake timers") != std::string::npos)
        {
            return out;
        }

        std::vector<std::string> lines = SplitLines(text);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string_view t = Trim(lines[i]);
            std::string lowLine = ToLowerAscii(t);
            if (lowLine.rfind("timer set by", 0) == 0)
            {
                WakeTimer w;
                w.owner.assign(Trim(t.substr(std::string("Timer set by").size())));
                // Look ahead for a "Reason:" line.
                for (size_t j = i + 1; j < lines.size() && j < i + 4; ++j)
                {
                    std::string_view r = Trim(lines[j]);
                    std::string lr = ToLowerAscii(r);
                    if (lr.rfind("reason:", 0) == 0)
                    {
                        w.reason.assign(Trim(r.substr(7)));
                        break;
                    }
                }
                out.push_back(std::move(w));
            }
        }
        return out;
    }

    void ParseLastWake(const std::string& text, std::string& type, std::string& name, bool& empty)
    {
        // English field labels only (we keep source ASCII-only). On localized Windows
        // these will not match; in that case 'empty' is set so the caller falls back to
        // the event-log correlator, whose XML field names are not localized.
        type.clear();
        name.clear();
        empty = false;

        std::vector<std::string> lines = SplitLines(text);
        for (const auto& line : lines)
        {
            std::string_view t = Trim(line);
            std::string low = ToLowerAscii(t);
            if (low.rfind("type:", 0) == 0)
            {
                type.assign(Trim(t.substr(5)));
            }
            else if (low.rfind("friendly name:", 0) == 0)
            {
                name.assign(Trim(t.substr(std::string("friendly name:").size())));
            }
            else if (low.rfind("instance path:", 0) == 0 && name.empty())
            {
                name.assign(Trim(t.substr(std::string("instance path:").size())));
            }
        }
        // No extractable name -> treat as empty so the event-log timeline supplies the
        // wake reason. Covers both English zero-source and any localized output.
        empty = name.empty();
    }

    std::vector<std::string> ParseWakeArmed(const std::string& text)
    {
        std::vector<std::string> out;
        std::vector<std::string> lines = SplitLines(text);
        for (const auto& line : lines)
        {
            std::string_view t = Trim(line);
            if (t.empty())
            {
                continue;
            }
            if (IEquals(t, "NONE") || IEquals(t, "None") || IEquals(t, "None."))
            {
                continue;
            }
            out.emplace_back(t);
        }
        return out;
    }

    bool IsModernStandbyEnabled()
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        // Read CsEnabled; absent or 0 means legacy S3. Locale-independent.
        LONG rc = RegGetValueW(HKEY_LOCAL_MACHINE,
                               L"SYSTEM\\CurrentControlSet\\Control\\Power",
                               L"CsEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size);
        return rc == ERROR_SUCCESS && value == 1;
    }

    double ParseDripsFromReport(const std::string& reportText)
    {
        // Best-effort: scan for a "DRIPS" mention followed by a percentage. The official
        // report markup varies by build, so this is only a hint; -1 when not found.
        double result = -1.0;
        std::string low = ToLowerAscii(reportText);
        size_t pos = low.find("drips");
        while (pos != std::string::npos)
        {
            // Look ahead a short window for a number followed by '%'.
            size_t end = (pos + 200 < low.size()) ? pos + 200 : low.size();
            for (size_t i = pos; i + 1 < end; ++i)
            {
                if (low[i] == '%')
                {
                    // Walk back over digits/dot to capture the number.
                    size_t j = i;
                    while (j > pos && (isdigit(static_cast<unsigned char>(low[j - 1])) ||
                                       low[j - 1] == '.'))
                    {
                        --j;
                    }
                    if (j < i)
                    {
                        double v = std::strtod(low.substr(j, i - j).c_str(), nullptr);
                        if (v >= 0.0 && v <= 100.0)
                        {
                            result = v;
                            return result;
                        }
                    }
                }
            }
            pos = low.find("drips", pos + 5);
        }
        return result;
    }

    DoctorData CollectPowerData()
    {
        DoctorData data;
        data.sleepStudy.modernStandby = IsModernStandbyEnabled();

        ProcessResult req = RunCapture(L"powercfg /requests");
        if (req.launched)
        {
            data.requests = ParseRequests(req.output, data.requestsNeedAdmin);
            // Exit code is the locale-independent admin signal (text is localized).
            if (req.exitCode != 0)
            {
                data.requestsNeedAdmin = true;
                data.requests.clear();
            }
        }

        ProcessResult lw = RunCapture(L"powercfg /lastwake");
        if (lw.launched)
        {
            ParseLastWake(lw.output, data.lastWakeType, data.lastWakeName, data.lastWakeEmpty);
        }

        ProcessResult wt = RunCapture(L"powercfg /waketimers");
        if (wt.launched)
        {
            data.waketimers = ParseWakeTimers(wt.output, data.waketimersNeedAdmin);
            if (wt.exitCode != 0)
            {
                data.waketimersNeedAdmin = true;
                data.waketimers.clear();
            }
        }

        ProcessResult wa = RunCapture(L"powercfg /devicequery wake_armed");
        if (wa.launched)
        {
            data.wakeArmedDevices = ParseWakeArmed(wa.output);
        }

        OMD_LOGI("doctor: collected requests=%zu(adm=%d) waketimers=%zu(adm=%d) wakeArmed=%zu lastWake='%s'",
                 data.requests.size(), data.requestsNeedAdmin ? 1 : 0,
                 data.waketimers.size(), data.waketimersNeedAdmin ? 1 : 0,
                 data.wakeArmedDevices.size(), data.lastWakeName.c_str());
        return data;
    }
}
