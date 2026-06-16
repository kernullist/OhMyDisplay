// OhMyDisplay - power event-log correlator implementation.
#include "core/event_log.h"
#include "core/common.h"
#include "core/logger.h"

#include <windows.h>
#include <winevt.h>
#include <vector>

#pragma comment(lib, "wevtapi.lib")

namespace omd
{
    namespace
    {
        // Extract the value of an attribute or element from a rendered event XML.
        // For attributes pass needle like " SystemTime='"; for elements "<EventID>".
        std::string ExtractBetween(const std::wstring& xml, const std::wstring& startNeedle,
                                   const std::wstring& endNeedle)
        {
            std::string out;
            size_t s = xml.find(startNeedle);
            if (s != std::wstring::npos)
            {
                s += startNeedle.size();
                size_t e = xml.find(endNeedle, s);
                if (e != std::wstring::npos)
                {
                    out = Utf8FromWide(xml.substr(s, e - s));
                }
            }
            return out;
        }

        // Pull a named EventData field: <Data Name='Foo'>value</Data>.
        std::string ExtractData(const std::wstring& xml, const std::wstring& name)
        {
            std::wstring needle = L"Name='" + name + L"'>";
            size_t s = xml.find(needle);
            if (s == std::wstring::npos)
            {
                return std::string();
            }
            s += needle.size();
            size_t e = xml.find(L"</Data>", s);
            if (e == std::wstring::npos)
            {
                return std::string();
            }
            return Utf8FromWide(xml.substr(s, e - s));
        }

        std::string SummarizeKernelPower(int id)
        {
            switch (id)
            {
            case 42:
                return "System entered sleep";
            case 107:
                return "System resumed from sleep";
            case 41:
                return "Unexpected shutdown (power loss / crash)";
            case 109:
                return "Kernel power transition";
            case 187:
                return "Wake source recorded";
            default:
                return "Kernel-Power event";
            }
        }
    }

    std::vector<PowerEvent> QueryPowerTimeline(int maxEvents)
    {
        std::vector<PowerEvent> out;

        // XPath: power-relevant events from the two key providers.
        const wchar_t* query =
            L"*[System[Provider[@Name='Microsoft-Windows-Kernel-Power'] or "
            L"Provider[@Name='Microsoft-Windows-Power-Troubleshooter']]]";

        EVT_HANDLE hResults = EvtQuery(nullptr, L"System", query,
                                       EvtQueryReverseDirection | EvtQueryTolerateQueryErrors);
        if (hResults == nullptr)
        {
            OMD_LOGW("evtlog: EvtQuery failed gle=%lu", GetLastError());
            return out;
        }

        EVT_HANDLE events[16];
        DWORD returned = 0;
        int collected = 0;
        while (collected < maxEvents &&
               EvtNext(hResults, 16, events, INFINITE, 0, &returned) != FALSE)
        {
            for (DWORD i = 0; i < returned; ++i)
            {
                DWORD bufUsed = 0;
                DWORD propCount = 0;
                EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &bufUsed, &propCount);
                std::wstring xml;
                if (bufUsed > 0)
                {
                    xml.resize(bufUsed / sizeof(wchar_t) + 1);
                    if (EvtRender(nullptr, events[i], EvtRenderEventXml,
                                  static_cast<DWORD>(xml.size() * sizeof(wchar_t)),
                                  xml.data(), &bufUsed, &propCount) != FALSE)
                    {
                        PowerEvent pe;
                        // <EventID> may carry a Qualifiers attribute; skip to the '>'.
                        {
                            std::string idChunk = ExtractBetween(xml, L"<EventID", L"</EventID>");
                            size_t gt = idChunk.find('>');
                            std::string digits = (gt == std::string::npos) ? idChunk : idChunk.substr(gt + 1);
                            pe.eventId = std::atoi(digits.c_str());
                        }
                        pe.timeUtc = ExtractBetween(xml, L"SystemTime='", L"'");
                        bool isTrouble = xml.find(L"Power-Troubleshooter") != std::wstring::npos;
                        if (isTrouble)
                        {
                            pe.provider = "Power-Troubleshooter";
                            pe.wakeSource = ExtractData(xml, L"WakeSource");
                            if (pe.wakeSource.empty())
                            {
                                pe.wakeSource = ExtractData(xml, L"WakeSourceText");
                            }
                            pe.summary = pe.wakeSource.empty()
                                             ? std::string("Wake event (source unspecified)")
                                             : ("Wake source: " + pe.wakeSource);
                        }
                        else
                        {
                            pe.provider = "Kernel-Power";
                            pe.summary = SummarizeKernelPower(pe.eventId);
                        }
                        out.push_back(std::move(pe));
                        collected++;
                    }
                }
                EvtClose(events[i]);
                if (collected >= maxEvents)
                {
                    break;
                }
            }
        }
        EvtClose(hResults);
        OMD_LOGI("evtlog: collected %zu power events", out.size());
        return out;
    }
}
