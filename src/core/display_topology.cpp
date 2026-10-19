// OhMyDisplay - CCD-based topology and monitor identity implementation.
#include "core/display_topology.h"
#include "core/common.h"
#include "core/logger.h"

#include <shellscalingapi.h>
#include <algorithm>
#include <map>

#pragma comment(lib, "shcore.lib")

namespace omd
{
    namespace
    {
        // Decode the packed EDID manufacturer id into 3 ASCII letters.
        std::string DecodeMfg(UINT16 edidManufactureId)
        {
            // EDID stores the id big-endian; swap then unpack three 5-bit letters.
            UINT16 id = static_cast<UINT16>((edidManufactureId >> 8) | (edidManufactureId << 8));
            char c1 = static_cast<char>(((id >> 10) & 0x1F) + 'A' - 1);
            char c2 = static_cast<char>(((id >> 5) & 0x1F) + 'A' - 1);
            char c3 = static_cast<char>((id & 0x1F) + 'A' - 1);
            std::string out;
            if (c1 >= 'A' && c1 <= 'Z')
            {
                out.push_back(c1);
            }
            if (c2 >= 'A' && c2 <= 'Z')
            {
                out.push_back(c2);
            }
            if (c3 >= 'A' && c3 <= 'Z')
            {
                out.push_back(c3);
            }
            if (out.empty())
            {
                out = "UNK";
            }
            return out;
        }

