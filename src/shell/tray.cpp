// OhMyDisplay - tray icon implementation.
#include "shell/tray.h"
#include "core/common.h"
#include "core/logger.h"

#include <shellapi.h>

namespace omd
{
    namespace
    {
        const UINT kTrayCallback = WM_APP + 1;
        const UINT kTrayUid = 1;

        enum MenuId : int
        {
            IDM_SAVE = 1001,
            IDM_RESTORE,
            IDM_DOCTOR,
            IDM_DOCTOR_ADMIN,
            IDM_LABELS,
            IDM_GHOSTS,
            IDM_TOGGLE_KEEPER,
            IDM_OPENFOLDER,
            IDM_EXIT
        };
    }

    bool Tray::Create(MessageWindow& window, HICON icon)
    {
        hwnd_ = window.Handle();
        if (hwnd_ == nullptr)
        {
            return false;
        }
        // Explorer broadcasts this when the taskbar (re)starts; we re-add the icon.
        taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

        ZeroMemory(&nid_, sizeof(nid_));
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = kTrayUid;
        nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid_.uCallbackMessage = kTrayCallback;
        nid_.hIcon = icon != nullptr ? icon : LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid_.szTip, L"OhMyDisplay");

        added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
        if (added_)
        {
            nid_.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid_);
        }

        window.AddHandler(kTrayCallback,
            [this](HWND, UINT, WPARAM, LPARAM lp, LRESULT& result) -> bool
            {
                UINT event = LOWORD(lp);
                if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU || event == WM_LBUTTONUP)
                {
                    ShowMenu();
                }
                result = 0;
                return true;
            });

        window.AddHandler(WM_COMMAND,
            [this](HWND, UINT, WPARAM wp, LPARAM, LRESULT& result) -> bool
            {
                if (OnCommand(LOWORD(wp)))
                {
                    result = 0;
                    return true;
                }
                return false;
            });

        if (taskbarCreatedMsg_ != 0)
        {
            window.AddHandler(taskbarCreatedMsg_,
                [this](HWND, UINT, WPARAM, LPARAM, LRESULT& result) -> bool
                {
                    Shell_NotifyIconW(NIM_ADD, &nid_);
                    nid_.uVersion = NOTIFYICON_VERSION_4;
                    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
                    added_ = true;
                    result = 0;
                    return true;
                });
        }

        return added_;
    }

    void Tray::Destroy()
    {
        if (added_)
        {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            added_ = false;
        }
    }

    void Tray::SetKeeperEnabled(bool enabled)
    {
        keeperEnabled_ = enabled;
    }

    void Tray::ShowBalloon(const std::string& title, const std::string& text)
    {
        if (!added_)
        {
            return;
        }
        NOTIFYICONDATAW b = nid_;
        b.uFlags = NIF_INFO;
        b.dwInfoFlags = NIIF_INFO;
        std::wstring wtitle = WideFromUtf8(title);
        std::wstring wtext = WideFromUtf8(text);
        wcsncpy_s(b.szInfoTitle, wtitle.c_str(), _TRUNCATE);
        wcsncpy_s(b.szInfo, wtext.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &b);
    }

    void Tray::ShowMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }
        AppendMenuW(menu, MF_STRING, IDM_SAVE, L"Save layout now\tCtrl+Alt+S");
        AppendMenuW(menu, MF_STRING, IDM_RESTORE, L"Restore layout now\tCtrl+Alt+R");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_DOCTOR, L"Sleep Doctor report...");
        AppendMenuW(menu, MF_STRING, IDM_DOCTOR_ADMIN, L"Sleep Doctor report (full, admin)...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_LABELS, L"Monitor labels...");
        AppendMenuW(menu, MF_STRING, IDM_GHOSTS, L"Detect ghost monitors");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (keeperEnabled_ ? MF_CHECKED : MF_UNCHECKED),
                    IDM_TOGGLE_KEEPER, L"Layout Keeper enabled");
        AppendMenuW(menu, MF_STRING, IDM_OPENFOLDER, L"Open data folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

        POINT pt;
        GetCursorPos(&pt);
        // Required so the menu dismisses correctly when focus is elsewhere.
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    bool Tray::OnCommand(int id)
    {
        switch (id)
        {
        case IDM_SAVE:
            if (onSave)
            {
                onSave();
            }
            return true;
        case IDM_RESTORE:
            if (onRestore)
            {
                onRestore();
            }
            return true;
        case IDM_DOCTOR:
            if (onDoctor)
            {
                onDoctor();
            }
            return true;
        case IDM_DOCTOR_ADMIN:
            if (onDoctorAdmin)
            {
                onDoctorAdmin();
            }
            return true;
        case IDM_LABELS:
            if (onLabels)
            {
                onLabels();
            }
            return true;
        case IDM_GHOSTS:
            if (onGhosts)
            {
                onGhosts();
            }
            return true;
        case IDM_TOGGLE_KEEPER:
            if (onToggleKeeper)
            {
                onToggleKeeper();
            }
            return true;
        case IDM_OPENFOLDER:
            if (onOpenFolder)
            {
                onOpenFolder();
            }
            return true;
        case IDM_EXIT:
            if (onExit)
            {
                onExit();
            }
            return true;
        default:
            return false;
        }
    }
}
