// OhMyDisplay - power/display notification implementation.
#include "core/power_notify.h"
#include "core/message_window.h"
#include "core/event_bus.h"
#include "core/logger.h"

namespace omd
{
    namespace
    {
        // Power-setting GUIDs declared locally to avoid initguid/link surprises.
        const GUID kGuidConsoleDisplayState =
            {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
        const GUID kGuidMonitorPowerOn =
            {0x02731015, 0x4510, 0x4526, {0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea}};
    }

    PowerNotify::~PowerNotify()
    {
        Unregister();
    }

    bool PowerNotify::Register(MessageWindow& window, EventBus& bus)
    {
        bool ok = false;
        do
        {
            HWND hwnd = window.Handle();
            if (hwnd == nullptr)
            {
                break;
            }
            bus_ = &bus;

            hConsoleDisplay_ = RegisterPowerSettingNotification(
                hwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
            hMonitorPower_ = RegisterPowerSettingNotification(
                hwnd, &kGuidMonitorPowerOn, DEVICE_NOTIFY_WINDOW_HANDLE);

            window.AddHandler(WM_POWERBROADCAST,
                [this](HWND, UINT, WPARAM wp, LPARAM lp, LRESULT& result) -> bool
                {
                    result = TRUE;
                    switch (wp)
                    {
                    case PBT_APMSUSPEND:
                        OMD_LOGI("power: suspend");
                        bus_->Post(CoreEvent::PowerSuspend);
                        break;
                    case PBT_APMRESUMEAUTOMATIC:
                    case PBT_APMRESUMESUSPEND:
                        OMD_LOGI("power: resume");
                        bus_->Post(CoreEvent::PowerResume);
                        break;
                    case PBT_POWERSETTINGCHANGE:
                    {
                        auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
                        if (setting != nullptr && setting->DataLength >= sizeof(DWORD))
                        {
                            DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
                            if (IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState))
                            {
                                // 0 = off, 1 = on, 2 = dimmed
                                if (state == 0)
                                {
                                    OMD_LOGI("power: console display off");
                                    bus_->Post(CoreEvent::DisplayStateOff);
                                }
                                else if (state == 1)
                                {
                                    OMD_LOGI("power: console display on");
                                    bus_->Post(CoreEvent::DisplayStateOn);
                                }
                            }
                            else if (IsEqualGUID(setting->PowerSetting, kGuidMonitorPowerOn))
                            {
                                // 0 = off, 1 = on. Complements GUID_CONSOLE_DISPLAY_STATE
                                // on machines that only fire the legacy monitor-power GUID.
                                if (state == 0)
                                {
                                    OMD_LOGI("power: monitor power off");
                                    bus_->Post(CoreEvent::DisplayStateOff);
                                }
                                else if (state == 1)
                                {
                                    OMD_LOGI("power: monitor power on");
                                    bus_->Post(CoreEvent::DisplayStateOn);
                                }
                            }
                        }
                        break;
                    }
                    default:
                        break;
                    }
                    return true;
                });
            ok = true;
        } while (false);
        return ok;
    }

    void PowerNotify::Unregister()
    {
        if (hConsoleDisplay_ != nullptr)
        {
            UnregisterPowerSettingNotification(hConsoleDisplay_);
            hConsoleDisplay_ = nullptr;
        }
        if (hMonitorPower_ != nullptr)
        {
            UnregisterPowerSettingNotification(hMonitorPower_);
            hMonitorPower_ = nullptr;
        }
    }
}
