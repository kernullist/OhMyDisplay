// OhMyDisplay - layouts.json persistence implementation.
#include "keeper/layout_store.h"
#include "core/store.h"
#include "core/paths.h"
#include "core/json.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdio>

namespace omd
{
    namespace
    {
        const char* ShowCmdToStr(int showCmd)
        {
            if (showCmd == SW_SHOWMINIMIZED || showCmd == SW_MINIMIZE || showCmd == SW_SHOWMINNOACTIVE)
            {
                return "min";
            }
            if (showCmd == SW_SHOWMAXIMIZED || showCmd == SW_MAXIMIZE)
            {
                return "max";
            }
            return "normal";
        }

        int StrToShowCmd(const std::string& s)
        {
            if (s == "min")
            {
                return SW_SHOWMINNOACTIVE;
            }
            if (s == "max")
            {
                return SW_SHOWMAXIMIZED;
            }
            return SW_SHOWNORMAL;
        }

        json::Value EdidToJson(const EdidInfo& e)
        {
            json::Value o = json::Value::MakeObject();
            o.Set("mfg", json::Value(e.mfg));
            o.Set("product", json::Value(static_cast<int>(e.product)));
            o.Set("serial", json::Value(e.serial));
            return o;
        }

        EdidInfo EdidFromJson(const json::Value& v)
        {
            EdidInfo e;
            if (v.IsObject())
            {
                e.mfg = v["mfg"].AsStringOr("");
                e.product = static_cast<uint32_t>(v["product"].AsInt(0));
                e.serial = v["serial"].AsStringOr("");
            }
            return e;
        }

        json::Value WindowToJson(const WindowSnap& w)
        {
            json::Value o = json::Value::MakeObject();
            o.Set("exe", json::Value(w.exe));
            o.Set("title", json::Value(w.title));
            o.Set("monitorId", json::Value(w.monitorId));

            json::Value placement = json::Value::MakeObject();
            placement.Set("showCmd", json::Value(std::string(ShowCmdToStr(w.showCmd))));
            json::Value rect = json::Value::MakeObject();
            rect.Set("x", json::Value(w.nx));
            rect.Set("y", json::Value(w.ny));
            rect.Set("w", json::Value(w.nw));
            rect.Set("h", json::Value(w.nh));
            placement.Set("normRect", std::move(rect));
            o.Set("placement", std::move(placement));

            json::Value match = json::Value::MakeObject();
            match.Set("class", json::Value(w.className));
            o.Set("match", std::move(match));
            // HWND is runtime-only. Persisting it across sessions matches the wrong
            // window after handle reuse (same exe, recycled HWND).
            return o;
        }

        WindowSnap WindowFromJson(const json::Value& v)
        {
            WindowSnap w;
            w.exe = v["exe"].AsStringOr("");
            // Recompute leaf for exclusion checks at restore time.
            {
                size_t pos = w.exe.find_last_of("\\/");
                std::string leaf = (pos == std::string::npos) ? w.exe : w.exe.substr(pos + 1);
                for (char& c : leaf)
                {
                    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                }
                w.exeLeaf = leaf;
            }
            w.title = v["title"].AsStringOr("");
            w.monitorId = v["monitorId"].AsStringOr("");
            const json::Value* p = v.Find("placement");
            if (p != nullptr && p->IsObject())
            {
                w.showCmd = StrToShowCmd((*p)["showCmd"].AsStringOr("normal"));
                const json::Value* r = p->Find("normRect");
                if (r != nullptr && r->IsObject())
                {
                    w.nx = (*r)["x"].AsNumber(0.0);
                    w.ny = (*r)["y"].AsNumber(0.0);
                    w.nw = (*r)["w"].AsNumber(0.0);
                    w.nh = (*r)["h"].AsNumber(0.0);
                }
            }
            const json::Value* m = v.Find("match");
            if (m != nullptr && m->IsObject())
            {
                w.className = (*m)["class"].AsStringOr("");
            }
            return w;
        }
    }

    std::string NowUtcIso()
    {
        SYSTEMTIME st;
        GetSystemTime(&st);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::string(buf);
    }

    std::wstring LayoutStore::FilePath()
    {
        return DataFile(L"layouts.json");
    }

    StoredTopology* LayoutStore::FindBySignature(const std::string& signature)
    {
        for (auto& t : topologies_)
        {
            if (t.signature == signature)
            {
                return &t;
            }
        }
        return nullptr;
    }

