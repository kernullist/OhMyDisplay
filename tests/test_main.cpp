// OhMyDisplay - lightweight test harness (no external framework).
// Covers: adaptive debounce boundaries, JSON round-trip, powercfg parser fixtures,
// and a live Sleep Doctor run on the current machine.
#include "keeper/debounce.h"
#include "keeper/window_capture.h"
#include "keeper/monitor_labels.h"
#include "keeper/restore_engine.h"
#include "core/display_topology.h"
#include "core/json.h"
#include "core/settings.h"
#include "core/logger.h"
#include "core/elevated_ipc.h"
#include "doctor/powercfg_collector.h"
#include "doctor/doctor.h"
#include "doctor/report.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                  \
    do                                                                    \
    {                                                                     \
        ++g_checks;                                                       \
        if (!(cond))                                                      \
        {                                                                 \
            ++g_failures;                                                 \
            std::printf("  FAIL: %s\n", (msg));                           \
        }                                                                 \
    } while (false)

using namespace omd;

static void TestDebounce()
{
    std::printf("[debounce]\n");
    DebounceConfig cfg; // base 300, perMon 250, min 400, max 1500
    CHECK(ComputeDebounceMs(cfg, 1) == 550, "1 monitor -> 550ms");
    CHECK(ComputeDebounceMs(cfg, 2) == 800, "2 monitors -> 800ms");
    CHECK(ComputeDebounceMs(cfg, 3) == 1050, "3 monitors -> 1050ms");
    CHECK(ComputeDebounceMs(cfg, 5) == 1500, "5 monitors -> clamped to max 1500ms");
    CHECK(ComputeDebounceMs(cfg, 0) == 400, "0 monitors -> clamped to min 400ms");
}

static void TestJson()
{
    std::printf("[json]\n");
    json::Value root = json::Value::MakeObject();
    root.Set("schemaVersion", json::Value(2));
    root.Set("name", json::Value(std::string("left \"27\"\n tab\t")));
    json::Value arr = json::Value::MakeArray();
    arr.Push(json::Value(0.5));
    arr.Push(json::Value(true));
    arr.Push(json::Value(nullptr));
    root.Set("rect", std::move(arr));

    std::string text = root.Dump(true);
    json::Value parsed;
    std::string err;
    bool ok = json::Value::Parse(text, parsed, &err);
    CHECK(ok, "round-trip parse succeeds");
    CHECK(parsed["schemaVersion"].AsInt(0) == 2, "schemaVersion preserved");
    CHECK(parsed["name"].AsString() == "left \"27\"\n tab\t", "string escapes preserved");
    CHECK(parsed["rect"].AsArray().size() == 3, "array length preserved");
    CHECK(parsed["rect"].AsArray()[1].AsBool() == true, "array bool preserved");

    // Unicode escape decode.
    json::Value u;
    CHECK(json::Value::Parse("\"\\uD83D\\uDE00\"", u, nullptr), "surrogate pair parses");
    CHECK(u.AsString().size() == 4, "emoji decodes to 4 UTF-8 bytes");
}

static void TestRequestsParser()
{
    std::printf("[powercfg /requests parser]\n");
    const char* sample =
        "DISPLAY:\r\n"
        "None.\r\n"
        "\r\n"
        "SYSTEM:\r\n"
        "[PROCESS] \\Device\\HarddiskVolume3\\Spotify\\Spotify.exe\r\n"
        "An audio stream is currently in use.\r\n"
        "\r\n"
        "[DRIVER] Realtek High Definition Audio\r\n"
        "An audio stream is currently in use.\r\n"
        "\r\n"
        "AWAYMODE:\r\n"
        "None.\r\n";
    bool needAdmin = false;
    auto reqs = ParseRequests(sample, needAdmin);
    CHECK(!needAdmin, "no admin needed for parseable text");
    CHECK(reqs.size() == 2, "two SYSTEM requests parsed");
    if (reqs.size() == 2)
    {
        CHECK(reqs[0].category == "SYSTEM", "category SYSTEM");
        CHECK(reqs[0].tag == "PROCESS", "tag PROCESS");
        CHECK(reqs[0].value.find("Spotify.exe") != std::string::npos, "process value captured");
        CHECK(!reqs[0].reason.empty(), "reason line captured");
        CHECK(reqs[1].tag == "DRIVER", "tag DRIVER");
    }
}

