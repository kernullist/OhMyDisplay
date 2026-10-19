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
#include "core/process_util.h"
#include "doctor/powercfg_collector.h"
#include "doctor/doctor.h"
#include "doctor/report.h"
#include "keeper/layout_store.h"

#include <dwmapi.h>
#include <atomic>
#include <chrono>

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

    Topology unlabeled = topo;
    for (auto& m : unlabeled.monitors)
    {
        m.label.clear();
    }
    CHECK(HasUnlabeledCollision(unlabeled), "collision without labels needs a prompt");

    labels.SetLabel(topo.monitors[0].id, "Left 27");
    labels.SetLabel(topo.monitors[1].id, "Right 27");
    Topology bothLabeled;
    bothLabeled.monitors = {m1, m2};
    ResolveIdentities(bothLabeled, labels);
    CHECK(!HasUnlabeledCollision(bothLabeled), "fully labeled collision should not prompt");

    Topology unique;
    MonitorInfo only = m1;
    only.baseId = "m_solo";
    only.id = "m_solo";
    unique.monitors = {only};
    CHECK(!HasUnlabeledCollision(unique), "single monitor is not a collision");
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

    // Snapped/maximized windows hang their invisible border past the work area, so
    // the normalized offset is negative and must still round-trip exactly.
    RECT work2{2560, 0, 5120, 1392};
    RECT snapped{2560 - 7, 0, 2560 + 1287, 1399};
    RECT snapBack = DenormalizeFromWork(NormalizeToWork(snapped, work2), work2);
    CHECK(EqualRect(&snapBack, &snapped) != 0, "negative offset (snapped edge) round-trips exactly");
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

    LiveWindow sameHwnd;
    sameHwnd.exeLeaf = "code.exe";
    sameHwnd.className = "Chrome_WidgetWin_1";
    sameHwnd.title = "other.txt - VS Code";
    sameHwnd.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    sameHwnd.pid = 4242;
    WindowSnap snapHwnd = snap;
    snapHwnd.hwnd = sameHwnd.hwnd;
    snapHwnd.pid = 4242;
    CHECK(WindowMatchScore(snapHwnd, sameHwnd) > sExact,
          "same HWND+pid outranks title+class on a different handle");
    LiveWindow recycledHwnd = sameHwnd;
    recycledHwnd.pid = 99;
    CHECK(WindowMatchScore(snapHwnd, recycledHwnd) < WindowMatchScore(snapHwnd, sameHwnd),
          "recycled HWND in another process does not get the HWND bonus");
}

static void TestMatchAssignment()
{
    std::printf("[restore match assignment]\n");
    WindowSnap weak;
    weak.exeLeaf = "chrome.exe";
    weak.className = "Chrome_WidgetWin_1";
    weak.title = "";
    WindowSnap strong;
    strong.exeLeaf = "chrome.exe";
    strong.className = "Chrome_WidgetWin_1";
    strong.title = "Gmail";

    LiveWindow gmail;
    gmail.exeLeaf = "chrome.exe";
    gmail.className = "Chrome_WidgetWin_1";
    gmail.title = "Gmail";
    LiveWindow other;
    other.exeLeaf = "chrome.exe";
    other.className = "Chrome_WidgetWin_1";
    other.title = "Other";

    std::vector<WindowSnap> snaps = {weak, strong};
    std::vector<LiveWindow> live = {gmail, other};
    std::vector<int> assigned = AssignRestoreMatches(snaps, live);
    CHECK(assigned.size() == 2, "assignment covers both snaps");
    CHECK(assigned[1] == 0, "titled snap claims the Gmail window");
    CHECK(assigned[0] == 1, "untitled snap takes the leftover window");
}

