// OhMyDisplay - system tray icon, context menu and balloon notifications.
#pragma once

#include "core/message_window.h"

#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>

namespace omd
{
    class Tray
    {
    public:
        using Action = std::function<void()>;

        bool Create(MessageWindow& window, HICON icon);
        void Destroy();

        void ShowBalloon(const std::string& title, const std::string& text);
        void SetKeeperEnabled(bool enabled);

        // Menu action hooks.
        Action onSave;
        Action onRestore;
        Action onDoctor;
        Action onDoctorAdmin;
        Action onLabels;
        Action onGhosts;
        Action onToggleKeeper;
        Action onOpenFolder;
        Action onExit;

    private:
        void ShowMenu();
        bool OnCommand(int id);

        HWND hwnd_ = nullptr;
        NOTIFYICONDATAW nid_{};
        UINT taskbarCreatedMsg_ = 0;
        bool keeperEnabled_ = true;
        bool added_ = false;
    };
}
