#include "utag_radio.h"

#include <algorithm>
#include <vector>

// -----------------------------------------------------------------------------
// File > Browse Radio Browser...
//
// Modeless window to look through radio-browser.info without typing filters:
// top lists (most clicked / most voted / trending / recently changed), or pick
// a tag, country, language or codec from the same filterable pickers used by
// the search window, or search by name. The stations found are listed; select
// some and press "Import selected" to create .utag files for them.
// -----------------------------------------------------------------------------

namespace
{
    constexpr int kIdRbbTitle = 1401;
    constexpr int kIdRbbList = 1402;
    constexpr int kIdRbbStatus = 1403;
    constexpr int kIdRbbHome = 1404;
    constexpr int kIdRbbBack = 1405;
    constexpr int kIdRbbImport = 1406;

    // Stations fetched per list.
    constexpr int kRbbListLimit = 200;

    // Commands on the home level.
    enum home_action
    {
        act_none = 0,
        act_topclick,
        act_topvote,
        act_lastclick,
        act_lastchange,
        act_tag,
        act_country,
        act_language,
        act_codec,
        act_search_name
    };

    struct browse_entry
    {
        enum class kind_t
        {
            header,  // section title, not selectable
            note,    // informational text
            action,  // home level command
            station  // can be imported
        };

        kind_t kind = kind_t::note;
        std::string label;
        int action = 0;
        json station; // the Radio Browser station object
    };

    struct browse_level
    {
        std::string title;
        std::vector<browse_entry> entries;
    };

    // The window is modeless: state lives on the heap, freed in WM_NCDESTROY.
    struct browse_state
    {
        fb2k::CCoreDarkModeHooks darkHooks;
        std::vector<browse_level> stack; // [0] is the home level
    };