static void TestCollapseDetection()
{
    std::printf("[collapse detection]\n");
    WindowSnap a;
    a.exeLeaf = "a.exe";
    a.className = "A";
    a.title = "A";
    a.monitorId = "m_left";
    WindowSnap b = a;
    b.exeLeaf = "b.exe";
    b.title = "B";
    b.className = "B";
    b.monitorId = "m_right";
    WindowSnap c = a;
    c.exeLeaf = "c.exe";
    c.title = "C";
    c.className = "C";
    c.monitorId = "m_left";
    WindowSnap d = a;
    d.exeLeaf = "d.exe";
    d.title = "D";
    d.className = "D";
    d.monitorId = "m_right";

    std::vector<WindowSnap> saved = {a, b, c, d};

    std::vector<WindowSnap> stable = saved;
    CHECK(!LayoutLooksCollapsed(saved, stable), "identical placement is not a collapse");

    std::vector<WindowSnap> oneMoved = saved;
    oneMoved[1].monitorId = "m_left"; // user dragged one window
    CHECK(!LayoutLooksCollapsed(saved, oneMoved), "one of four windows moved is not a collapse");

    std::vector<WindowSnap> piled = saved;
    piled[1].monitorId = "m_left";
    piled[3].monitorId = "m_left"; // half fled
    CHECK(LayoutLooksCollapsed(saved, piled), "half the windows on the wrong monitor is a collapse");

    std::vector<WindowSnap> empty;
    CHECK(!LayoutLooksCollapsed(saved, empty), "empty current capture is not a collapse");
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

static void TestLastWakeParser()
{
    std::printf("[powercfg /lastwake parser]\n");
    std::string type;
    std::string name;
    bool empty = false;
    ParseLastWake("Wake History Count - 0\r\n", type, name, empty);
    CHECK(empty, "zero-source lastwake is empty");

    ParseLastWake(
        "Wake History Count - 1\r\n"
        "Wake History [0]\r\n"
        "  Wake Source Count - 1\r\n"
        "  Wake Source [0]\n"
        "    Type: Device\r\n"
        "    Instance Path: PCI\\VEN_10EC\r\n"
        "    Friendly Name: Realtek PCIe GbE Family Controller\r\n",
        type, name, empty);
    CHECK(!empty, "named lastwake is not empty");
    CHECK(type == "Device", "lastwake type parsed");
    CHECK(name.find("Realtek") != std::string::npos, "lastwake friendly name parsed");
}

static void TestWakeTimersParser()
{
    std::printf("[powercfg /waketimers parser]\n");
    bool needAdmin = false;
    auto none = ParseWakeTimers("There are no active wake timers in the system.\r\n", needAdmin);
    CHECK(none.empty(), "no active wake timers -> empty");
    CHECK(!needAdmin, "plain empty is not an admin failure");

    const char* sample =
        "Timer set by [SERVICE] \\Device\\HarddiskVolume3\\Windows\\System32\\svchost.exe (SystemEventsBroker) expires at 14:00:00.\r\n"
        "  Reason: Windows will execute '\\Microsoft\\Windows\\UpdateOrchestrator\\Reboot' scheduled task.\r\n";
    auto timers = ParseWakeTimers(sample, needAdmin);
    CHECK(timers.size() == 1, "one wake timer parsed");
    if (!timers.empty())
    {
        CHECK(timers[0].owner.find("svchost.exe") != std::string::npos, "timer owner captured");
        CHECK(timers[0].reason.find("UpdateOrchestrator") != std::string::npos, "timer reason captured");
    }
}

static void TestRunCaptureTimeout()
{
    std::printf("[process timeout]\n");
    ULONGLONG t0 = GetTickCount64();
    ProcessResult r = RunCapture(L"cmd.exe /c ping -n 20 127.0.0.1 >nul", 800);
    ULONGLONG dt = GetTickCount64() - t0;
    CHECK(r.launched, "timeout test process launched");
    CHECK(dt < 5000, "hung child is killed within 5s (was unbounded ReadFile)");
}

static void TestLiveTopology()
{
    std::printf("[live topology query]\n");
    Topology t;
    CHECK(QueryActiveTopology(t), "QueryActiveTopology succeeds on this machine");
    CHECK(t.Count() >= 1, "at least one monitor");
    CHECK(!t.signature.empty(), "signature is set");
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

static void TestDebounceSlidesForwardOnly()
{
    std::printf("[debounce never shortens a pending window]\n");
    std::atomic<int> fired{0};
    auto start = std::chrono::steady_clock::now();
    std::atomic<long long> firedAtMs{0};
    Debouncer d([&]
    {
        firedAtMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
        fired.fetch_add(1);
    });
    DebounceConfig cfg; // 5 monitors -> 1500ms, 1 monitor -> 550ms
    d.SetConfig(cfg);
    d.Start();
    d.Trigger(5);
    d.Trigger(1); // a momentary dip to one monitor during wake
    Sleep(2200);
    d.Stop();
    CHECK(fired.load() == 1, "burst fires exactly once");
    CHECK(firedAtMs.load() >= 1400, "later short trigger did not pull the deadline in");
}

static void TestSettingsMigration()
{
    std::printf("[settings migration]\n");
    Settings fresh;
    CHECK(fresh.keeper.excludeExe.empty(), "File Explorer windows are not excluded by default");

    json::Value v1;
    json::Value::Parse("{\"keeper\":{\"excludeExe\":[\"explorer.exe\"]}}", v1, nullptr);
    Settings old;
    old.FromJson(v1);
    CHECK(old.keeper.excludeExe.empty(), "v1 explorer.exe default is dropped");

    json::Value v2;
    json::Value::Parse("{\"settingsVersion\":2,\"keeper\":{\"excludeExe\":[\"explorer.exe\"]}}", v2, nullptr);
    Settings chosen;
    chosen.FromJson(v2);
    CHECK(chosen.keeper.excludeExe.size() == 1, "explicit v2 exclusion is kept");
}

namespace
{
    HWND MakeTestWindow(const wchar_t* title, DWORD style, DWORD ex, HWND owner, int x, int y)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"OmdTestWnd";
            RegisterClassW(&wc);
            registered = true;
        }
        return CreateWindowExW(ex, L"OmdTestWnd", title, style, x, y, 320, 240, owner, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    }

    void PumpFor(DWORD ms)
    {
        DWORD until = GetTickCount() + ms;
        MSG m;
        while (GetTickCount() < until)
        {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE) != 0)
            {
                DispatchMessageW(&m);
            }
            Sleep(10);
        }
    }
}

