
#include "utag_ui.h"

#include <uxtheme.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Gdi32.lib")

namespace
{
    constexpr int kPrefHandoff = 2001;
    constexpr int kPrefFavicon = 2002;
    constexpr int kPrefFolder = 2003;
    constexpr int kPrefBrowse = 2004;
    constexpr int kPrefLogging = 2005;
    constexpr int kPrefPopups = 2006;
    constexpr int kPrefTuneinDetails = 2007;
    constexpr int kPrefCtxLookup = 2008;
    constexpr int kPrefPlRadioBrowser = 2009;
    constexpr int kPrefPlTunein = 2010;
    constexpr int kPrefPlIcecast = 2011;

    // {85C77DE3-3774-474F-A8EA-87564A3BBDE9}
    static const GUID g_guid_prefs_page =
    {
        0x85c77de3, 0x3774, 0x474f,
        { 0xa8, 0xea, 0x87, 0x56, 0x4a, 0x3b, 0xbd, 0xe9 }
    };

    class utag_prefs_instance : public preferences_page_instance
    {
    public:
        utag_prefs_instance(
            HWND parent,
            preferences_page_callback::ptr callback)
            : m_callback(callback)
        {
            constexpr WORD kButton = 0x0080;
            constexpr WORD kEdit = 0x0081;
            constexpr WORD kStatic = 0x0082;

            constexpr DWORD kGroup = BS_GROUPBOX;
            constexpr DWORD kCheck =
                BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP;

            // Compact page. No vertical scrollbar is used.
            dialog_template t;
            t.begin(
                WS_CHILD | DS_CONTROL,
                0, 0, 280, 300,
                L"", L"Segoe UI", 8);

            // Playback
            t.item(kButton, kGroup,
                0, 0, 280, 48, 0xFFFF, L"Playback");

            t.item(kButton, kCheck,
                8, 11, 264, 18, kPrefHandoff,
                L"Reuse the running stream when the playing item is retagged "
                L"(avoids a reconnect pause)");

            t.item(kButton, kCheck,
                8, 29, 264, 18, kPrefFavicon,
                L"Download the favicon image during playback "
                L"(saved next to the .utag file)");

            // Default folder
            t.item(kButton, kGroup,
                0, 51, 280, 49, 0xFFFF, L"Default folder");

            t.item(kStatic, SS_LEFT,
                8, 62, 264, 15, 0xFFFF,
                L"Used by \"Create utag files in default folder\". "
                L"Leave empty to be asked when needed.");

            t.item(kEdit,
                ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                8, 79, 198, 15, kPrefFolder, L"");

            t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP,
                212, 77, 60, 19, kPrefBrowse, L"Browse...");

            // Messages
            t.item(kButton, kGroup,
                0, 103, 280, 44, 0xFFFF, L"Messages");

            t.item(kButton, kCheck,
                8, 114, 264, 16, kPrefLogging,
                L"Write messages to the foobar2000 console");

            t.item(kButton, kCheck,
                8, 130, 264, 17, kPrefPopups,
                L"Show a results window after import and create commands "
                L"(errors are always shown)");

            // Radio Browser and TuneIn
            t.item(kButton, kGroup,
                0, 150, 280, 65, 0xFFFF,
                L"Radio Browser and TuneIn");

            t.item(kButton, kCheck,
                8, 161, 264, 25, kPrefCtxLookup,
                L"Look each URL up in Radio Browser when creating utag "
                L"files from playlist tracks (uses its station details)");

            t.item(kButton, kCheck,
                8, 187, 264, 27, kPrefTuneinDetails,
                L"Look up station details (location, description, genre, "
                L"region) when importing from TuneIn; one extra request "
                L"per station");

            // Playlists
            t.item(kButton, kGroup,
                0, 218, 280, 53, 0xFFFF,
                L"Playlists for the browse windows");

            t.item(kStatic, SS_LEFT,
                8, 231, 62, 15, 0xFFFF, L"Radio Browser:");

            t.item(kEdit,
                ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                74, 229, 198, 11, kPrefPlRadioBrowser, L"");

            t.item(kStatic, SS_LEFT,
                8, 245, 62, 15, 0xFFFF, L"TuneIn:");

            t.item(kEdit,
                ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                74, 243, 198, 11, kPrefPlTunein, L"");

            t.item(kStatic, SS_LEFT,
                8, 259, 62, 15, 0xFFFF, L"Icecast:");

            t.item(kEdit,
                ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                74, 257, 198, 11, kPrefPlIcecast, L"");

            m_hwnd = CreateDialogIndirectParamW(
                core_api::get_my_instance(),
                t.data(),
                parent,
                &utag_prefs_instance::dlg_proc,
                reinterpret_cast<LPARAM>(this));
        }

