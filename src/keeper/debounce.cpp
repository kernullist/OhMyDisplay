// OhMyDisplay - adaptive debouncer implementation.
#include "keeper/debounce.h"
#include "core/logger.h"

namespace omd
{
    DWORD ComputeDebounceMs(const DebounceConfig& cfg, int activeMonitorCount)
    {
        DWORD value = 0;
        do
        {
            long raw = static_cast<long>(cfg.baseMs)
                     + static_cast<long>(cfg.perMonitorMs) * activeMonitorCount;
            if (raw < static_cast<long>(cfg.minMs))
            {
                value = cfg.minMs;
                break;
            }
            if (raw > static_cast<long>(cfg.maxMs))
            {
                value = cfg.maxMs;
                break;
            }
            value = static_cast<DWORD>(raw);
        } while (false);
        return value;
    }

    Debouncer::Debouncer(Callback cb) : cb_(std::move(cb))
    {
    }

    Debouncer::~Debouncer()
    {
        Stop();
    }

    void Debouncer::SetConfig(const DebounceConfig& cfg)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg_ = cfg;
    }

    void Debouncer::Start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
        {
            return;
        }
        quit_.store(false);
        running_ = true;
        worker_ = std::thread(&Debouncer::WorkerLoop, this);
    }

    void Debouncer::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
            {
                return;
            }
            quit_.store(true);
        }
        cv_.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        pending_ = false;
    }

    void Debouncer::Trigger(int activeMonitorCount)
    {
        DWORD ms = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ms = ComputeDebounceMs(cfg_, activeMonitorCount);
            auto candidate = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            // Slide forward only: a burst trigger with a smaller window (fewer monitors
            // momentarily) must not pull an already-pending deadline earlier.
            if (!pending_ || candidate > deadline_)
            {
                deadline_ = candidate;
            }
            pending_ = true;
        }
        OMD_LOGI("debounce: trigger monitors=%d window=%lums", activeMonitorCount, ms);
        cv_.notify_all();
    }

    void Debouncer::WorkerLoop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!quit_.load())
        {
            if (!pending_)
            {
                cv_.wait(lock, [this]
                {
                    return quit_.load() || pending_;
                });
                continue;
            }
            // Wait until the (possibly slid) deadline, waking if a new trigger arrives.
            auto target = deadline_;
            std::cv_status status = cv_.wait_until(lock, target);
            if (quit_.load())
            {
                break;
            }
            if (status == std::cv_status::timeout)
            {
                // No new trigger slid the deadline past now -> fire.
                if (std::chrono::steady_clock::now() >= deadline_)
                {
                    pending_ = false;
                    lock.unlock();
                    if (cb_)
                    {
                        try
                        {
                            cb_();
                        }
                        catch (...)
                        {
                        }
                    }
                    lock.lock();
                }
            }
            // Otherwise the deadline was extended; loop re-reads it.
        }
    }
}