static void TestWindowFilter()
{
    std::printf("[window filter: taskbar-visible set]\n");
    const DWORD ov = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    HWND hiddenOwner = MakeTestWindow(L"omd-hidden-owner", WS_OVERLAPPEDWINDOW, 0, nullptr, 0, 0);
    HWND ownedA = MakeTestWindow(L"omd-owned-a", ov, 0, hiddenOwner, 120, 120);
    HWND ownedB = MakeTestWindow(L"omd-owned-b", ov, 0, hiddenOwner, 140, 140);
    HWND mainWnd = MakeTestWindow(L"omd-main", ov, 0, nullptr, 160, 160);
    HWND dialog = MakeTestWindow(L"omd-dialog", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                 WS_EX_DLGMODALFRAME, mainWnd, 180, 180);
    HWND ownedApp = MakeTestWindow(L"omd-owned-app", ov, WS_EX_APPWINDOW, mainWnd, 200, 200);
    HWND toolApp = MakeTestWindow(L"omd-tool-app", ov, WS_EX_TOOLWINDOW | WS_EX_APPWINDOW, nullptr, 220, 220);
    HWND tool = MakeTestWindow(L"omd-tool", ov, WS_EX_TOOLWINDOW, nullptr, 240, 240);
    HWND overlay = MakeTestWindow(L"omd-overlay", WS_POPUP | WS_VISIBLE,
                                  WS_EX_NOACTIVATE | WS_EX_TOPMOST, nullptr, 260, 260);
    HWND zero = CreateWindowExW(0, L"OmdTestWnd", L"omd-zero", WS_POPUP | WS_VISIBLE, 0, 0, 0, 0,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND minimized = MakeTestWindow(L"omd-min", ov, 0, nullptr, 280, 280);
    ShowWindow(minimized, SW_SHOWMINNOACTIVE);
    HWND cloaked = MakeTestWindow(L"omd-cloaked", ov, 0, nullptr, 300, 300);
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(cloaked, DWMWA_CLOAK, &cloak, sizeof(cloak));
    PumpFor(300);

    CHECK(IsManageableWindow(ownedA) && IsManageableWindow(ownedB),
          "windows owned by a hidden owner (WinForms/Delphi) are kept");
    CHECK(IsManageableWindow(mainWnd), "main window with an open dialog is kept");
    CHECK(!IsManageableWindow(dialog), "owned dialog is not saved separately");
    CHECK(IsManageableWindow(ownedApp), "owned WS_EX_APPWINDOW window is kept");
    CHECK(IsManageableWindow(toolApp), "tool window forced onto the taskbar is kept");
    CHECK(!IsManageableWindow(tool), "plain tool window is skipped");
    CHECK(!IsManageableWindow(overlay), "no-activate overlay is skipped");
    CHECK(!IsManageableWindow(zero), "zero-size helper is skipped");
    CHECK(IsManageableWindow(minimized), "minimized window is kept");
    DWORD cloakState = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(cloaked, DWMWA_CLOAKED, &cloakState, sizeof(cloakState))) &&
        cloakState != 0)
    {
        CHECK(!IsManageableWindow(cloaked), "app-cloaked window is skipped");
    }

    HWND all[] = {ownedA, ownedB, dialog, ownedApp, mainWnd, hiddenOwner, toolApp, tool, overlay, zero,
                  minimized, cloaked};
    for (HWND h : all)
    {
        DestroyWindow(h);
    }
}

