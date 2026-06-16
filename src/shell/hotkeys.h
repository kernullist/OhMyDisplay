// OhMyDisplay - global hotkey registration and dispatch.
#pragma once

#include "core/message_window.h"

#include <windows.h>
#include <functional>
#include <string>
#include <map>

namespace omd
{
    class Hotkeys
    {
    public:
        using Action = std::function<void()>;

        bool Init(MessageWindow& window);
        void Shutdown();

        // Register a hotkey from a string like "Ctrl+Alt+S". Returns false if the
        // string is invalid or the combination is already taken by another app.
        bool Register(const std::string& spec, Action action);

    private:
        static bool ParseSpec(const std::string& spec, UINT& mods, UINT& vk);

        HWND hwnd_ = nullptr;
        int nextId_ = 1;
        std::map<int, Action> actions_;
    };
}
