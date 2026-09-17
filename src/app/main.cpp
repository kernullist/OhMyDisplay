// OhMyDisplay - main application entry point and wiring.
//
// Non-elevated (asInvoker). Owns the hidden hub window, routes Core signals to the
// Keeper, and exposes Keeper/Doctor actions through the tray menu and hotkeys.
#include "core/message_window.h"
#include "core/power_notify.h"
#include "core/event_bus.h"
#include "core/settings.h"
#include "core/paths.h"
#include "core/logger.h"
#include "core/common.h"
#include "keeper/keeper.h"
#include "keeper/monitor_labels.h"
#include "doctor/doctor.h"
#include "doctor/report.h"
#include "shell/tray.h"
#include "shell/hotkeys.h"
#include "shell/label_ui.h"
#include "shell/report_window.h"
#include "app/resource.h"

#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

using namespace omd;

namespace
{
    const UINT OMD_MSG_NOTIFY = WM_APP + 10; // lParam: std::string* (balloon text)
    const UINT OMD_MSG_OPEN_LABELS = WM_APP + 11;
    const UINT OMD_MSG_SECOND_INSTANCE = WM_APP + 12;

    class JobGate
    {
    public:
        void Run(std::function<void()> fn)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_.load())
                {
                    return;
                }
                inflight_.fetch_add(1);
            }
            try
            {
                std::thread([this, work = std::move(fn)]()
                {
                    try
                    {
                        work();
                    }
                    catch (...)
                    {
                    }
                    inflight_.fetch_sub(1);
                    cv_.notify_all();
                }).detach();
            }
            catch (...)
            {
                inflight_.fetch_sub(1);
                cv_.notify_all();
            }
        }

        void Shutdown()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_.store(true);
            }
            // Must wait until every job that captured &app has finished. Timing out
            // here and then destroying App is a use-after-free (UAC doctor, restore).
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return inflight_.load() == 0; });
        }

    private:
        std::atomic<int> inflight_{0};
        std::atomic<bool> stopping_{false};
        std::mutex mutex_;
        std::condition_variable cv_;
    };

    struct App
    {
        HINSTANCE hInst = nullptr;
        MessageWindow hub;
        PowerNotify power;
        EventBus bus;
        Settings settings;
        Tray tray;
        Hotkeys hotkeys;
        std::unique_ptr<Keeper> keeper;
        std::unique_ptr<Doctor> doctor;
        HICON icon = nullptr;
        JobGate jobs;

        bool labelEditorOpen = false;

        void PostNotify(const std::string& text)
        {
            auto* s = new std::string(text);
            if (PostMessageW(hub.Handle(), OMD_MSG_NOTIFY, 0, reinterpret_cast<LPARAM>(s)) == 0)
            {
                delete s;
            }
        }
    };

    void OpenLabelEditor(App& app)
    {
        if (app.labelEditorOpen)
        {
            return;
        }
        Topology topo = app.keeper->CurrentResolvedTopology();
        if (topo.Empty())
        {
            return;
        }
        struct EditorGuard
        {
            bool& flag;
            explicit EditorGuard(bool& f) : flag(f)
            {
                flag = true;
            }
            ~EditorGuard()
            {
                flag = false;
            }
        } editorGuard(app.labelEditorOpen);
        // Mark same-model collisions (monitors sharing a base identity).
        std::map<std::string, int> baseCount;
        for (const auto& m : topo.monitors)
        {
            baseCount[m.baseId]++;
        }

        std::vector<LabelRow> rows;
        for (const auto& m : topo.monitors)
        {
            LabelRow r;
            r.id = m.id;
            r.friendlyName = m.friendlyName;
            r.current = m.label;
            r.collision = baseCount[m.baseId] > 1;
            rows.push_back(std::move(r));
        }

        if (ShowLabelEditor(app.hub.Handle(), app.hInst, rows))
        {
            for (const auto& r : rows)
            {
                if (r.edited != r.current)
                {
                    app.keeper->SetMonitorLabel(r.id, r.edited);
                }
            }
            app.tray.ShowBalloon("OhMyDisplay", "Monitor labels updated");
        }
    }

    void ShowGhosts(App& app)
    {
        std::vector<GhostMonitor> ghosts = app.keeper->Ghosts();
        std::wstring text;
        if (ghosts.empty())
        {
            text = L"No ghost (phantom) monitors detected.";
        }
        else
        {
            text = L"Detected phantom monitor entries the system still remembers:\n\n";
            for (const auto& g : ghosts)
            {
                text += L"  - " + WideFromUtf8(g.friendlyName.empty() ? g.devicePath : g.friendlyName) + L"\n";
            }
            text += L"\nYou can remove them in Device Manager "
                    L"(View > Show hidden devices > Display adapters/Monitors).";
        }
        MessageBoxW(app.hub.Handle(), text.c_str(), L"OhMyDisplay - Ghost monitors",
                    MB_OK | MB_ICONINFORMATION);
    }

    void WireTray(App& app)
    {
        app.tray.onSave = [&app]
        {
            app.jobs.Run([&app] { app.keeper->SaveNow(); });
        };
        app.tray.onRestore = [&app]
        {
            app.jobs.Run([&app] { app.keeper->RestoreNow(); });
        };
        app.tray.onDoctor = [&app]
        {
            app.PostNotify("Running Sleep Doctor...");
            app.jobs.Run([&app]
            {
                DoctorReport rep = app.doctor->Run(false);
                WriteHtmlReport(rep);   // persisted for support bundles
                ShowReportWindow(rep, app.settings.doctor.useWebView2);
            });
        };
        app.tray.onDoctorAdmin = [&app]
        {
            app.PostNotify("Running Sleep Doctor (admin)...");
            app.jobs.Run([&app]
            {
                DoctorReport rep = app.doctor->Run(true);
                WriteHtmlReport(rep);
                ShowReportWindow(rep, app.settings.doctor.useWebView2);
            });
        };
        app.tray.onLabels = [&app]
        {
            OpenLabelEditor(app); // UI-thread (menu command)
        };
        app.tray.onGhosts = [&app]
        {
            ShowGhosts(app);
        };
        app.tray.onToggleKeeper = [&app]
        {
            app.settings.keeper.enabled = !app.settings.keeper.enabled;
            app.settings.Save();
            app.tray.SetKeeperEnabled(app.settings.keeper.enabled);
            app.PostNotify(app.settings.keeper.enabled ? "Layout Keeper enabled"
                                                       : "Layout Keeper disabled");
        };
        app.tray.onOpenFolder = [&app]
        {
            std::wstring dir = AppDataDir();
            if (!dir.empty())
            {
                ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        };
        app.tray.onExit = []
        {
            PostQuitMessage(0);
        };
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // Single-instance guard. A second launch tells the running copy to balloon
    // instead of exiting silently.
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\OhMyDisplay.SingleInstance");
    if (singleton != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = nullptr;
        for (int i = 0; i < 40 && existing == nullptr; ++i)
        {
            existing = FindWindowW(MessageWindow::ClassName(), L"OhMyDisplay");
            if (existing == nullptr)
            {
                Sleep(50);
            }
        }
        if (existing != nullptr)
        {
            PostMessageW(existing, OMD_MSG_SECOND_INSTANCE, 0, 0);
        }
        CloseHandle(singleton);
        return 0;
    }

    Logger::Instance().Init(L"main", LogLevel::Info);
    OMD_LOGI("OhMyDisplay starting (v0.3)");

    App app;
    app.hInst = hInstance;
    app.icon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    app.settings.Load();

    if (!app.hub.Create())
    {
        OMD_LOGE("fatal: hub window creation failed");
        return 1;
    }

    app.keeper = std::make_unique<Keeper>(app.settings);
    app.doctor = std::make_unique<Doctor>(app.settings);

    // Keeper -> shell hooks (marshalled to the UI thread via PostMessage).
    app.keeper->onNotify = [&app](const std::string& s)
    {
        app.PostNotify(s);
    };
    app.keeper->onCollision = [&app](const std::string& s)
    {
        app.PostNotify(s);
        PostMessageW(app.hub.Handle(), OMD_MSG_OPEN_LABELS, 0, 0);
    };

    app.keeper->Init();

    // Core signals -> Keeper topology evaluation.
    app.bus.Subscribe(CoreEvent::DisplayChanged, [&app](const EventArgs&)
    {
        app.keeper->OnTopologyEvent();
    });
    app.bus.Subscribe(CoreEvent::PowerResume, [&app](const EventArgs&)
    {
        app.keeper->OnTopologyEvent();
    });
    app.bus.Subscribe(CoreEvent::DisplayStateOn, [&app](const EventArgs&)
    {
        app.keeper->OnTopologyEvent();
    });

    // WM_DISPLAYCHANGE on the hub -> bus.
    app.hub.AddHandler(WM_DISPLAYCHANGE, [&app](HWND, UINT, WPARAM, LPARAM, LRESULT& result) -> bool
    {
        app.bus.Post(CoreEvent::DisplayChanged);
        result = 0;
        return true;
    });

    // Balloon notifications marshalled from worker threads.
    app.hub.AddHandler(OMD_MSG_NOTIFY, [&app](HWND, UINT, WPARAM, LPARAM lp, LRESULT& result) -> bool
    {
        std::string* s = reinterpret_cast<std::string*>(lp);
        if (s != nullptr)
        {
            app.tray.ShowBalloon("OhMyDisplay", *s);
            delete s;
        }
        result = 0;
        return true;
    });

    app.hub.AddHandler(OMD_MSG_OPEN_LABELS, [&app](HWND, UINT, WPARAM, LPARAM, LRESULT& result) -> bool
    {
        OpenLabelEditor(app);
        result = 0;
        return true;
    });

    app.hub.AddHandler(OMD_MSG_SECOND_INSTANCE, [&app](HWND, UINT, WPARAM, LPARAM, LRESULT& result) -> bool
    {
        app.tray.ShowBalloon("OhMyDisplay", "Already running. Right-click the tray icon.");
        result = 0;
        return true;
    });

    app.power.Register(app.hub, app.bus);

    if (!app.tray.Create(app.hub, app.icon))
    {
        OMD_LOGW("tray: icon add failed (continuing headless)");
    }
    app.tray.SetKeeperEnabled(app.settings.keeper.enabled);
    WireTray(app);

    app.hotkeys.Init(app.hub);
    app.hotkeys.Register(app.settings.hotkeys.saveNow, [&app]
    {
        app.jobs.Run([&app] { app.keeper->SaveNow(); });
    });
    app.hotkeys.Register(app.settings.hotkeys.restoreNow, [&app]
    {
        app.jobs.Run([&app] { app.keeper->RestoreNow(); });
    });

    // Capture (or restore) the current topology shortly after startup so the first
    // sleep/resume cycle has a snapshot even if no DisplayChanged has fired yet.
    app.keeper->OnTopologyEvent();

    app.tray.ShowBalloon("OhMyDisplay", "Running in the tray. Right-click the icon for options.");

    // Message loop.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OMD_LOGI("OhMyDisplay shutting down");
    CloseReportWindows();
    app.jobs.Shutdown();
    app.hotkeys.Shutdown();
    app.power.Unregister();
    app.keeper->Shutdown();
    MSG drain;
    while (PeekMessageW(&drain, app.hub.Handle(), OMD_MSG_NOTIFY, OMD_MSG_NOTIFY, PM_REMOVE) != 0)
    {
        delete reinterpret_cast<std::string*>(drain.lParam);
    }
    app.tray.Destroy();
    app.hub.Destroy();

    if (singleton != nullptr)
    {
        CloseHandle(singleton);
    }
    return 0;
}
