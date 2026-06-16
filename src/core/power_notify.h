// OhMyDisplay - power and display-state notifications, translated onto the EventBus.
#pragma once

#include <windows.h>

namespace omd
{
    class MessageWindow;
    class EventBus;

    class PowerNotify
    {
    public:
        PowerNotify() = default;
        ~PowerNotify();

        PowerNotify(const PowerNotify&) = delete;
        PowerNotify& operator=(const PowerNotify&) = delete;

        // Register power-setting notifications on the hub window and install a
        // WM_POWERBROADCAST handler that posts CoreEvents to the bus.
        bool Register(MessageWindow& window, EventBus& bus);
        void Unregister();

    private:
        EventBus* bus_ = nullptr;
        HPOWERNOTIFY hConsoleDisplay_ = nullptr;
        HPOWERNOTIFY hMonitorPower_ = nullptr;
    };
}
