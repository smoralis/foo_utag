#include "utag_tunein.h"

#include <algorithm>
#include <set>
#include <vector>

// -----------------------------------------------------------------------------
// File > Browse TuneIn...
//
// Modeless window to walk TuneIn the way the footuner script's menu does:
// categories, genres, location/language, your presets, searches, recommendations
// by artists and recommendations for the playing station. Tick one or more
// stations and press "Import selected" to create .utag files for them.
// -----------------------------------------------------------------------------

namespace
{
    constexpr int kIdTbTitle = 1301;
    constexpr int kIdTbList = 1302;
    constexpr int kIdTbStatus = 1303;
    constexpr int kIdTbHome = 1304;
    constexpr int kIdTbBack = 1305;
    constexpr int kIdTbImport = 1306;
    constexpr int kIdTbMode = 1307;   // search type drop-down
    constexpr int kIdTbSearch = 1308; // inline search box (edit control)

    // Order of the entries in the search type drop-down.
    enum search_mode { mode_station = 0, mode_artist, mode_song, mode_recommend };

    // Commands on the home level.
    enum home_action
    {
        act_none = 0,
        act_genres,
        act_presets,
        act_username,
        act_search_station,
        act_search_artist,
        act_search_song,
        act_recommend,
        act_station_recs
    };

    struct tunein_level
    {
        std::string title;
        std::vector<tunein_entry> entries;
        bool fromSearchBox = false; // result of a search typed in the search box
    };

    // The window is modeless: state lives on the heap, freed in WM_NCDESTROY.
    struct tunein_browser_state
    {
        fb2k::CCoreDarkModeHooks darkHooks;
        std::vector<tunein_level> stack; // [0] is the home level
    };

    HWND g_tuneinBrowserWnd = nullptr; // the one open window, if any

    // ---- helpers -------------------------------------------------------------------

    // Never throws: window procedures must not let exceptions escape.
    std::wstring to_wide_safe(const std::string& text)
    {
        try
        {
            return utf8_to_wide(text);
        }
        catch (...)
        {
            return {};
        }
    }

    void set_status(HWND dlg, const std::string& text)
    {
        SetDlgItemTextW(dlg, kIdTbStatus, to_wide_safe(text).c_str());
    }

    tunein_entry make_link(const std::string& label, const std::string& pathAndQuery)
    {
        tunein_entry e;
        e.kind = tunein_entry::kind_t::link;
        e.name = label;
        e.label = "> " + label;
        e.url = tunein_url(pathAndQuery);
        return e;
    }

    tunein_entry make_action(const std::string& label, int action)
    {
        tunein_entry e;
        e.kind = tunein_entry::kind_t::action;
        e.name = label;
        e.label = label;
        e.action = action;
        return e;
    }

    tunein_entry make_header(const std::string& text)
    {
        tunein_entry e;
        e.kind = tunein_entry::kind_t::header;
        e.name = text;
        e.label = "-- " + text + " --";
        return e;
    }

    std::string saved_username()
    {
        return ti_trim(g_cfg_tunein_username.get_ptr());
    }

    std::vector<tunein_entry> build_home_entries()
    {
        const std::string user = saved_username();

        std::vector<tunein_entry> home;
        home.push_back(make_header("Browse"));
        home.push_back(make_link("Local", "Browse.ashx?c=local&partnerId=1&render=json"));
        home.push_back(make_link("Music", "Browse.ashx?c=music&partnerId=1&render=json"));
        home.push_back(make_link("Sports", "Browse.ashx?c=sports&partnerId=1&render=json"));
        home.push_back(make_link("Talk", "Browse.ashx?c=talk&partnerId=1&render=json"));
        home.push_back(make_link("By location", "Browse.ashx?c=world&partnerId=1&render=json"));
        home.push_back(make_link("By language", "Browse.ashx?c=lang&partnerId=1&render=json"));
        home.push_back(make_link("Best", "Browse.ashx?c=best&partnerId=1&render=json"));
        home.push_back(make_link("Popular", "Browse.ashx?c=popular&partnerId=1&render=json"));

        tunein_entry genres = make_action("Genres", act_genres);
        genres.label = "> Genres";
        home.push_back(std::move(genres));

        home.push_back(make_header("Your stations"));
        home.push_back(make_action(
            user.empty() ? "My presets (set your username first)" : "My presets: " + user,
            act_presets));
        home.push_back(make_action("Set TuneIn username...", act_username));

        home.push_back(make_header("Search and recommendations"));
        home.push_back(make_action("Search stations...", act_search_station));
        home.push_back(make_action("Search by artist...", act_search_artist));
        home.push_back(make_action("Search by song title...", act_search_song));
        home.push_back(make_action("Recommended stations for a list of artists...", act_recommend));
        home.push_back(make_action("Recommendations for the playing station", act_station_recs));
        return home;
    }