    const StoredTopology* LayoutStore::FindBySignature(const std::string& signature) const
    {
        for (const auto& t : topologies_)
        {
            if (t.signature == signature)
            {
                return &t;
            }
        }
        return nullptr;
    }

    void LayoutStore::Upsert(const Topology& topo, const std::vector<WindowSnap>& windows)
    {
        StoredTopology* st = FindBySignature(topo.signature);
        if (st == nullptr)
        {
            topologies_.push_back(StoredTopology{});
            st = &topologies_.back();
            st->signature = topo.signature;
        }
        st->lastSeenUtc = NowUtcIso();
        st->windows = windows;

        st->monitors.clear();
        for (const auto& m : topo.monitors)
        {
            StoredMonitor sm;
            sm.id = m.id;
            sm.label = m.label;
            sm.edid = m.edid;
            sm.devicePath = m.devicePath;
            sm.friendlyName = m.friendlyName;
            st->monitors.push_back(std::move(sm));
        }
    }

    bool LayoutStore::Load()
    {
        bool ok = true;
        topologies_.clear();
        do
        {
            json::Value root;
            if (!Store::Load(FilePath(), root) || !root.IsObject())
            {
                // Fresh start.
                break;
            }

            int ver = root["schemaVersion"].AsInt(0);
            if (ver != kSchemaVersion)
            {
                OMD_LOGW("layouts: schema version %d != %d, starting fresh (old kept as .bak)", ver, kSchemaVersion);
                std::wstring bak = FilePath() + L".v" + std::to_wstring(ver) + L".bak";
                CopyFileW(FilePath().c_str(), bak.c_str(), FALSE);
                break;
            }

            const json::Value* lbl = root.Find("labels");
            if (lbl != nullptr)
            {
                labels_.LoadFrom(*lbl);
            }

            const json::Value* topos = root.Find("topologies");
            if (topos != nullptr && topos->IsArray())
            {
                for (const auto& tv : topos->AsArray())
                {
                    if (!tv.IsObject())
                    {
                        continue;
                    }
                    StoredTopology st;
                    st.signature = tv["signature"].AsStringOr("");
                    st.lastSeenUtc = tv["lastSeenUtc"].AsStringOr("");

                    const json::Value* mons = tv.Find("monitors");
                    if (mons != nullptr && mons->IsArray())
                    {
                        for (const auto& mv : mons->AsArray())
                        {
                            StoredMonitor sm;
                            sm.id = mv["id"].AsStringOr("");
                            sm.label = mv["label"].AsStringOr("");
                            sm.devicePath = mv["devicePath"].AsStringOr("");
                            sm.friendlyName = mv["friendlyName"].AsStringOr("");
                            sm.edid = EdidFromJson(mv["edid"]);
                            st.monitors.push_back(std::move(sm));
                        }
                    }

                    const json::Value* wins = tv.Find("windows");
                    if (wins != nullptr && wins->IsArray())
                    {
                        for (const auto& wv : wins->AsArray())
                        {
                            st.windows.push_back(WindowFromJson(wv));
                        }
                    }
                    topologies_.push_back(std::move(st));
                }
            }
        } while (false);
        return ok;
    }

    bool LayoutStore::Save() const
    {
        json::Value root = json::Value::MakeObject();
        root.Set("schemaVersion", json::Value(kSchemaVersion));
        root.Set("labels", labels_.ToJson());

        json::Value topos = json::Value::MakeArray();
        for (const auto& t : topologies_)
        {
            json::Value tv = json::Value::MakeObject();
            tv.Set("signature", json::Value(t.signature));

            json::Value mons = json::Value::MakeArray();
            for (const auto& m : t.monitors)
            {
                json::Value mv = json::Value::MakeObject();
                mv.Set("id", json::Value(m.id));
                mv.Set("label", json::Value(m.label));
                mv.Set("edid", EdidToJson(m.edid));
                mv.Set("devicePath", json::Value(m.devicePath));
                mv.Set("friendlyName", json::Value(m.friendlyName));
                mons.Push(std::move(mv));
            }
            tv.Set("monitors", std::move(mons));
            tv.Set("lastSeenUtc", json::Value(t.lastSeenUtc));

            json::Value wins = json::Value::MakeArray();
            for (const auto& w : t.windows)
            {
                wins.Push(WindowToJson(w));
            }
            tv.Set("windows", std::move(wins));
            topos.Push(std::move(tv));
        }
        root.Set("topologies", std::move(topos));

        bool ok = Store::Save(FilePath(), root);
        if (!ok)
        {
            OMD_LOGE("layouts: save failed");
        }
        return ok;
    }
}