        ~utag_prefs_instance()
        {
            if (m_hwnd && IsWindow(m_hwnd))
                DestroyWindow(m_hwnd);
        }

        fb2k::hwnd_t get_wnd() override
        {
            return m_hwnd;
        }

        t_uint32 get_state() override
        {
            t_uint32 state =
                preferences_state::resettable |
                preferences_state::dark_mode_supported;

            if (has_changed())
                state |= preferences_state::changed;

            return state;
        }

        void apply() override
        {
            g_cfg_handoff = checked(kPrefHandoff);
            g_cfg_favicon = checked(kPrefFavicon);
            g_cfg_logging = checked(kPrefLogging);
            g_cfg_result_popups = checked(kPrefPopups);
            g_cfg_tunein_details = checked(kPrefTuneinDetails);
            g_cfg_ctx_rb_lookup = checked(kPrefCtxLookup);

            g_cfg_playlist_radiobrowser.set_string(
                playlist_utf8(kPrefPlRadioBrowser).c_str());

            g_cfg_playlist_tunein.set_string(
                playlist_utf8(kPrefPlTunein).c_str());

            g_cfg_playlist_icecast.set_string(
                playlist_utf8(kPrefPlIcecast).c_str());

            g_cfg_default_dir.set_string(folder_utf8().c_str());

            m_callback->on_state_changed();
        }

        void reset() override
        {
            set_checked(kPrefHandoff, true);
            set_checked(kPrefFavicon, false);
            set_checked(kPrefLogging, true);
            set_checked(kPrefPopups, true);
            set_checked(kPrefTuneinDetails, false);
            set_checked(kPrefCtxLookup, true);

            SetDlgItemTextW(m_hwnd, kPrefPlRadioBrowser, L"RadioBrowser");
            SetDlgItemTextW(m_hwnd, kPrefPlTunein, L"Tunein");
            SetDlgItemTextW(m_hwnd, kPrefPlIcecast, L"Icecast");
            SetDlgItemTextW(m_hwnd, kPrefFolder, L"");

            m_callback->on_state_changed();
        }

    private:
        bool checked(int id) const
        {
            return IsDlgButtonChecked(m_hwnd, id) == BST_CHECKED;
        }

        void set_checked(int id, bool value) const
        {
            CheckDlgButton(
                m_hwnd, id,
                value ? BST_CHECKED : BST_UNCHECKED);
        }

        std::string playlist_utf8(int id) const
        {
            return wide_to_utf8(
                trim_wide(read_dialog_text(m_hwnd, id)));
        }

        std::string folder_utf8() const
        {
            return wide_to_utf8(
                trim_wide(read_dialog_text(m_hwnd, kPrefFolder)));
        }

        bool has_changed() const
        {
            if (!m_hwnd)
                return false;

            return
                checked(kPrefHandoff) != static_cast<bool>(g_cfg_handoff) ||
                checked(kPrefFavicon) != static_cast<bool>(g_cfg_favicon) ||
                checked(kPrefLogging) != static_cast<bool>(g_cfg_logging) ||
                checked(kPrefPopups) != static_cast<bool>(g_cfg_result_popups) ||
                checked(kPrefTuneinDetails) != static_cast<bool>(g_cfg_tunein_details) ||
                checked(kPrefCtxLookup) != static_cast<bool>(g_cfg_ctx_rb_lookup) ||
                playlist_utf8(kPrefPlRadioBrowser) !=
                std::string(g_cfg_playlist_radiobrowser.get_ptr()) ||
                playlist_utf8(kPrefPlTunein) !=
                std::string(g_cfg_playlist_tunein.get_ptr()) ||
                playlist_utf8(kPrefPlIcecast) !=
                std::string(g_cfg_playlist_icecast.get_ptr()) ||
                folder_utf8() !=
                std::string(g_cfg_default_dir.get_ptr());
        }