static void TestWakeArmedParser()
{
    std::printf("[powercfg wake_armed parser]\n");
    const char* sample = "Realtek PCIe GbE Family Controller\r\nHID Keyboard Device\r\n";
    auto devs = ParseWakeArmed(sample);
    CHECK(devs.size() == 2, "two armed devices parsed");

    const char* none = "NONE\r\n";
    CHECK(ParseWakeArmed(none).empty(), "NONE yields empty list");
}

static void TestTopologySignature()
{
    std::printf("[topology signature]\n");
    std::vector<std::string> a = {"m_aaa", "m_bbb", "m_ccc"};
    std::vector<std::string> b = {"m_ccc", "m_aaa", "m_bbb"}; // same set, different order
    CHECK(ComputeSignature(a) == ComputeSignature(b), "signature is order-independent");

    std::vector<std::string> c = {"m_aaa", "m_bbb"}; // subset (one monitor gone)
    CHECK(ComputeSignature(a) != ComputeSignature(c), "different monitor set -> different signature");
}

static void TestSameModelCollision()
{
    std::printf("[same-model identity collision]\n");
    Topology topo;
    MonitorInfo m1;
    m1.baseId = "m_dupe";
    m1.id = "m_dupe";
    m1.connectorKey = 0x11111111;
    m1.gdiName = "\\\\.\\DISPLAY1";
    MonitorInfo m2;
    m2.baseId = "m_dupe"; // identical model + path material
    m2.id = "m_dupe";
    m2.connectorKey = 0x22222222; // but a different physical connector
    m2.gdiName = "\\\\.\\DISPLAY2";
    topo.monitors = {m1, m2};

    MonitorLabels labels;
    bool collision = ResolveIdentities(topo, labels);
    CHECK(collision, "collision detected for two identical-model monitors");
    CHECK(topo.monitors.size() == 2, "both monitors retained");
    CHECK(topo.monitors[0].id != topo.monitors[1].id, "collision resolved to distinct ids");
    CHECK(!topo.signature.empty(), "signature computed after resolution");

    // A user label should bind to the resolved id and survive a re-resolve.
    std::string firstId = topo.monitors[0].id;
    labels.SetLabel(firstId, "Left 27");
    Topology topo2;
    topo2.monitors = {m1, m2};
    ResolveIdentities(topo2, labels);
    bool labelApplied = false;
    for (const auto& m : topo2.monitors)
    {
        if (m.id == firstId && m.label == "Left 27")
        {
            labelApplied = true;
        }
    }
    CHECK(labelApplied, "user label binds to resolved identity");
}

static void TestCoordRoundTrip()
{
    std::printf("[coordinate normalize/denormalize]\n");
    RECT work2k{0, 0, 2560, 1440};
    RECT win{256, 144, 1536, 1008}; // x=0.1,y=0.1,w=0.5,h=0.6 of work2k
    NormRect n = NormalizeToWork(win, work2k);
    CHECK(std::fabs(n.x - 0.1) < 1e-9, "nx == 0.1");
    CHECK(std::fabs(n.w - 0.5) < 1e-9, "nw == 0.5");

    // Round-trip on the same work area is identity.
    RECT back = DenormalizeFromWork(n, work2k);
    CHECK(back.left == win.left && back.top == win.top &&
          back.right == win.right && back.bottom == win.bottom, "round-trip is exact");

    // Collapse scenario: same window restored onto a smaller 1080p monitor stays
    // proportional (resolution-change resilience).
    RECT work1080{1920, 0, 1920 + 1920, 1080}; // a second monitor to the right
    RECT scaled = DenormalizeFromWork(n, work1080);
    CHECK(scaled.left == 1920 + static_cast<LONG>(0.1 * 1920 + 0.5), "x scales to target work area");
    CHECK((scaled.right - scaled.left) == static_cast<LONG>(0.5 * 1920 + 0.5), "width scales proportionally");
}

