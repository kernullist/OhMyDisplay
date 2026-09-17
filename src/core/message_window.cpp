// OhMyDisplay - hidden message hub window implementation.
#include "core/message_window.h"
#include "core/logger.h"

namespace omd
{
    const wchar_t* MessageWindow::ClassName()
    {
        return L"OhMyDisplay.MessageHub";
    }

    MessageWindow::~MessageWindow()
    {
        Destroy();
    }

    void MessageWindow::AddHandler(UINT msg, Handler handler)
    {
        handlers_[msg].push_back(std::move(handler));
    }

    bool MessageWindow::Create()
    {
        bool ok = false;
        do
        {
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            WNDCLASSEXW wc;
            ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &MessageWindow::WndProc;
            wc.hInstance = hInst;
            wc.lpszClassName = ClassName();
            if (RegisterClassExW(&wc) == 0)
            {
                if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                {
                    OMD_LOGE("msgwin: RegisterClassEx failed gle=%lu", GetLastError());
                    break;
                }
            }
            classRegistered_ = true;

            // Create a normal but hidden top-level window so it receives broadcasts.
            hwnd_ = CreateWindowExW(0, ClassName(), L"OhMyDisplay", WS_OVERLAPPED,
                                    0, 0, 0, 0, nullptr, nullptr, hInst, this);
            if (hwnd_ == nullptr)
            {
                OMD_LOGE("msgwin: CreateWindowEx failed gle=%lu", GetLastError());
                break;
            }
            ok = true;
        } while (false);
        return ok;
    }

    void MessageWindow::Destroy()
    {
        if (hwnd_ != nullptr)
        {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (classRegistered_)
        {
            UnregisterClassW(ClassName(), GetModuleHandleW(nullptr));
            classRegistered_ = false;
        }
    }

    LRESULT CALLBACK MessageWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* self = reinterpret_cast<MessageWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        auto* self = reinterpret_cast<MessageWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr)
        {
            return self->Dispatch(hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT MessageWindow::Dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        LRESULT result = 0;
        do
        {
            auto it = handlers_.find(msg);
            if (it != handlers_.end())
            {
                bool consumed = false;
                for (auto& h : it->second)
                {
                    LRESULT r = 0;
                    if (h && h(hwnd, msg, wp, lp, r))
                    {
                        result = r;
                        consumed = true;
                        break;
                    }
                }
                if (consumed)
                {
                    break;
                }
            }
            result = DefWindowProcW(hwnd, msg, wp, lp);
        } while (false);
        return result;
    }
}