static void TestRestoreRoundTrip()
{
    std::printf("[restore round-trip on live windows]\n");
    const DWORD ov = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    HWND normal = MakeTestWindow(L"omd-rt-normal", ov, 0, nullptr, 100, 110);
    HWND toolApp = MakeTestWindow(L"omd-rt-toolapp", ov, WS_EX_TOOLWINDOW | WS_EX_APPWINDOW, nullptr, 150, 160);
    HWND minimized = MakeTestWindow(L"omd-rt-min", ov, 0, nullptr, 200, 210);
    ShowWindow(minimized, SW_SHOWMINNOACTIVE);
    PumpFor(200);

    RECT wantNormal{};
    RECT wantTool{};
    GetWindowRect(normal, &wantNormal);
    GetWindowRect(toolApp, &wantTool);

    Topology topo;
    bool haveTopo = QueryActiveTopology(topo);
    CHECK(haveTopo, "live topology available");
    KeeperConfig cfg;
    StoredTopology stored;
    DWORD me = GetCurrentProcessId();
    for (auto& w : CaptureWindows(topo, cfg))
    {
        if (w.pid == me)
        {
            w.hwnd = nullptr; // force title/class matching, as after a reboot
            stored.windows.push_back(w);
        }
    }
    CHECK(stored.windows.size() == 3, "all three test windows captured");

    SetWindowPos(normal, nullptr, 700, 500, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(toolApp, nullptr, 650, 450, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(minimized, SW_SHOWNOACTIVATE);
    PumpFor(100);

    RestoreEngine engine(cfg);
    RestoreResult res = engine.Restore(stored, topo);
    PumpFor(100);
    RECT gotNormal{};
    RECT gotTool{};
    GetWindowRect(normal, &gotNormal);
    GetWindowRect(toolApp, &gotTool);
    CHECK(res.restored == 3, "restore reports all three windows");
    CHECK(EqualRect(&gotNormal, &wantNormal) != 0, "normal window back at its saved rect");
    CHECK(EqualRect(&gotTool, &wantTool) != 0, "tool+appwindow (screen-coord placement) back at its rect");
    CHECK(IsIconic(minimized) != 0, "minimized window minimized again");

    DestroyWindow(normal);
    DestroyWindow(toolApp);
    DestroyWindow(minimized);
}

static void TestIdenticalTitleTieBreak()
{
    std::printf("[identical-title tie-break]\n");
    // Seven Sticky Notes share exe/class/title; only position tells them apart.
    auto note = [](const char* mon, double x, double y)
    {
        WindowSnap s;
        s.exeLeaf = "applicationframehost.exe";
        s.className = "ApplicationFrameWindow";
        s.title = "Sticky Notes";
        s.monitorId = mon;
        s.nx = x;
        s.ny = y;
        return s;
    };
    auto liveNote = [](const char* mon, double x, double y)
    {
        LiveWindow l;
        l.exeLeaf = "applicationframehost.exe";
        l.className = "ApplicationFrameWindow";
        l.title = "Sticky Notes";
        l.hasGeometry = true;
        l.monitorId = mon;
        l.nx = x;
        l.ny = y;
        return l;
    };
    std::vector<WindowSnap> snaps = {note("m_a", 0.10, 0.10), note("m_a", 0.60, 0.40), note("m_b", 0.30, 0.30)};
    // Live order differs from capture order; positions drifted slightly.
    std::vector<LiveWindow> live = {liveNote("m_b", 0.31, 0.29), liveNote("m_a", 0.12, 0.11),
                                    liveNote("m_a", 0.58, 0.41)};
    std::vector<int> a = AssignRestoreMatches(snaps, live);
    CHECK(a.size() == 3 && a[0] == 1 && a[1] == 2 && a[2] == 0,
          "identical windows pair with their nearest live counterpart");

    // Same monitor beats a nearer position on another monitor.
    std::vector<WindowSnap> one = {note("m_a", 0.50, 0.50)};
    std::vector<LiveWindow> two = {liveNote("m_b", 0.50, 0.50), liveNote("m_a", 0.90, 0.90)};
    std::vector<int> b = AssignRestoreMatches(one, two);
    CHECK(b.size() == 1 && b[0] == 1, "same-monitor candidate preferred");

    // Title still outranks geometry.
    WindowSnap titled = note("m_a", 0.10, 0.10);
    titled.title = "Groceries";
    LiveWindow nearUntitled = liveNote("m_a", 0.10, 0.10);
    LiveWindow farTitled = liveNote("m_b", 0.90, 0.90);
    farTitled.title = "Groceries";
    std::vector<int> c = AssignRestoreMatches({titled}, {nearUntitled, farTitled});
    CHECK(c.size() == 1 && c[0] == 1, "exact title beats geometric closeness");

    // Without geometry the assignment is deterministic: capture order -> enumeration order.
    std::vector<LiveWindow> blind(3, LiveWindow{});
    for (auto& l : blind)
    {
        l.exeLeaf = "applicationframehost.exe";
        l.className = "ApplicationFrameWindow";
        l.title = "Sticky Notes";
    }
    std::vector<int> d = AssignRestoreMatches(snaps, blind);
    CHECK(d.size() == 3 && d[0] == 0 && d[1] == 1 && d[2] == 2, "no geometry -> stable order pairing");
}

static void TestIdenticalTitleRestoreLive()
{
    std::printf("[identical-title restore on live windows]\n");
    const DWORD ov = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    HWND w[3] = {MakeTestWindow(L"omd-same-title", ov, 0, nullptr, 100, 100),
                 MakeTestWindow(L"omd-same-title", ov, 0, nullptr, 500, 140),
                 MakeTestWindow(L"omd-same-title", ov, 0, nullptr, 900, 180)};
    PumpFor(200);
    RECT want[3];
    for (int i = 0; i < 3; ++i)
    {
        GetWindowRect(w[i], &want[i]);
    }
    Topology topo;
    QueryActiveTopology(topo);
    KeeperConfig cfg;
    StoredTopology stored;
    DWORD me = GetCurrentProcessId();
    for (auto& s : CaptureWindows(topo, cfg))
    {
        if (s.pid == me && s.title == "omd-same-title")
        {
            s.hwnd = nullptr; // as after a reboot: only exe/class/title + geometry remain
            stored.windows.push_back(s);
        }
    }
    CHECK(stored.windows.size() == 3, "three same-titled windows captured");

    // Each drifts a little (an app reopening its windows near where they were) and the
    // z-order is shuffled so enumeration order no longer matches capture order.
    for (int i = 0; i < 3; ++i)
    {
        SetWindowPos(w[i], HWND_TOP, want[i].left + 30, want[i].top + 20, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
    PumpFor(100);
    RestoreEngine engine(cfg);
    engine.Restore(stored, topo);
    PumpFor(100);
    bool allHome = true;
    for (int i = 0; i < 3; ++i)
    {
        RECT got{};
        GetWindowRect(w[i], &got);
        allHome = allHome && EqualRect(&got, &want[i]) != 0;
        DestroyWindow(w[i]);
    }
    CHECK(allHome, "each identical window returns to its own saved rect");
}

int main()
{
    Logger::Instance().Init(L"tests", LogLevel::Warn);
    std::printf("OhMyDisplay test harness\n========================\n");
    TestDebounce();
    TestDebounceSlidesForwardOnly();
    TestSettingsMigration();
    TestJson();
    TestTopologySignature();
    TestSameModelCollision();
    TestCoordRoundTrip();
    TestMatchScore();
    TestMatchAssignment();
    TestCollapseDetection();
    TestWindowFilter();
    TestRestoreRoundTrip();
    TestIdenticalTitleTieBreak();
    TestIdenticalTitleRestoreLive();
    TestIpcFraming();
    TestRequestsParser();
    TestWakeArmedParser();
    TestLastWakeParser();
    TestWakeTimersParser();
    TestRunCaptureTimeout();
    TestLiveTopology();
    TestSleepStudy();
    TestLiveDoctor();
    std::printf("========================\n%d/%d checks passed%s\n",
                g_checks - g_failures, g_checks, g_failures == 0 ? " (ALL OK)" : " (FAILURES)");
    return g_failures == 0 ? 0 : 1;
}