    // TuneIn id of the playing track, if it came from a TuneIn import.
    std::string playing_tunein_id()
    {
        try
        {
            static_api_ptr_t<playback_control> playback;
            static_api_ptr_t<titleformat_compiler> compiler;

            service_ptr_t<titleformat_object> script;
            compiler->compile_safe(script, "[%stream_tunein_id%]");

            pfc::string8 id;
            if (!playback->playback_format_title(nullptr, id, script, nullptr,
                playback_control::display_level_all))
                return {};

            return ti_trim(id.get_ptr());
        }
        catch (...)
        {
            return {};
        }
    }

    // Results are always listed ascending by name (case-insensitive). Section
    // headings keep their place; the entries inside each section are sorted.
    void sort_entries(std::vector<tunein_entry>& entries)
    {
        auto runStart = entries.begin();

        for (auto it = entries.begin();; ++it)
        {
            if (it == entries.end() || it->kind == tunein_entry::kind_t::header)
            {
                std::stable_sort(runStart, it,
                    [](const tunein_entry& a, const tunein_entry& b)
                    {
                        return compare_names(a.name, b.name) < 0;
                    });

                if (it == entries.end())
                    break;

                runStart = it + 1;
            }
        }
    }

    // ---- loading a level -----------------------------------------------------------------

    class tunein_level_task : public threaded_process_callback
    {
    public:
        tunein_level_task(std::string url, bool genres)
            : m_url(std::move(url)), m_genres(genres) {
        }

        void run(threaded_process_status& status, abort_callback& abort) override
        {
            status.set_item("Loading from TuneIn...");

            try
            {
                json response;
                if (!tunein_fetch_json(m_url, abort, response, m_error))
                    return;

                m_title = tunein_head_title(response);

                const auto body = response.find("body");
                if (body == response.end() || !body->is_array())
                {
                    m_error = "unexpected response";
                    return;
                }

                if (m_genres)
                    tunein_parse_genres(*body, m_entries);
                else
                    tunein_parse_entries(*body, m_entries);

                sort_entries(m_entries);
            }
            catch (const exception_aborted&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                m_error = e.what();
            }
            catch (...)
            {
                m_error = "unexpected error";
            }
        }

        void on_done(HWND, bool) override {}

        const std::string& error() const { return m_error; }
        const std::string& title() const { return m_title; }
        std::vector<tunein_entry> take_entries() { return std::move(m_entries); }

    private:
        std::string m_url;
        bool m_genres;
        std::string m_error;
        std::string m_title;
        std::vector<tunein_entry> m_entries;
    };

    // ---- list handling -----------------------------------------------------------------------