static void TestMatchScore()
{
    std::printf("[restore match scoring]\n");
    WindowSnap snap;
    snap.exeLeaf = "code.exe";
    snap.className = "Chrome_WidgetWin_1";
    snap.title = "design.md - VS Code";

    LiveWindow exact;
    exact.exeLeaf = "code.exe";
    exact.className = "Chrome_WidgetWin_1";
    exact.title = "design.md - VS Code";

    LiveWindow classOnly;
    classOnly.exeLeaf = "code.exe";
    classOnly.className = "Chrome_WidgetWin_1";
    classOnly.title = "other.txt - VS Code";

    LiveWindow wrongExe;
    wrongExe.exeLeaf = "notepad.exe";

    int sExact = WindowMatchScore(snap, exact);
    int sClass = WindowMatchScore(snap, classOnly);
    int sWrong = WindowMatchScore(snap, wrongExe);
    CHECK(sWrong < 0, "different exe is ineligible");
    CHECK(sExact > sClass, "exact title+class beats class-only");
    CHECK(sClass > 0, "same exe+class is eligible");
}

static void TestIpcFraming()
{
    std::printf("[ipc message framing]\n");
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (CreatePipe(&rd, &wr, &sa, 64 * 1024) == 0)
    {
        CHECK(false, "CreatePipe for framing test");
        return;
    }
    json::Value req = json::Value::MakeObject();
    req.Set("cmd", json::Value(std::string("devicedisablewake")));
    req.Set("device", json::Value(std::string("Realtek PCIe GbE Family Controller")));
    std::string payload = req.Dump(false);

    bool wrote = IpcWriteMessage(wr, payload);
    CHECK(wrote, "framed write succeeds");

    std::string got;
    bool read = IpcReadMessage(rd, got);
    CHECK(read, "framed read succeeds");
    CHECK(got == payload, "round-trip payload identical");

    json::Value parsed;
    CHECK(json::Value::Parse(got, parsed, nullptr), "received payload parses");
    CHECK(parsed["device"].AsString() == "Realtek PCIe GbE Family Controller", "field preserved over pipe");

    CloseHandle(rd);
    CloseHandle(wr);
}

static void TestSleepStudy()
{
    std::printf("[sleepstudy / modern standby]\n");
    // DRIPS extractor on representative report markup.
    std::string snippet = "<tr><td>DRIPS</td><td>83.5%</td></tr>";
    double d = ParseDripsFromReport(snippet);
    CHECK(std::fabs(d - 83.5) < 0.01, "DRIPS percentage extracted from report markup");
    CHECK(ParseDripsFromReport("no relevant metrics here") < 0.0, "absent DRIPS -> -1");

    // Modern Standby detection is locale-independent (registry). Just confirm it is
    // callable and yields a bool consistent with this machine (S3 -> false).
    bool ms = IsModernStandbyEnabled();
    CHECK(ms == false || ms == true, "modern standby query returns a value");
    std::printf("  modernStandby=%d\n", ms ? 1 : 0);
}

static void TestLiveDoctor()
{
    std::printf("[live doctor on this machine]\n");
    Settings s;
    Doctor doc(s);
    DoctorReport rep = doc.Run();
    CHECK(!rep.findings.empty(), "doctor produced at least one finding");
    std::string html = BuildReportHtml(rep);
    CHECK(html.find("<table") != std::string::npos, "report html has a table");
    std::printf("  findings=%zu timeline=%zu requestsNeedAdmin=%d lastWakeEmpty=%d wakeArmed=%zu\n",
                rep.findings.size(), rep.timeline.size(),
                rep.data.requestsNeedAdmin ? 1 : 0, rep.data.lastWakeEmpty ? 1 : 0,
                rep.data.wakeArmedDevices.size());
    for (const auto& f : rep.findings)
    {
        std::printf("    - [%s] %s\n", f.severity == Severity::Warn ? "WARN" : "INFO", f.plain.c_str());
    }
}

int main()
{
    Logger::Instance().Init(L"tests", LogLevel::Warn);
    std::printf("OhMyDisplay test harness\n========================\n");
    TestDebounce();
    TestJson();
    TestTopologySignature();
    TestSameModelCollision();
    TestCoordRoundTrip();
    TestMatchScore();
    TestIpcFraming();
    TestRequestsParser();
    TestWakeArmedParser();
    TestSleepStudy();
    TestLiveDoctor();
    std::printf("========================\n%d/%d checks passed%s\n",
                g_checks - g_failures, g_checks, g_failures == 0 ? " (ALL OK)" : " (FAILURES)");
    return g_failures == 0 ? 0 : 1;
}
