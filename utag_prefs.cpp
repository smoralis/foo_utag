#include "utag_ui.h"

namespace
{
    // -------------------------------------------------------------------------
    // Preferences page: Preferences > Tools > Utag Input
    // -------------------------------------------------------------------------
    constexpr int kPrefHandoff = 2001;
    constexpr int kPrefFavicon = 2002;
    constexpr int kPrefFolder = 2003;
    constexpr int kPrefBrowse = 2004;
    constexpr int kPrefLogging = 2005;
    constexpr int kPrefPopups = 2006;
    constexpr int kPrefTuneinDetails = 2007;

    // {85C77DE3-3774-474F-A8EA-87564A3BBDE9}
    static const GUID g_guid_prefs_page =
    { 0x85c77de3, 0x3774, 0x474f, { 0xa8, 0xea, 0x87, 0x56, 0x4a, 0x3b, 0xbd, 0xe9 } };

    class utag_prefs_instance : public preferences_page_instance
    {
    public:
        utag_prefs_instance(HWND parent, preferences_page_callback::ptr callback)
            : m_callback(callback)
        {
            constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;
            constexpr DWORD kGroup = BS_GROUPBOX;
            constexpr DWORD kCheck = BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP;

            dialog_template t;
            t.begin(WS_CHILD | DS_CONTROL, 0, 0, 280, 232, L"", L"Segoe UI", 8);

            t.item(kButton, kGroup, 0, 0, 280, 64, 0xFFFF, L"Playback");
            t.item(kButton, kCheck, 8, 12, 264, 20, kPrefHandoff,
                L"Reuse the running stream when the playing item is retagged (avoids a reconnect pause)");
            t.item(kButton, kCheck, 8, 36, 264, 20, kPrefFavicon,
                L"Download the favicon image during playback (saved next to the .utag file)");

            t.item(kButton, kGroup, 0, 70, 280, 54, 0xFFFF, L"Default folder");
            t.item(kStatic, SS_LEFT, 8, 82, 264, 18, 0xFFFF,
                L"Used by \"Create utag files in default folder\". Leave empty to be asked when needed.");
            t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 102, 198, 13, kPrefFolder, L"");
            t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 212, 101, 60, 14, kPrefBrowse, L"Browse...");

            t.item(kButton, kGroup, 0, 130, 280, 54, 0xFFFF, L"Messages");
            t.item(kButton, kCheck, 8, 142, 264, 10, kPrefLogging,
                L"Write messages to the foobar2000 console");
            t.item(kButton, kCheck, 8, 156, 264, 20, kPrefPopups,
                L"Show a results window after import and create commands (errors are always shown)");

            t.item(kButton, kGroup, 0, 190, 280, 38, 0xFFFF, L"TuneIn");
            t.item(kButton, kCheck, 8, 202, 264, 20, kPrefTuneinDetails,
                L"Look up station details (location, description, genre, region) when importing from TuneIn; one extra request per station");

            m_hwnd = CreateDialogIndirectParamW(
                core_api::get_my_instance(), t.data(), parent,
                &utag_prefs_instance::dlg_proc, reinterpret_cast<LPARAM>(this));
        }

        ~utag_prefs_instance()
        {
            if (m_hwnd && IsWindow(m_hwnd))
                DestroyWindow(m_hwnd);
        }

        fb2k::hwnd_t get_wnd() override { return m_hwnd; }

        t_uint32 get_state() override
        {
            t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
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
            SetDlgItemTextW(m_hwnd, kPrefFolder, L"");
            m_callback->on_state_changed();
        }

    private:
        bool checked(int id) const
        {
            return IsDlgButtonChecked(m_hwnd, id) == BST_CHECKED;
        }

        void set_checked(int id, bool value)
        {
            CheckDlgButton(m_hwnd, id, value ? BST_CHECKED : BST_UNCHECKED);
        }

        std::string folder_utf8() const
        {
            return wide_to_utf8(trim_wide(read_dialog_text(m_hwnd, kPrefFolder)));
        }

        bool has_changed() const
        {
            if (!m_hwnd)
                return false;

            return checked(kPrefHandoff) != static_cast<bool>(g_cfg_handoff)
                || checked(kPrefFavicon) != static_cast<bool>(g_cfg_favicon)
                || checked(kPrefLogging) != static_cast<bool>(g_cfg_logging)
                || checked(kPrefPopups) != static_cast<bool>(g_cfg_result_popups)
                || checked(kPrefTuneinDetails) != static_cast<bool>(g_cfg_tunein_details)
                || folder_utf8() != std::string(g_cfg_default_dir.get_ptr());
        }

        void load_values()
        {
            m_loading = true;
            set_checked(kPrefHandoff, g_cfg_handoff);
            set_checked(kPrefFavicon, g_cfg_favicon);
            set_checked(kPrefLogging, g_cfg_logging);
            set_checked(kPrefPopups, g_cfg_result_popups);
            set_checked(kPrefTuneinDetails, g_cfg_tunein_details);

            try
            {
                SetDlgItemTextW(m_hwnd, kPrefFolder,
                    utf8_to_wide(g_cfg_default_dir.get_ptr()).c_str());
            }
            catch (...)
            {
            }

            SendDlgItemMessageW(m_hwnd, kPrefFolder, EM_SETCUEBANNER, FALSE,
                reinterpret_cast<LPARAM>(L"Ask when needed"));
            m_loading = false;
        }

        void browse()
        {
            const std::wstring start = trim_wide(read_dialog_text(m_hwnd, kPrefFolder));
            std::wstring chosen;
            if (pick_folder(m_hwnd, start, chosen))
                SetDlgItemTextW(m_hwnd, kPrefFolder, chosen.c_str());
        }

        static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* self = reinterpret_cast<utag_prefs_instance*>(
                GetWindowLongPtrW(dlg, DWLP_USER));

            switch (msg)
            {
            case WM_INITDIALOG:
                self = reinterpret_cast<utag_prefs_instance*>(lp);
                SetWindowLongPtrW(dlg, DWLP_USER, lp);
                self->m_hwnd = dlg;
                self->m_darkHooks.AddDialogWithControls(dlg);
                self->load_values();
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
                    if (HIWORD(wp) == BN_CLICKED)
                        self->m_callback->on_state_changed();
                    break;
                case kPrefFolder:
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
        preferences_page_callback::ptr m_callback;
        fb2k::CCoreDarkModeHooks m_darkHooks;
    };

    class utag_prefs_page : public preferences_page_v3
    {
    public:
        const char* get_name() override { return "Utag Input"; }
        GUID get_guid() override { return g_guid_prefs_page; }
        GUID get_parent_guid() override { return preferences_page::guid_tools; }

        preferences_page_instance::ptr instantiate(
            fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override
        {
            return new service_impl_t<utag_prefs_instance>(parent, callback);
        }
    };

    preferences_page_factory_t<utag_prefs_page> g_utag_prefs_factory;
}
