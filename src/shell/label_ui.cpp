// OhMyDisplay - monitor label editor implementation.
#include "shell/label_ui.h"
#include "core/common.h"

#include <vector>

namespace omd
{
    namespace
    {
        const wchar_t* kClass = L"OhMyDisplay.LabelEditor";
        const int ID_SAVE = 1;
        const int ID_CANCEL = 2;
        const int ID_EDIT_BASE = 100;

        struct DialogState
        {
            std::vector<LabelRow>* rows = nullptr;
            std::vector<HWND> edits;
            bool saved = false;
            bool done = false;
        };

        void ApplyFont(HWND hwnd)
        {
            HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_NCCREATE)
            {
                auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
            auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

            switch (msg)
            {
            case WM_COMMAND:
            {
                int id = LOWORD(wp);
                if (id == ID_SAVE && st != nullptr)
                {
                    for (size_t i = 0; i < st->edits.size() && i < st->rows->size(); ++i)
                    {
                        wchar_t buf[256];
                        GetWindowTextW(st->edits[i], buf, static_cast<int>(std::size(buf)));
                        (*st->rows)[i].edited = Utf8FromWide(buf);
                    }
                    st->saved = true;
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (id == ID_CANCEL && st != nullptr)
                {
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
                if (st != nullptr)
                {
                    st->done = true;
                }
                DestroyWindow(hwnd);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    bool ShowLabelEditor(HWND owner, HINSTANCE hInst, std::vector<LabelRow>& rows)
    {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc); // ignore already-registered

        DialogState state;
        state.rows = &rows;

        const int margin = 14;
        const int rowH = 56;
        const int width = 480;
        const int headerH = 36;
        const int footerH = 52;
        int height = headerH + static_cast<int>(rows.size()) * rowH + footerH + margin;

        // Center on the work area.
        RECT wa{0, 0, 800, 600};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int x = wa.left + ((wa.right - wa.left) - width) / 2;
        int y = wa.top + ((wa.bottom - wa.top) - height) / 2;

        HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"OhMyDisplay - Monitor labels",
                                    WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                    x, y, width, height, owner, nullptr, hInst, &state);
        if (hwnd == nullptr)
        {
            return false;
        }

        HWND header = CreateWindowExW(0, L"STATIC",
            L"Give each monitor a friendly name. Same-model monitors need distinct labels.",
            WS_CHILD | WS_VISIBLE, margin, 10, width - 2 * margin, 24, hwnd, nullptr, hInst, nullptr);
        ApplyFont(header);

        int cy = headerH;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            std::wstring caption = WideFromUtf8(rows[i].friendlyName);
            if (caption.empty())
            {
                caption = L"(unknown monitor)";
            }
            caption += L"  -  ";
            caption += WideFromUtf8(rows[i].id);
            if (rows[i].collision)
            {
                caption += L"  [same-model!]";
            }

            HWND cap = CreateWindowExW(0, L"STATIC", caption.c_str(), WS_CHILD | WS_VISIBLE,
                                       margin, cy, width - 2 * margin, 18, hwnd, nullptr, hInst, nullptr);
            ApplyFont(cap);

            HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                        WideFromUtf8(rows[i].current).c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        margin, cy + 20, width - 2 * margin, 24,
                                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_BASE + i)),
                                        hInst, nullptr);
            ApplyFont(edit);
            state.edits.push_back(edit);
            cy += rowH;
        }

        int btnW = 90;
        int btnY = cy + 6;
        HWND save = CreateWindowExW(0, L"BUTTON", L"Save",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    width - 2 * btnW - margin - 8, btnY, btnW, 28,
                                    hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)), hInst, nullptr);
        ApplyFont(save);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      width - btnW - margin, btnY, btnW, 28,
                                      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL)), hInst, nullptr);
        ApplyFont(cancel);

        // Modal: disable the owner and pump until the dialog closes.
        if (owner != nullptr)
        {
            EnableWindow(owner, FALSE);
        }
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);

        MSG m;
        while (!state.done && GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(hwnd, &m))
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }

        if (owner != nullptr)
        {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        return state.saved;
    }
}
