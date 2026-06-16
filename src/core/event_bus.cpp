// OhMyDisplay - event bus implementation.
#include "core/event_bus.h"

namespace omd
{
    int EventBus::Subscribe(CoreEvent type, EventHandler handler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int token = nextToken_++;
        entries_.push_back(Entry{token, type, std::move(handler)});
        return token;
    }

    void EventBus::Unsubscribe(int token)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < entries_.size(); ++i)
        {
            if (entries_[i].token == token)
            {
                entries_.erase(entries_.begin() + static_cast<long>(i));
                break;
            }
        }
    }

    void EventBus::Post(CoreEvent type, int64_t param)
    {
        // Copy matching handlers under lock, then invoke unlocked so a handler may
        // safely (un)subscribe without deadlocking.
        std::vector<EventHandler> matched;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& e : entries_)
            {
                if (e.type == type)
                {
                    matched.push_back(e.handler);
                }
            }
        }
        EventArgs args{type, param};
        for (auto& h : matched)
        {
            if (h)
            {
                h(args);
            }
        }
    }
}