        uint32_t HashConnector(const LUID& adapterId, UINT32 sourceId)
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](uint32_t v)
            {
                for (int b = 0; b < 4; ++b)
                {
                    h ^= static_cast<uint8_t>(v & 0xFF);
                    h *= 1099511628211ull;
                    v >>= 8;
                }
            };
            mix(static_cast<uint32_t>(adapterId.LowPart));
            mix(static_cast<uint32_t>(adapterId.HighPart));
            mix(sourceId);
            return static_cast<uint32_t>(h ^ (h >> 32));
        }

        UINT QueryMonitorDpi(HMONITOR hmon)
        {
            UINT dpiX = 96;
            UINT dpiY = 96;
            if (SUCCEEDED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
            {
                return dpiX;
            }
            return 96;
        }

        struct GdiMonitor
        {
            std::string gdiName;
            RECT rcMonitor{};
            RECT rcWork{};
            UINT dpi = 96;
            bool primary = false;
        };

        BOOL CALLBACK EnumMonProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam)
        {
            auto* list = reinterpret_cast<std::vector<GdiMonitor>*>(lparam);
            MONITORINFOEXW mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi) != 0)
            {
                GdiMonitor g;
                g.gdiName = Utf8FromWide(mi.szDevice);
                g.rcMonitor = mi.rcMonitor;
                g.rcWork = mi.rcWork;
                g.dpi = QueryMonitorDpi(hmon);
                g.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                list->push_back(std::move(g));
            }
            return TRUE;
        }
    }

    const MonitorInfo* Topology::FindById(const std::string& id) const
    {
        for (const auto& m : monitors)
        {
            if (m.id == id)
            {
                return &m;
            }
        }
        return nullptr;
    }

    const MonitorInfo* Topology::FindByGdiName(const std::string& gdi) const
    {
        for (const auto& m : monitors)
        {
            if (m.gdiName == gdi)
            {
                return &m;
            }
        }
        return nullptr;
    }

    std::string ComputeSignature(const std::vector<std::string>& ids)
    {
        std::vector<std::string> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        std::string joined;
        for (const auto& id : sorted)
        {
            joined += id;
            joined.push_back('|');
        }
        return HashHex(joined);
    }

    bool QueryDisplayConfigRetry(UINT32 flags,
                                 std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                                 std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
    {
        bool ok = false;
        do
        {
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                UINT32 numPaths = 0;
                UINT32 numModes = 0;
                LONG rc = GetDisplayConfigBufferSizes(flags, &numPaths, &numModes);
                if (rc != ERROR_SUCCESS)
                {
                    Sleep(40);
                    continue;
                }
                if (numPaths == 0)
                {
                    paths.clear();
                    modes.clear();
                    ok = true;
                    break;
                }
                if (numPaths > 4096 || numModes > 16384)
                {
                    OMD_LOGE("ccd: implausible buffer sizes paths=%u modes=%u", numPaths, numModes);
                    break;
                }
                paths.resize(numPaths);
                modes.resize(numModes);
                rc = QueryDisplayConfig(flags, &numPaths, paths.data(),
                                        &numModes, modes.data(), nullptr);
                if (rc == ERROR_SUCCESS)
                {
                    paths.resize(numPaths);
                    modes.resize(numModes);
                    ok = true;
                    break;
                }
                if (rc != ERROR_INSUFFICIENT_BUFFER && rc != ERROR_GEN_FAILURE)
                {
                    OMD_LOGE("ccd: QueryDisplayConfig failed rc=%ld", rc);
                    break;
                }
                Sleep(40);
            }
        } while (false);
        return ok;
    }

    bool QueryActiveTopology(Topology& out)
    {
        out.monitors.clear();
        out.signature.clear();
        out.complete = true;

        bool ok = false;
        do
        {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths;
            std::vector<DISPLAYCONFIG_MODE_INFO> modes;
            if (!QueryDisplayConfigRetry(QDC_ONLY_ACTIVE_PATHS, paths, modes))
            {
                OMD_LOGE("ccd: QueryDisplayConfigRetry failed");
                break;
            }

            // Map GDI device name -> per-path target identity fields.
            struct TargetData
            {
                EdidInfo edid;
                std::string devicePath;
                std::string friendlyName;
                uint32_t connectorKey = 0;
            };
            std::map<std::string, TargetData> byGdi;

            for (const auto& p : paths)
            {
                // Source GDI device name for this path.
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
                ZeroMemory(&src, sizeof(src));
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof(src);
                src.header.adapterId = p.sourceInfo.adapterId;
                src.header.id = p.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS)
                {
                    continue;
                }
                std::string gdi = Utf8FromWide(src.viewGdiDeviceName);

                // Target name carries EDID mfg/product, friendly name, device path.
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
                ZeroMemory(&tgt, sizeof(tgt));
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof(tgt);
                tgt.header.adapterId = p.targetInfo.adapterId;
                tgt.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
                {
                    continue;
                }

                TargetData td;
                if (tgt.flags.edidIdsValid)
                {
                    td.edid.mfg = DecodeMfg(tgt.edidManufactureId);
                    td.edid.product = tgt.edidProductCodeId;
                }
                td.friendlyName = Utf8FromWide(tgt.monitorFriendlyDeviceName);
                td.devicePath = Utf8FromWide(tgt.monitorDevicePath);
                td.connectorKey = HashConnector(p.targetInfo.adapterId, p.sourceInfo.id);

                // First target per source wins (clone groups share a source).
                if (byGdi.find(gdi) == byGdi.end())
                {
                    byGdi.emplace(gdi, std::move(td));
                }
            }

            // Enumerate live GDI monitors for geometry/DPI, then join with CCD data.
            std::vector<GdiMonitor> gdiMons;
            EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&gdiMons));

            std::vector<std::string> ids;
            ids.reserve(gdiMons.size());

            for (const auto& g : gdiMons)
            {
                MonitorInfo m;
                m.gdiName = g.gdiName;
                m.rcMonitor = g.rcMonitor;
                m.rcWork = g.rcWork;
                m.dpi = g.dpi;
                m.primary = g.primary;

                auto it = byGdi.find(g.gdiName);
                if (it != byGdi.end())
                {
                    m.edid = it->second.edid;
                    m.devicePath = it->second.devicePath;
                    m.friendlyName = it->second.friendlyName;
                    m.connectorKey = it->second.connectorKey;
                }
                if (m.devicePath.empty())
                {
                    out.complete = false;
                }

                // Base identity: EDID mfg/product + stable device path. The device
                // path embeds the connector UID, which separates identical models.
                std::string idMaterial = m.edid.mfg;
                idMaterial.push_back('#');
                idMaterial += std::to_string(m.edid.product);
                idMaterial.push_back('#');
                idMaterial += m.devicePath.empty() ? m.gdiName : m.devicePath;
                m.baseId = "m_" + HashHex(idMaterial);
                m.id = m.baseId; // discriminator (if any) applied later by labels

                ids.push_back(m.id);
                out.monitors.push_back(std::move(m));
            }

            std::sort(out.monitors.begin(), out.monitors.end(),
                      [](const MonitorInfo& a, const MonitorInfo& b)
                      {
                          return a.id < b.id;
                      });
            out.signature = ComputeSignature(ids);
            if (out.monitors.empty())
            {
                out.complete = false;
            }
            ok = true;
        } while (false);
        return ok;
    }

    std::string MonitorIdForWindow(const Topology& topo, HWND hwnd)
    {
        std::string result;
        do
        {
            HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (hmon == nullptr)
            {
                break;
            }
            MONITORINFOEXW mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi) == 0)
            {
                break;
            }
            std::string gdi = Utf8FromWide(mi.szDevice);
            const MonitorInfo* m = topo.FindByGdiName(gdi);
            if (m != nullptr)
            {
                result = m->id;
            }
        } while (false);
        return result;
    }
}
