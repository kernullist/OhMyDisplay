// OhMyDisplay - adaptive sliding-window debouncer for topology-change bursts.
#pragma once

#include "core/settings.h"

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace omd
{
    // Compute the debounce window in milliseconds, scaled by active monitor count
    // and clamped to [minMs, maxMs]. Single-exit per project style.
    DWORD ComputeDebounceMs(const DebounceConfig& cfg, int activeMonitorCount);

    class Debouncer
    {
    public:
        using Callback = std::function<void()>;

        explicit Debouncer(Callback cb);
        ~Debouncer();

        Debouncer(const Debouncer&) = delete;
        Debouncer& operator=(const Debouncer&) = delete;

        void Start();
        void Stop();

        void SetConfig(const DebounceConfig& cfg);

        // Schedule a fire after an adaptive window. A new Trigger within the window
        // slides the deadline forward (burst absorption).
        void Trigger(int activeMonitorCount);

    private:
        void WorkerLoop();

        Callback cb_;
        DebounceConfig cfg_;
        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::chrono::steady_clock::time_point deadline_;
        bool pending_ = false;
        bool running_ = false;
        std::atomic<bool> quit_{false};
    };
}
