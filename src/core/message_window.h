// OhMyDisplay - hidden top-level message hub window.
//
// Design note: the spec mentions HWND_MESSAGE, but message-only windows do NOT
// receive broadcast messages (WM_DISPLAYCHANGE, WM_POWERBROADCAST, WM_SETTINGCHANGE).
// We therefore use a normal top-level window that is simply never shown. It is the
// single hub for power/display notifications, tray callbacks and global hotkeys.
#pragma once

#include <windows.h>
#include <functional>
#include <map>
#include <vector>

namespace omd
{
    class MessageWindow
    {
    public:
        // Handler returns true if it consumed the message and set 'result'.
        using Handler = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT&)>;

        MessageWindow() = default;
        ~MessageWindow();

        MessageWindow(const MessageWindow&) = delete;
        MessageWindow& operator=(const MessageWindow&) = delete;

        bool Create();
        void Destroy();

        HWND Handle() const
        {
            return hwnd_;
        }

        // Register a handler for a message id. Multiple handlers per id run in order
        // until one consumes the message.
        void AddHandler(UINT msg, Handler handler);

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        LRESULT Dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

        HWND hwnd_ = nullptr;
        bool classRegistered_ = false;
        std::map<UINT, std::vector<Handler>> handlers_;
    };
}