        void load_values()
        {
            m_loading = true;

            set_checked(kPrefHandoff, g_cfg_handoff);
            set_checked(kPrefFavicon, g_cfg_favicon);
            set_checked(kPrefLogging, g_cfg_logging);
            set_checked(kPrefPopups, g_cfg_result_popups);
            set_checked(kPrefTuneinDetails, g_cfg_tunein_details);
            set_checked(kPrefCtxLookup, g_cfg_ctx_rb_lookup);

            try
            {
                SetDlgItemTextW(
                    m_hwnd, kPrefPlRadioBrowser,
                    utf8_to_wide(g_cfg_playlist_radiobrowser.get_ptr()).c_str());

                SetDlgItemTextW(
                    m_hwnd, kPrefPlTunein,
                    utf8_to_wide(g_cfg_playlist_tunein.get_ptr()).c_str());

                SetDlgItemTextW(
                    m_hwnd, kPrefPlIcecast,
                    utf8_to_wide(g_cfg_playlist_icecast.get_ptr()).c_str());
            }
            catch (...)
            {
            }

            SendDlgItemMessageW(
                m_hwnd, kPrefPlRadioBrowser,
                EM_SETCUEBANNER, FALSE,
                reinterpret_cast<LPARAM>(L"RadioBrowser"));

            SendDlgItemMessageW(
                m_hwnd, kPrefPlTunein,
                EM_SETCUEBANNER, FALSE,
                reinterpret_cast<LPARAM>(L"Tunein"));

            SendDlgItemMessageW(
                m_hwnd, kPrefPlIcecast,
                EM_SETCUEBANNER, FALSE,
                reinterpret_cast<LPARAM>(L"Icecast"));

            try
            {
                SetDlgItemTextW(
                    m_hwnd, kPrefFolder,
                    utf8_to_wide(g_cfg_default_dir.get_ptr()).c_str());
            }
            catch (...)
            {
            }

            SendDlgItemMessageW(
                m_hwnd, kPrefFolder,
                EM_SETCUEBANNER, FALSE,
                reinterpret_cast<LPARAM>(L"Ask when needed"));

            m_loading = false;
        }

        // ---------------------------------------------------------------------
        // Layout: no scrollbar. Controls stretch horizontally and sections
        // expand vertically when the host provides more room.
        // ---------------------------------------------------------------------
        enum class resize_kind
        {
            fixed,
            stretch,
            shift
        };

        struct ctrl_info
        {
            HWND hwnd;
            RECT rc;
            resize_kind kind;
        };

        void capture_layout()
        {
            RECT client{};
            GetClientRect(m_hwnd, &client);

            m_designW = client.right;
            m_designH = client.bottom;
            m_ctrls.clear();

            for (HWND c = GetWindow(m_hwnd, GW_CHILD);
                c != nullptr;
                c = GetWindow(c, GW_HWNDNEXT))
            {
                RECT r{};
                GetWindowRect(c, &r);

                MapWindowPoints(
                    nullptr, m_hwnd,
                    reinterpret_cast<POINT*>(&r), 2);

                wchar_t cls[16]{};
                GetClassNameW(c, cls, 16);

                const LONG style = GetWindowLongW(c, GWL_STYLE);

                resize_kind kind = resize_kind::fixed;

                if (_wcsicmp(cls, L"Button") == 0)
                {
                    const LONG type = style & BS_TYPEMASK;

                    kind =
                        (type == BS_GROUPBOX ||
                            type == BS_AUTOCHECKBOX ||
                            type == BS_CHECKBOX)
                        ? resize_kind::stretch
                        : resize_kind::shift;
                }
                else if (_wcsicmp(cls, L"Edit") == 0)
                {
                    kind = resize_kind::stretch;
                }
                else if (_wcsicmp(cls, L"Static") == 0 &&
                    (r.right - r.left) > m_designW / 2)
                {
                    kind = resize_kind::stretch;
                }

                m_ctrls.push_back({ c, r, kind });
            }
        }

        bool page_is_dark() const
        {
            const HDC dc = GetDC(m_hwnd);
            if (!dc)
                return false;

            const HWND ref =
                m_ctrls.empty() ? m_hwnd : m_ctrls.front().hwnd;

            SendMessageW(
                m_hwnd,
                WM_CTLCOLORSTATIC,
                reinterpret_cast<WPARAM>(dc),
                reinterpret_cast<LPARAM>(ref));

            const COLORREF text = GetTextColor(dc);
            ReleaseDC(m_hwnd, dc);

            const int luminance =
                (GetRValue(text) * 299 +
                    GetGValue(text) * 587 +
                    GetBValue(text) * 114) / 1000;

            return luminance > 128;
        }

        void update_scrollbar_theme()
        {
            // No scrollbar is used. Keep the page's existing dark-mode hooks.
            const bool dark = page_is_dark();

            if (m_themeSet && dark == m_darkTheme)
                return;

            m_themeSet = true;
            m_darkTheme = dark;

            SetWindowTheme(
                m_hwnd,
                dark ? L"DarkMode_Explorer" : L"Explorer",
                nullptr);
        }