    HWND g_radioBrowseWnd = nullptr; // the one open window, if any

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
        SetDlgItemTextW(dlg, kIdRbbStatus, to_wide_safe(text).c_str());
    }

    std::string trim_ascii(const std::string& text)
    {
        const char* ws = " \t\r\n";
        const size_t first = text.find_first_not_of(ws);
        if (first == std::string::npos)
            return {};
        const size_t last = text.find_last_not_of(ws);
        return text.substr(first, last - first + 1);
    }

    std::string station_text(const json& station, const char* key)
    {
        const auto it = station.find(key);
        if (it == station.end() || !it->is_string())
            return {};
        return trim_ascii(it->get<std::string>());
    }

    bool is_http_url(const std::string& url)
    {
        return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    }

    // Builds the list row for a station; false if it can't be imported (no
    // name or no http(s) stream URL).
    bool make_station_entry(const json& station, browse_entry& out)
    {
        if (!station.is_object())
            return false;

        const std::string name = station_text(station, "name");
        std::string url = station_text(station, "url_resolved");
        if (url.empty())
            url = station_text(station, "url");

        if (name.empty() || !is_http_url(url))
            return false;

        std::string info = station_text(station, "codec");

        const auto bitrate = station.find("bitrate");
        if (bitrate != station.end() && bitrate->is_number() && bitrate->get<double>() > 0)
        {
            if (!info.empty())
                info += " ";
            info += std::to_string(static_cast<long long>(bitrate->get<double>())) + " kbps";
        }

        out.kind = browse_entry::kind_t::station;
        out.label = name;
        if (!info.empty())
            out.label += "  [" + info + "]";

        const std::string country = station_text(station, "countrycode");
        if (!country.empty())
            out.label += "  " + country;

        out.station = station;
        return true;
    }

    browse_entry make_header(const std::string& text)
    {
        browse_entry e;
        e.kind = browse_entry::kind_t::header;
        e.label = "-- " + text + " --";
        return e;
    }

    browse_entry make_action(const std::string& label, int action)
    {
        browse_entry e;
        e.kind = browse_entry::kind_t::action;
        e.label = label;
        e.action = action;
        return e;
    }

    std::vector<browse_entry> build_home_entries()
    {
        std::vector<browse_entry> home;
        home.push_back(make_header("Top lists"));
        home.push_back(make_action("> Most clicked", act_topclick));
        home.push_back(make_action("> Most voted", act_topvote));
        home.push_back(make_action("> Trending now (recently clicked)", act_lastclick));
        home.push_back(make_action("> Recently added or changed", act_lastchange));

        home.push_back(make_header("Browse by"));
        home.push_back(make_action("> Tag...", act_tag));
        home.push_back(make_action("> Country...", act_country));
        home.push_back(make_action("> Language...", act_language));
        home.push_back(make_action("> Codec...", act_codec));

        home.push_back(make_header("Search"));
        home.push_back(make_action("Search by name...", act_search_name));
        return home;
    }

    std::string search_path(const std::string& filters)
    {
        return "/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=" +
            std::to_string(kRbbListLimit) + filters;
    }

    std::string top_path(const char* list)
    {
        return std::string("/json/stations/") + list + "/" +
            std::to_string(kRbbListLimit) + "?hidebroken=true";
    }

    // ---- loading a station list ---------------------------------------------------------

    class radio_list_task : public threaded_process_callback
    {
    public:
        explicit radio_list_task(std::string path) : m_path(std::move(path)) {}

        void run(threaded_process_status& status, abort_callback& abort) override
        {
            status.set_item("Loading stations from Radio Browser...");

            try
            {
                std::string body;
                if (!radio_fetch(m_path, abort, body, m_error))
                    return;

                const json response = json::parse(body, nullptr, false);
                if (response.is_discarded() || !response.is_array())
                {
                    m_error = "unexpected response";
                    return;
                }

                for (const auto& station : response)
                {
                    browse_entry entry;
                    if (make_station_entry(station, entry))
                        m_entries.push_back(std::move(entry));
                }

                // Always ascending by station name (case-insensitive).
                std::stable_sort(m_entries.begin(), m_entries.end(),
                    [](const browse_entry& a, const browse_entry& b)
                    {
                        return compare_names(station_text(a.station, "name"),
                            station_text(b.station, "name")) < 0;
                    });
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
        std::vector<browse_entry> take_entries() { return std::move(m_entries); }

    private:
        std::string m_path;
        std::string m_error;
        std::vector<browse_entry> m_entries;
    };

    // ---- list handling -----------------------------------------------------------------

    void refresh(HWND dlg, browse_state& state)
    {
        const HWND list = GetDlgItem(dlg, kIdRbbList);
        const browse_level& level = state.stack.back();

        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& e : level.entries)
            SendMessageW(list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(to_wide_safe(e.label).c_str()));
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);

        std::string path = "Radio Browser";
        for (size_t i = 1; i < state.stack.size(); ++i)
            path += "  >  " + state.stack[i].title;
        SetDlgItemTextW(dlg, kIdRbbTitle, to_wide_safe(path).c_str());

        const BOOL canGoBack = state.stack.size() > 1 ? TRUE : FALSE;
        EnableWindow(GetDlgItem(dlg, kIdRbbBack), canGoBack);
        EnableWindow(GetDlgItem(dlg, kIdRbbHome), canGoBack);
    }

    std::vector<int> selected_indices(HWND dlg)
    {
        const HWND list = GetDlgItem(dlg, kIdRbbList);
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

    // Loads `path` (modal progress window, abortable) and shows the stations
    // as a new level.
    void push_stations(HWND dlg, browse_state& state, const std::string& title,
        const std::string& path)
    {
        service_ptr_t<radio_list_task> task = new service_impl_t<radio_list_task>(path);

        // flag_show_delayed: the progress window only appears if loading is slow.
        const bool completed = threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort | threaded_process::flag_show_delayed,
            dlg,
            "Loading stations from Radio Browser...");

        if (!IsWindow(dlg))
            return;

        if (!completed)
        {
            set_status(dlg, "Cancelled.");
            return;
        }

        if (!task->error().empty())
        {
            set_status(dlg, "Could not load stations: " + task->error());
            return;
        }

        browse_level level;
        level.title = title;
        level.entries = task->take_entries();

        if (level.entries.empty())
        {
            browse_entry note;
            note.kind = browse_entry::kind_t::note;
            note.label = "(no stations found)";
            level.entries.push_back(std::move(note));
        }

        state.stack.push_back(std::move(level));
        refresh(dlg, state);
        set_status(dlg, std::to_string(state.stack.back().entries.size()) + " entries.");
    }

    // ---- commands ---------------------------------------------------------------------------

    void run_action(HWND dlg, browse_state& state, int action)
    {
        std::string value;

        switch (action)
        {
        case act_topclick:
            push_stations(dlg, state, "Most clicked", top_path("topclick"));
            break;
        case act_topvote:
            push_stations(dlg, state, "Most voted", top_path("topvote"));
            break;
        case act_lastclick:
            push_stations(dlg, state, "Trending now", top_path("lastclick"));
            break;
        case act_lastchange:
            push_stations(dlg, state, "Recently changed", top_path("lastchange"));
            break;

        case act_tag:
            if (radio_pick_tag(dlg, value) && !value.empty())
                push_stations(dlg, state, "Tag: " + value,
                    search_path("&tag=" + percent_encode(value) + "&tagExact=true"));
            break;

        case act_country:
            if (radio_pick_country(dlg, value) && !value.empty())
                push_stations(dlg, state, "Country: " + value,
                    search_path("&countrycode=" + percent_encode(value)));
            break;

        case act_language:
            if (radio_pick_language(dlg, value) && !value.empty())
                push_stations(dlg, state, "Language: " + value,
                    search_path("&language=" + percent_encode(value) + "&languageExact=true"));
            break;

        case act_codec:
            if (radio_pick_codec(dlg, value) && !value.empty())
                push_stations(dlg, state, "Codec: " + value,
                    search_path("&codec=" + percent_encode(value) + "&codecExact=true"));
            break;

        case act_search_name:
            if (prompt_text(dlg, L"Search Radio Browser", L"Station name contains:", value) &&
                !value.empty())
                push_stations(dlg, state, "Search: " + value,
                    search_path("&name=" + percent_encode(value)));
            break;

        default:
            break;
        }
    }

    void import_selected(HWND dlg, browse_state& state)
    {
        const browse_level& level = state.stack.back();

        std::vector<json> stations;
        for (const int index : selected_indices(dlg))
        {
            if (index < 0 || static_cast<size_t>(index) >= level.entries.size())
                continue;

            const browse_entry& e = level.entries[static_cast<size_t>(index)];
            if (e.kind == browse_entry::kind_t::station)
                stations.push_back(e.station);
        }

        if (stations.empty())
        {
            set_status(dlg, "Select one or more stations first.");
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
            ? "Radio Browser - " + level.title
            : std::string("Radio Browser");

        const size_t count = stations.size();
        radio_start_import_stations(description, std::move(stations), parentFolder);
        set_status(dlg, "Importing " + std::to_string(count) + " station(s)...");
    }

    void open_selected(HWND dlg, browse_state& state)
    {
        const std::vector<int> selected = selected_indices(dlg);
        const browse_level& level = state.stack.back();

        if (selected.empty() || selected[0] < 0 ||
            static_cast<size_t>(selected[0]) >= level.entries.size())
        {
            set_status(dlg, "Select an entry first.");
            return;
        }

        // Copy: pushing a level may reallocate the stack.
        const browse_entry::kind_t kind = level.entries[static_cast<size_t>(selected[0])].kind;
        const int action = level.entries[static_cast<size_t>(selected[0])].action;

        switch (kind)
        {
        case browse_entry::kind_t::action:
            run_action(dlg, state, action);
            break;

        case browse_entry::kind_t::station:
            import_selected(dlg, state);
            break;

        default:
            set_status(dlg, "Nothing to open here.");
            break;
        }
    }

    // ---- window ----------------------------------------------------------------------------------

    INT_PTR CALLBACK radio_browse_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<browse_state*>(GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<browse_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);

            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg); // Tab / Enter / Esc keys

            browse_level home;
            home.title = "Radio Browser";
            home.entries = build_home_entries();
            state->stack.push_back(std::move(home));

            refresh(dlg, *state);
            set_status(dlg, "Open a list or search. Select stations and press Import selected.");
            SetFocus(GetDlgItem(dlg, kIdRbbList));
            return FALSE; // focus set manually
        }

        case WM_COMMAND:
        {
            if (state == nullptr)
                break;

            const int id = LOWORD(wp);
            const int code = HIWORD(wp);

            if (id == IDCANCEL)
            {
                DestroyWindow(dlg);
                return TRUE;
            }

            if (id == IDOK || (id == kIdRbbList && code == LBN_DBLCLK))
            {
                open_selected(dlg, *state);
                return TRUE;
            }

            if (id == kIdRbbBack && state->stack.size() > 1)
            {
                state->stack.pop_back();
                refresh(dlg, *state);
                set_status(dlg, "");
                return TRUE;
            }

            if (id == kIdRbbHome && state->stack.size() > 1)
            {
                state->stack.resize(1);
                refresh(dlg, *state);
                set_status(dlg, "");
                return TRUE;
            }

            if (id == kIdRbbImport)
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
            if (g_radioBrowseWnd == dlg)
                g_radioBrowseWnd = nullptr;
            delete state;
            break;

        default:
            break;
        }

        return FALSE;
    }

    // Opens the (modeless) window, or brings the open one to the front.
    void open_radio_browse(HWND owner)
    {
        if (g_radioBrowseWnd != nullptr && IsWindow(g_radioBrowseWnd))
        {
            ShowWindow(g_radioBrowseWnd, SW_RESTORE);
            SetForegroundWindow(g_radioBrowseWnd);
            return;
        }

        constexpr WORD kButton = 0x0080, kStatic = 0x0082, kListBox = 0x0083;

        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 320, 232, L"Browse Radio Browser", L"Segoe UI", 8);

        t.item(kStatic, SS_LEFT, 8, 6, 304, 10, kIdRbbTitle, L"");
        t.item(kListBox,
            LBS_EXTENDEDSEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
            8, 20, 304, 170, kIdRbbList, L"");
        t.item(kStatic, SS_LEFT, 8, 194, 304, 10, kIdRbbStatus, L"");

        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 8, 212, 40, 14, kIdRbbHome, L"Home");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 52, 212, 40, 14, kIdRbbBack, L"< Back");
        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 96, 212, 40, 14, IDOK, L"Open");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 140, 212, 80, 14, kIdRbbImport, L"Import selected");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 262, 212, 50, 14, IDCANCEL, L"Close");

        auto* state = new browse_state;
        const HWND dlg = CreateDialogIndirectParamW(
            core_api::get_my_instance(), t.data(), owner,
            radio_browse_proc, reinterpret_cast<LPARAM>(state));

        if (dlg == nullptr)
        {
            delete state;
            return;
        }

        g_radioBrowseWnd = dlg;
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
    }
}

void utag_open_radio_browse(HWND owner)
{
    open_radio_browse(owner);
}