    void refresh(HWND dlg, tunein_browser_state& state)
    {
        const HWND list = GetDlgItem(dlg, kIdTbList);
        const tunein_level& level = state.stack.back();

        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& e : level.entries)
            SendMessageW(list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(to_wide_safe(e.label).c_str()));
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);

        std::string path = "TuneIn";
        for (size_t i = 1; i < state.stack.size(); ++i)
            path += "  >  " + state.stack[i].title;
        SetDlgItemTextW(dlg, kIdTbTitle, to_wide_safe(path).c_str());

        const BOOL canGoBack = state.stack.size() > 1 ? TRUE : FALSE;
        EnableWindow(GetDlgItem(dlg, kIdTbBack), canGoBack);
        EnableWindow(GetDlgItem(dlg, kIdTbHome), canGoBack);
    }

    std::vector<int> selected_indices(HWND dlg)
    {
        const HWND list = GetDlgItem(dlg, kIdTbList);
        std::vector<int> result;

        const LRESULT count = SendMessageW(list, LB_GETSELCOUNT, 0, 0);
        if (count == LB_ERR || count <= 0)
            return result;

        result.resize(static_cast<size_t>(count));
        const LRESULT got = SendMessageW(list, LB_GETSELITEMS,
            static_cast<WPARAM>(result.size()), reinterpret_cast<LPARAM>(result.data()));

        if (got == LB_ERR)
            result.clear();
        else
            result.resize(static_cast<size_t>(got));

        return result;
    }

    // Loads `url` (modal progress window, abortable) and opens it as a new level.
    // Returns true when the new level was added.
    bool push_level(HWND dlg, tunein_browser_state& state,
        const std::string& fallbackTitle, const std::string& url, bool genres,
        bool fromSearchBox = false)
    {
        service_ptr_t<tunein_level_task> task =
            new service_impl_t<tunein_level_task>(url, genres);

        // flag_show_delayed: the progress window only appears if loading is slow.
        const bool completed = threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort | threaded_process::flag_show_delayed,
            dlg,
            "Loading from TuneIn...");

        if (!IsWindow(dlg))
            return false;

        if (!completed)
        {
            set_status(dlg, "Cancelled.");
            return false;
        }

        if (!task->error().empty())
        {
            set_status(dlg, "Could not load this list: " + task->error());
            return false;
        }

        tunein_level level;
        level.fromSearchBox = fromSearchBox;
        level.title = task->title().empty() ? fallbackTitle : task->title();
        level.entries = task->take_entries();

        if (level.entries.empty())
        {
            tunein_entry note;
            note.kind = tunein_entry::kind_t::note;
            note.label = "(no entries found)";
            level.entries.push_back(std::move(note));
        }

        state.stack.push_back(std::move(level));
        refresh(dlg, state);
        set_status(dlg, std::to_string(state.stack.back().entries.size()) + " entries.");
        return true;
    }

    // ---- commands ---------------------------------------------------------------------------------

    // Asks for the TuneIn username and stores it. True if a name is now set.
    bool ask_username(HWND dlg, tunein_browser_state& state)
    {
        std::string user = saved_username();
        if (!prompt_text(dlg, L"TuneIn username",
            L"Your TuneIn username (used for My presets):", user))
            return false;

        user = ti_trim(user);
        g_cfg_tunein_username.set_string(user.c_str());

        // The home list shows the name: rebuild it.
        state.stack.front().entries = build_home_entries();
        if (state.stack.size() == 1)
            refresh(dlg, state);

        return !user.empty();
    }

    // ---- search box -----------------------------------------------------------------------

    std::string read_search_box(HWND dlg)
    {
        wchar_t buf[512] = {};
        GetDlgItemTextW(dlg, kIdTbSearch, buf, _countof(buf));

        try
        {
            return ti_trim(wide_to_utf8(buf));
        }
        catch (...)
        {
            return {};
        }
    }

    const wchar_t* search_hint(int mode)
    {
        switch (mode)
        {
        case mode_artist:    return L"Artist name, then press Enter";
        case mode_song:      return L"Song title, then press Enter";
        case mode_recommend: return L"Up to 10 artists separated by commas, then press Enter";
        default:             return L"Station name, then press Enter";
        }
    }

    void update_search_hint(HWND dlg)
    {
        const LRESULT mode = SendDlgItemMessageW(dlg, kIdTbMode, CB_GETCURSEL, 0, 0);
        SendDlgItemMessageW(dlg, kIdTbSearch, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(search_hint(mode == CB_ERR ? mode_station : static_cast<int>(mode))));
    }

    // Selects the search type, then moves to the search box.
    void focus_search(HWND dlg, int mode)
    {
        SendDlgItemMessageW(dlg, kIdTbMode, CB_SETCURSEL, static_cast<WPARAM>(mode), 0);
        update_search_hint(dlg);
        SetFocus(GetDlgItem(dlg, kIdTbSearch));
        SendDlgItemMessageW(dlg, kIdTbSearch, EM_SETSEL, 0, -1);
        set_status(dlg, "Type in the search box and press Enter.");
    }

    // Runs the search typed in the box, using the selected search type.
    void run_search(HWND dlg, tunein_browser_state& state)
    {
        const std::string text = read_search_box(dlg);
        if (text.empty())
        {
            set_status(dlg, "Type in the search box and press Enter.");
            return;
        }

        const LRESULT sel = SendDlgItemMessageW(dlg, kIdTbMode, CB_GETCURSEL, 0, 0);
        const int mode = sel == CB_ERR ? mode_station : static_cast<int>(sel);

        std::string title;
        std::string url;

        if (mode == mode_recommend)
        {
            std::vector<std::string> artists;
            size_t start = 0;
            while (start <= text.size())
            {
                size_t comma = text.find(',', start);
                if (comma == std::string::npos)
                    comma = text.size();

                const std::string artist = ti_trim(text.substr(start, comma - start));
                if (!artist.empty())
                    artists.push_back(artist);

                start = comma + 1;
            }

            if (artists.empty())
                return;

            if (artists.size() > 10)
            {
                set_status(dlg, "You entered more than 10 artists.");
                return;
            }

            std::string query = "Recommend.ashx?partnerId=1";
            for (size_t i = 0; i < artists.size(); ++i)
                query += "&a[" + std::to_string(i) + "]=" + percent_encode(artists[i]);
            query += "&render=json";

            title = "Recommended stations";
            url = tunein_url(query);
        }
        else
        {
            const tunein_kind kind = mode == mode_artist ? tunein_kind::artist
                : mode == mode_song ? tunein_kind::song : tunein_kind::station;
            title = "Search: " + text;
            url = tunein_search_url(kind, text);
        }

        // A new search while a search result is showing replaces it, so Back
        // still returns to the list the search was started from.
        const bool wasSearch = state.stack.size() > 1 && state.stack.back().fromSearchBox;
        if (push_level(dlg, state, title, url, false, true) && wasSearch)
        {
            state.stack.erase(state.stack.end() - 2);
            refresh(dlg, state);
        }
    }

    void run_action(HWND dlg, tunein_browser_state& state, int action)
    {
        switch (action)
        {
        case act_genres:
            push_level(dlg, state, "Genres",
                tunein_url("Describe.ashx?c=genres&partnerId=1&render=json"), true);
            break;

        case act_presets:
        {
            if (saved_username().empty() && !ask_username(dlg, state))
                break;

            push_level(dlg, state, "My presets",
                tunein_url("Browse.ashx?c=presets&partnerId=1&username=" +
                    percent_encode(saved_username()) + "&render=json"),
                false);
            break;
        }

        case act_username:
            if (ask_username(dlg, state))
                set_status(dlg, "Username saved.");
            else
                set_status(dlg, "No username set.");
            break;

        // The search entries move to the search box (with the matching type)
        // instead of opening a popup.
        case act_search_station:
            focus_search(dlg, mode_station);
            break;
        case act_search_artist:
            focus_search(dlg, mode_artist);
            break;
        case act_search_song:
            focus_search(dlg, mode_song);
            break;

        case act_recommend:
            focus_search(dlg, mode_recommend);
            break;

        case act_station_recs:
        {
            const std::string id = playing_tunein_id();
            if (id.empty())
            {
                set_status(dlg, "The playing track has no TuneIn id "
                    "(play a station imported from TuneIn).");
                break;
            }

            push_level(dlg, state, "Recommendations",
                tunein_url("Browse.ashx?id=" + percent_encode(id) +
                    "&partnerId=1&render=json"),
                false);
            break;
        }

        default:
            break;
        }
    }

    void import_selected(HWND dlg, tunein_browser_state& state)
    {
        const tunein_level& level = state.stack.back();

        std::vector<tunein_candidate> candidates;
        std::set<std::string> seen;

        for (const int index : selected_indices(dlg))
        {
            if (index < 0 || static_cast<size_t>(index) >= level.entries.size())
                continue;

            const tunein_entry& e = level.entries[static_cast<size_t>(index)];
            if (e.kind != tunein_entry::kind_t::station)
                continue;

            if (!seen.insert(e.guideId.empty() ? e.url : e.guideId).second)
                continue;

            candidates.push_back({ e.name, e.url, e.image, e.guideId });
        }

        if (candidates.empty())
        {
            set_status(dlg, "Select one or more stations first (entries starting with > are lists).");
            return;
        }

        // May ask for the default folder the first time.
        std::wstring parentFolder;
        EnableWindow(dlg, FALSE);
        const bool haveFolder = get_default_folder(parentFolder, false);
        EnableWindow(dlg, TRUE);
        if (!haveFolder)
            return;

        const std::string description = state.stack.size() > 1
            ? "TuneIn - " + level.title
            : std::string("TuneIn");

        const size_t count = candidates.size();
        tunein_start_import(description, std::move(candidates), parentFolder);
        set_status(dlg, "Importing " + std::to_string(count) + " station(s)...");
    }

    void open_selected(HWND dlg, tunein_browser_state& state)
    {
        const std::vector<int> selected = selected_indices(dlg);
        const tunein_level& level = state.stack.back();

        if (selected.empty() || selected[0] < 0 ||
            static_cast<size_t>(selected[0]) >= level.entries.size())
        {
            set_status(dlg, "Select an entry first.");
            return;
        }

        // Copy: pushing a level may reallocate the stack.
        const tunein_entry entry = level.entries[static_cast<size_t>(selected[0])];

        switch (entry.kind)
        {
        case tunein_entry::kind_t::link:
            push_level(dlg, state, entry.name, entry.url, false);
            break;

        case tunein_entry::kind_t::action:
            run_action(dlg, state, entry.action);
            break;

        case tunein_entry::kind_t::station:
            import_selected(dlg, state);
            break;

        default:
            set_status(dlg, "Nothing to open here.");
            break;
        }
    }

    // ---- window ----------------------------------------------------------------------------------------

    INT_PTR CALLBACK tunein_browser_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<tunein_browser_state*>(
            GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<tunein_browser_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);

            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg); // Tab / Enter / Esc keys

            tunein_level home;
            home.title = "TuneIn";
            home.entries = build_home_entries();
            state->stack.push_back(std::move(home));

            refresh(dlg, *state);
            set_status(dlg, "Open a category, or search. Select stations and press Import selected.");
            for (const wchar_t* item : { L"Station", L"Artist", L"Song title", L"Recommend" })
                SendDlgItemMessageW(dlg, kIdTbMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            SendDlgItemMessageW(dlg, kIdTbMode, CB_SETCURSEL, mode_station, 0);
            update_search_hint(dlg);
            SetFocus(GetDlgItem(dlg, kIdTbList));
            return FALSE; // focus set manually
        }

        case WM_COMMAND:
        {
            if (state == nullptr)
                break;

            const int id = LOWORD(wp);
            const int code = HIWORD(wp);

            if (id == kIdTbMode)
            {
                if (code == CBN_SELCHANGE)
                    update_search_hint(dlg);
                return TRUE;
            }

            if (id == kIdTbSearch)
                return TRUE; // typing only; the search runs on Enter

            {
                const bool inSearch = GetFocus() == GetDlgItem(dlg, kIdTbSearch);
                if (id == IDCANCEL && inSearch && !read_search_box(dlg).empty())
                {
                    SetDlgItemTextW(dlg, kIdTbSearch, L""); // Esc clears the box first
                    return TRUE;
                }
                if (id == IDOK && inSearch)
                {
                    run_search(dlg, *state);
                    return TRUE;
                }
            }

            if (id == IDCANCEL)
            {
                DestroyWindow(dlg);
                return TRUE;
            }

            if (id == IDOK || (id == kIdTbList && code == LBN_DBLCLK))
            {
                open_selected(dlg, *state);
                return TRUE;
            }

            if (id == kIdTbBack && state->stack.size() > 1)
            {
                state->stack.pop_back();
                refresh(dlg, *state);
                set_status(dlg, "");
                return TRUE;
            }

            if (id == kIdTbHome && state->stack.size() > 1)
            {
                state->stack.resize(1);
                refresh(dlg, *state);
                set_status(dlg, "");
                return TRUE;
            }

            if (id == kIdTbImport)
            {
                import_selected(dlg, *state);
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
            if (g_tuneinBrowserWnd == dlg)
                g_tuneinBrowserWnd = nullptr;
            delete state;
            break;

        default:
            break;
        }

        return FALSE;
    }

    // Opens the (modeless) window, or brings the open one to the front.
    void open_tunein_browser(HWND owner)
    {
        if (g_tuneinBrowserWnd != nullptr && IsWindow(g_tuneinBrowserWnd))
        {
            ShowWindow(g_tuneinBrowserWnd, SW_RESTORE);
            SetForegroundWindow(g_tuneinBrowserWnd);
            return;
        }

        constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082, kListBox = 0x0083, kComboBox = 0x0085;

        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 320, 232, L"Browse TuneIn", L"Segoe UI", 8);

        t.item(kStatic, SS_LEFT, 8, 6, 304, 10, kIdTbTitle, L"");
        t.item(kComboBox, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            8, 20, 62, 60, kIdTbMode, L"");
        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
            74, 20, 238, 13, kIdTbSearch, L"");
        t.item(kListBox,
            LBS_EXTENDEDSEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
            8, 37, 304, 153, kIdTbList, L"");
        t.item(kStatic, SS_LEFT, 8, 194, 304, 10, kIdTbStatus, L"");

        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 8, 212, 40, 14, kIdTbHome, L"Home");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 52, 212, 40, 14, kIdTbBack, L"< Back");
        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 96, 212, 40, 14, IDOK, L"Open");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 140, 212, 80, 14, kIdTbImport, L"Import selected");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 262, 212, 50, 14, IDCANCEL, L"Close");

        auto* state = new tunein_browser_state;
        const HWND dlg = CreateDialogIndirectParamW(
            core_api::get_my_instance(), t.data(), owner,
            tunein_browser_proc, reinterpret_cast<LPARAM>(state));

        if (dlg == nullptr)
        {
            delete state;
            return;
        }

        g_tuneinBrowserWnd = dlg;
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
    }
}

void utag_open_tunein_browser(HWND owner)
{
    open_tunein_browser(owner);
}
