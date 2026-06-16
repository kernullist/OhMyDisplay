// OhMyDisplay - lightweight in-process pub/sub. Synchronous dispatch on the caller
// thread; subscribers that need heavy work hand off to their own worker threads.
#pragma once

#include <functional>
#include <vector>
#include <mutex>
#include <cstdint>

namespace omd
{
    enum class CoreEvent
    {
        DisplayChanged,     // WM_DISPLAYCHANGE: topology may have changed
        PowerSuspend,       // entering sleep
        PowerResume,        // resumed from sleep
        DisplayStateOn,     // monitor display turned on
        DisplayStateOff,    // monitor display turned off
        SettingChange       // WM_SETTINGCHANGE catch-all
    };

    struct EventArgs
    {
        CoreEvent type;
        int64_t param = 0; // optional payload (e.g. raw WPARAM)
    };

    using EventHandler = std::function<void(const EventArgs&)>;

    class EventBus
    {
    public:
        // Subscribe to an event type. Returns a token usable for Unsubscribe.
        int Subscribe(CoreEvent type, EventHandler handler);
        void Unsubscribe(int token);

        // Publish to all subscribers of this type (synchronous).
        void Post(CoreEvent type, int64_t param = 0);

    private:
        struct Entry
        {
            int token;
            CoreEvent type;
            EventHandler handler;
        };
        std::mutex mutex_;
        std::vector<Entry> entries_;
        int nextToken_ = 1;
    };
}
