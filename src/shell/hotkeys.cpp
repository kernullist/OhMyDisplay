// OhMyDisplay - global hotkey implementation.
#include "shell/hotkeys.h"
#include "core/common.h"
#include "core/logger.h"

#include <vector>
#include <sstream>

namespace omd
{
    bool Hotkeys::ParseSpec(const std::string& spec, UINT& mods, UINT& vk)
    {
        mods = MOD_NOREPEAT;
        vk = 0;

        // Split on '+'.
        std::vector<std::string> parts;
        std::string cur;
        for (char c : spec)
        {
            if (c == '+')
            {
                parts.push_back(std::string(Trim(cur)));
                cur.clear();
            }
            else
            {
                cur.push_back(c);
            }
        }
        parts.push_back(std::string(Trim(cur)));

        bool keySet = false;
        for (const auto& p : parts)
        {
            if (p.empty())
            {
                continue;
            }
            if (IEquals(p, "Ctrl") || IEquals(p, "Control"))
            {
                mods |= MOD_CONTROL;
            }
            else if (IEquals(p, "Alt"))
            {
                mods |= MOD_ALT;
            }
            else if (IEquals(p, "Shift"))
            {
                mods |= MOD_SHIFT;
            }
            else if (IEquals(p, "Win"))
            {
                mods |= MOD_WIN;
            }
            else
            {
                // The remaining token is the key.
                std::string k = ToLowerAscii(p);
                if (k.size() == 1)
                {
                    char c = k[0];
                    if (c >= 'a' && c <= 'z')
                    {
                        vk = static_cast<UINT>('A' + (c - 'a'));
                        keySet = true;
                    }
                    else if (c >= '0' && c <= '9')
                    {
                        vk = static_cast<UINT>(c);
                        keySet = true;
                    }
                }
                else if (k.size() >= 2 && k[0] == 'f')
                {
                    int n = std::atoi(k.c_str() + 1);
                    if (n >= 1 && n <= 24)
                    {
                        vk = static_cast<UINT>(VK_F1 + (n - 1));
                        keySet = true;
                    }
                }
            }
        }
        return keySet;
    }

    bool Hotkeys::Init(MessageWindow& window)
    {
        hwnd_ = window.Handle();
        if (hwnd_ == nullptr)
        {
            return false;
        }
        window.AddHandler(WM_HOTKEY,
            [this](HWND, UINT, WPARAM wp, LPARAM, LRESULT& result) -> bool
            {
                int id = static_cast<int>(wp);
                auto it = actions_.find(id);
                if (it != actions_.end() && it->second)
                {
                    it->second();
                }
                result = 0;
                return true;
            });
        return true;
    }

    void Hotkeys::Shutdown()
    {
        if (hwnd_ != nullptr)
        {
            for (const auto& kv : actions_)
            {
                UnregisterHotKey(hwnd_, kv.first);
            }
        }
        actions_.clear();
    }

    bool Hotkeys::Register(const std::string& spec, Action action)
    {
        UINT mods = 0;
        UINT vk = 0;
        if (!ParseSpec(spec, mods, vk))
        {
            OMD_LOGW("hotkey: invalid spec '%s'", spec.c_str());
            return false;
        }
        int id = nextId_++;
        if (RegisterHotKey(hwnd_, id, mods, vk) == 0)
        {
            OMD_LOGW("hotkey: register failed for '%s' gle=%lu (in use?)", spec.c_str(), GetLastError());
            return false;
        }
        actions_[id] = std::move(action);
        OMD_LOGI("hotkey: registered '%s'", spec.c_str());
        return true;
    }
}
