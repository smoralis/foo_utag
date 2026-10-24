#include "utag_tunein.h"

// -----------------------------------------------------------------------------
// File > Import stations from TuneIn...
//
// Searches TuneIn by station name, artist or song and imports the first N
// stations found as .utag files (see utag_tunein_api.cpp). To pick stations
// by hand, use the TuneIn browser (utag_tunein_browser.cpp).
// -----------------------------------------------------------------------------

namespace
{
    constexpr int kIdTiQuery = 1201;
    constexpr int kIdTiStation = 1202; // station / artist / song must stay contiguous
    constexpr int kIdTiArtist = 1203;
    constexpr int kIdTiSong = 1204;
    constexpr int kIdTiLimit = 1205;
    constexpr int kIdTiStatus = 1206;

    // The window is modeless, so its state lives on the heap and is freed in
    // WM_NCDESTROY.
    struct tunein_prompt_state
    {
        fb2k::CCoreDarkModeHooks darkHooks;
    };

    HWND g_tuneinPromptWnd = nullptr; // the one open window, if any

    INT_PTR CALLBACK tunein_prompt_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<tunein_prompt_state*>(
            GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<tunein_prompt_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);

            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg); // Tab / Enter / Esc keys

            CheckRadioButton(dlg, kIdTiStation, kIdTiSong, kIdTiStation);
            SetFocus(GetDlgItem(dlg, kIdTiQuery));
            return FALSE; // focus set manually
        }

        case WM_COMMAND:
        {
            const int id = LOWORD(wp);

            if (id == IDCANCEL)
            {
                DestroyWindow(dlg);
                return TRUE;
            }

            if (id == IDOK && state != nullptr)
            {
                const std::string text =
                    wide_to_utf8(trim_wide(read_dialog_text(dlg, kIdTiQuery)));

                if (text.empty())
                {
                    SetDlgItemTextW(dlg, kIdTiStatus, L"Enter something to search for.");
                    SetFocus(GetDlgItem(dlg, kIdTiQuery));
                    return TRUE;
                }

                long limit = 25;
                const std::wstring limitText = trim_wide(read_dialog_text(dlg, kIdTiLimit));
                if (!limitText.empty())
                {
                    wchar_t* end = nullptr;
                    limit = std::wcstol(limitText.c_str(), &end, 10);
                    if (end == nullptr || *end != L'\0' || limit < 1 || limit > 200)
                    {
                        SetDlgItemTextW(dlg, kIdTiStatus,
                            L"Maximum stations must be between 1 and 200.");
                        SetFocus(GetDlgItem(dlg, kIdTiLimit));
                        return TRUE;
                    }
                }

                tunein_search_request request;
                request.text = text;
                request.limit = limit;
                if (IsDlgButtonChecked(dlg, kIdTiArtist) == BST_CHECKED)
                    request.kind = tunein_kind::artist;
                else if (IsDlgButtonChecked(dlg, kIdTiSong) == BST_CHECKED)
                    request.kind = tunein_kind::song;

                // May ask for the default folder the first time; keep this
                // window open if that is cancelled.
                std::wstring parentFolder;
                EnableWindow(dlg, FALSE);
                const bool haveFolder = get_default_folder(parentFolder, false);
                EnableWindow(dlg, TRUE);

                if (!haveFolder)
                    return TRUE;

                DestroyWindow(dlg);
                tunein_start_search_import(request, parentFolder);
                return TRUE;
            }

            break;
        }

        case WM_CLOSE:
            DestroyWindow(dlg);
            return TRUE;

        case WM_NCDESTROY:
            modeless_dialog_manager::g_remove(dlg);
            SetWindowLongPtrW(dlg, DWLP_USER, 0);
            if (g_tuneinPromptWnd == dlg)
                g_tuneinPromptWnd = nullptr;
            delete state;
            break;

        default:
            break;
        }

        return FALSE;
    }

    // Opens the (modeless) window, or brings the open one to the front.
    void open_tunein_dialog(HWND owner)
    {
        if (g_tuneinPromptWnd != nullptr && IsWindow(g_tuneinPromptWnd))
        {
            ShowWindow(g_tuneinPromptWnd, SW_RESTORE);
            SetForegroundWindow(g_tuneinPromptWnd);
            return;
        }

        constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;
        constexpr DWORD kEditStyle = ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP;

        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 260, 124, L"Import stations from TuneIn", L"Segoe UI", 8);

        t.item(kStatic, SS_LEFT, 8, 12, 60, 10, 0xFFFF, L"Search for:");
        t.item(kEdit, kEditStyle, 72, 10, 180, 13, kIdTiQuery, L"");

        t.item(kStatic, SS_LEFT, 8, 30, 60, 10, 0xFFFF, L"Search by:");
        t.item(kButton, BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 72, 29, 58, 10,
            kIdTiStation, L"Station name");
        t.item(kButton, BS_AUTORADIOBUTTON, 134, 29, 52, 10, kIdTiArtist, L"Artist");
        t.item(kButton, BS_AUTORADIOBUTTON, 190, 29, 62, 10, kIdTiSong, L"Song title");

        t.item(kStatic, SS_LEFT, 8, 48, 60, 10, 0xFFFF, L"Max stations:");
        t.item(kEdit, kEditStyle | ES_NUMBER, 72, 46, 60, 13, kIdTiLimit, L"25");

        t.item(kStatic, SS_LEFT, 8, 64, 244, 20, 0xFFFF,
            L"Each station needs a further request to find its stream, so a large "
            L"number takes a while. To choose stations by hand, use File > Browse TuneIn.");

        t.item(kStatic, SS_LEFT, 8, 88, 244, 10, kIdTiStatus, L"");

        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 104, 50, 14, IDOK, L"Import");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 202, 104, 50, 14, IDCANCEL, L"Cancel");

        auto* state = new tunein_prompt_state;
        const HWND dlg = CreateDialogIndirectParamW(
            core_api::get_my_instance(), t.data(), owner,
            tunein_prompt_proc, reinterpret_cast<LPARAM>(state));

        if (dlg == nullptr)
        {
            delete state;
            return;
        }

        g_tuneinPromptWnd = dlg;
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
    }
}

void utag_open_tunein_dialog(HWND owner)
{
    open_tunein_dialog(owner);
}
