// OhMyDisplay - settings load/save implementation.
#include "core/settings.h"
#include "core/store.h"
#include "core/paths.h"
#include "core/json.h"
#include "core/logger.h"

namespace omd
{
    namespace
    {
        std::vector<std::string> ReadStringArray(const json::Value& arr)
        {
            std::vector<std::string> out;
            if (arr.IsArray())
            {
                for (const auto& v : arr.AsArray())
                {
                    if (v.IsString())
                    {
                        out.push_back(v.AsString());
                    }
                }
            }
            return out;
        }

        json::Value WriteStringArray(const std::vector<std::string>& v)
        {
            json::Value arr = json::Value::MakeArray();
            for (const auto& s : v)
            {
                arr.Push(json::Value(s));
            }
            return arr;
        }
    }

    std::wstring Settings::FilePath()
    {
        return DataFile(L"settings.json");
    }

    bool Settings::Load()
    {
        bool ok = true;
        do
        {
            json::Value root;
            if (!Store::Load(FilePath(), root) || !root.IsObject())
            {
                // No file yet: keep defaults.
                break;
            }

            const json::Value* k = root.Find("keeper");
            if (k != nullptr && k->IsObject())
            {
                keeper.enabled = (*k)["enabled"].AsBool(keeper.enabled);
                keeper.restoreRetries = (*k)["restoreRetries"].AsInt(keeper.restoreRetries);
                keeper.elevateForAllWindows = (*k)["elevateForAllWindows"].AsBool(keeper.elevateForAllWindows);

                const json::Value* d = k->Find("debounce");
                if (d != nullptr && d->IsObject())
                {
                    keeper.debounce.baseMs = static_cast<DWORD>((*d)["baseMs"].AsInt(static_cast<int>(keeper.debounce.baseMs)));
                    keeper.debounce.perMonitorMs = static_cast<DWORD>((*d)["perMonitorMs"].AsInt(static_cast<int>(keeper.debounce.perMonitorMs)));
                    keeper.debounce.minMs = static_cast<DWORD>((*d)["minMs"].AsInt(static_cast<int>(keeper.debounce.minMs)));
                    keeper.debounce.maxMs = static_cast<DWORD>((*d)["maxMs"].AsInt(static_cast<int>(keeper.debounce.maxMs)));
                }

                const json::Value* ex = k->Find("excludeExe");
                if (ex != nullptr)
                {
                    keeper.excludeExe = ReadStringArray(*ex);
                }
                const json::Value* et = k->Find("excludeTitleSubstr");
                if (et != nullptr)
                {
                    keeper.excludeTitleSubstr = ReadStringArray(*et);
                }
            }

            const json::Value* doc = root.Find("doctor");
            if (doc != nullptr && doc->IsObject())
            {
                doctor.useWebView2 = (*doc)["useWebView2"].AsBool(doctor.useWebView2);
                doctor.collectSleepStudy = (*doc)["collectSleepStudy"].AsBool(doctor.collectSleepStudy);
            }

            const json::Value* hk = root.Find("hotkeys");
            if (hk != nullptr && hk->IsObject())
            {
                hotkeys.saveNow = (*hk)["saveNow"].AsStringOr(hotkeys.saveNow);
                hotkeys.restoreNow = (*hk)["restoreNow"].AsStringOr(hotkeys.restoreNow);
            }
        } while (false);
        return ok;
    }

    bool Settings::Save() const
    {
        json::Value root = json::Value::MakeObject();

        json::Value k = json::Value::MakeObject();
        k.Set("enabled", json::Value(keeper.enabled));
        json::Value d = json::Value::MakeObject();
        d.Set("baseMs", json::Value(static_cast<int>(keeper.debounce.baseMs)));
        d.Set("perMonitorMs", json::Value(static_cast<int>(keeper.debounce.perMonitorMs)));
        d.Set("minMs", json::Value(static_cast<int>(keeper.debounce.minMs)));
        d.Set("maxMs", json::Value(static_cast<int>(keeper.debounce.maxMs)));
        k.Set("debounce", std::move(d));
        k.Set("restoreRetries", json::Value(keeper.restoreRetries));
        k.Set("elevateForAllWindows", json::Value(keeper.elevateForAllWindows));
        k.Set("excludeExe", WriteStringArray(keeper.excludeExe));
        k.Set("excludeTitleSubstr", WriteStringArray(keeper.excludeTitleSubstr));
        root.Set("keeper", std::move(k));

        json::Value doc = json::Value::MakeObject();
        doc.Set("useWebView2", json::Value(doctor.useWebView2));
        doc.Set("collectSleepStudy", json::Value(doctor.collectSleepStudy));
        root.Set("doctor", std::move(doc));

        json::Value hk = json::Value::MakeObject();
        hk.Set("saveNow", json::Value(hotkeys.saveNow));
        hk.Set("restoreNow", json::Value(hotkeys.restoreNow));
        root.Set("hotkeys", std::move(hk));

        bool ok = Store::Save(FilePath(), root);
        if (!ok)
        {
            OMD_LOGE("settings: save failed");
        }
        return ok;
    }
}