        void layout()
        {
            if (!m_hwnd || m_designW <= 0 || m_inLayout)
                return;

            m_inLayout = true;

            RECT rc{};
            GetClientRect(m_hwnd, &rc);

            // Explicitly disable the vertical scrollbar.
            ShowScrollBar(m_hwnd, SB_VERT, FALSE);

            const int deltaW = rc.right - m_designW;
            const int deltaH = rc.bottom - m_designH;

            // Distribute additional height among the section group boxes.
            // Children keep their vertical positions and sizes so that
            // compact layouts remain predictable at smaller heights.
            const int sectionCount = 5;
            const int extraPerSection =
                deltaH > 0 ? deltaH / sectionCount : 0;

            HDWP defer =
                BeginDeferWindowPos(
                    static_cast<int>(m_ctrls.size()));

            int sectionIndex = 0;

            for (const auto& c : m_ctrls)
            {
                int x = c.rc.left;
                int y = c.rc.top;
                int w = c.rc.right - c.rc.left;
                int h = c.rc.bottom - c.rc.top;

                if (c.kind == resize_kind::stretch)
                    w = (std::max)(w + deltaW, 40);
                else if (c.kind == resize_kind::shift)
                    x += deltaW;

                // Expand group boxes vertically when space is available.
                wchar_t cls[16]{};
                GetClassNameW(c.hwnd, cls, 16);

                if (_wcsicmp(cls, L"Button") == 0 &&
                    (GetWindowLongW(c.hwnd, GWL_STYLE) & BS_TYPEMASK)
                    == BS_GROUPBOX)
                {
                    h += extraPerSection;
                    ++sectionIndex;
                }

                if (defer)
                {
                    defer = DeferWindowPos(
                        defer, c.hwnd, nullptr,
                        x, y, w, h,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                else
                {
                    SetWindowPos(
                        c.hwnd, nullptr,
                        x, y, w, h,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }

            if (defer)
                EndDeferWindowPos(defer);

            update_scrollbar_theme();
            InvalidateRect(m_hwnd, nullptr, TRUE);

            m_inLayout = false;
        }

        void browse() const
        {
            const std::wstring start =
                trim_wide(read_dialog_text(m_hwnd, kPrefFolder));

            std::wstring chosen;

            if (pick_folder(m_hwnd, start, chosen))
            {
                SetDlgItemTextW(
                    m_hwnd, kPrefFolder, chosen.c_str());
            }
        }

        static INT_PTR CALLBACK dlg_proc(
            HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* self =
                reinterpret_cast<utag_prefs_instance*>(
                    GetWindowLongPtrW(dlg, DWLP_USER));

            switch (msg)
            {
            case WM_INITDIALOG:
                self =
                    reinterpret_cast<utag_prefs_instance*>(lp);

                SetWindowLongPtrW(
                    dlg, DWLP_USER,
                    reinterpret_cast<LONG_PTR>(self));

                self->m_hwnd = dlg;
                self->m_darkHooks.AddDialogWithControls(dlg);

                self->capture_layout();
                self->load_values();

                return TRUE;

            case WM_SIZE:
                if (self)
                    self->layout();
                return TRUE;

            case WM_COMMAND:
                if (!self || self->m_loading)
                    break;

                switch (LOWORD(wp))
                {
                case kPrefBrowse:
                    if (HIWORD(wp) == BN_CLICKED)
                        self->browse();
                    break;

                case kPrefHandoff:
                case kPrefFavicon:
                case kPrefLogging:
                case kPrefPopups:
                case kPrefTuneinDetails:
                case kPrefCtxLookup:
                    if (HIWORD(wp) == BN_CLICKED)
                        self->m_callback->on_state_changed();
                    break;

                case kPrefFolder:
                case kPrefPlRadioBrowser:
                case kPrefPlTunein:
                case kPrefPlIcecast:
                    if (HIWORD(wp) == EN_CHANGE)
                        self->m_callback->on_state_changed();
                    break;
                }

                break;

            case WM_NCDESTROY:
                if (self)
                    self->m_hwnd = nullptr;

                SetWindowLongPtrW(dlg, DWLP_USER, 0);
                break;
            }

            return FALSE;
        }

        HWND m_hwnd = nullptr;

        bool m_loading = false;
        bool m_inLayout = false;
        bool m_themeSet = false;
        bool m_darkTheme = false;

        int m_designW = 0;
        int m_designH = 0;

        std::vector<ctrl_info> m_ctrls;

        preferences_page_callback::ptr m_callback;
        fb2k::CCoreDarkModeHooks m_darkHooks;
    };

    class utag_prefs_page : public preferences_page_v3
    {
    public:
        const char* get_name() override
        {
            return "Utag Input";
        }

        GUID get_guid() override
        {
            return g_guid_prefs_page;
        }

        GUID get_parent_guid() override
        {
            return preferences_page::guid_tools;
        }

        preferences_page_instance::ptr instantiate(
            fb2k::hwnd_t parent,
            preferences_page_callback::ptr callback) override
        {
            return new service_impl_t<utag_prefs_instance>(
                parent, callback);
        }
    };

    preferences_page_factory_t<utag_prefs_page>
        g_utag_prefs_factory;
}
