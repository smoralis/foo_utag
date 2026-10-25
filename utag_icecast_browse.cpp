#include "utag_radio.h"

#include <algorithm>
#include <string>
#include <vector>

// Icecast Yellow Pages browser. The directory exposes a machine-readable XML
// export at https://dir.xiph.org/yp.xml. Results are fetched on demand and
// kept only in memory; station objects are passed to the existing .utag importer.

namespace
{
    constexpr int kIdIceTitle = 1501;
    constexpr int kIdIceList = 1502;
    constexpr int kIdIceStatus = 1503;
    constexpr int kIdIceSearch = 1504; // inline search box (edit control)
    constexpr int kIdIceRefresh = 1505;
    constexpr int kIdIceImport = 1506;

    // Separate import-style dialog; the directory browser remains available.
    constexpr int kIdIceImportQuery = 1511;
    constexpr int kIdIceImportLimit = 1512;
    constexpr int kIdIceImportGo = 1513;

    struct ice_station
    {
        std::string name;
        std::string url;
        std::string genre;
        std::string codec;
        std::string description;
        std::string current_song;
        std::string bitrate;
        json as_json;
        std::string label;
    };

    struct ice_state
    {
        fb2k::CCoreDarkModeHooks darkHooks;
        std::vector<ice_station> all;
        std::vector<size_t> visible;
        std::string filter;
        bool fromCache = false; // list came from the session cache, not the network
    };

    HWND g_iceWnd = nullptr;

    // The parsed directory is kept for the rest of the foobar2000 session so the
    // browser and the import dialog do not download the whole XML every time.
    // Only touched from the main thread. "Refresh" bypasses and replaces it.
    struct ice_cache
    {
        std::vector<ice_station> stations;
        bool valid = false;
    };
    ice_cache g_iceCache;

    std::wstring to_wide_safe(const std::string& text)
    {
        try { return utf8_to_wide(text); }
        catch (...) { return {}; }
    }

    std::string trim(const std::string& s)
    {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::string xml_decode(std::string s)
    {
        const std::pair<const char*, const char*> entities[] = {
            {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
            {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"}
        };
        for (const auto& e : entities) {
            size_t pos = 0;
            while ((pos = s.find(e.first, pos)) != std::string::npos) {
                s.replace(pos, std::strlen(e.first), e.second);
                pos += std::strlen(e.second);
            }
        }
        return s;
    }

    std::string xml_field(const std::string& block, const char* tag)
    {
        const std::string open = std::string("<") + tag + ">";
        const std::string close = std::string("</") + tag + ">";
        size_t start = block.find(open);
        if (start == std::string::npos) {
            // Some XML serializers add attributes to elements.
            const std::string prefix = std::string("<") + tag;
            start = block.find(prefix);
            if (start == std::string::npos) return {};
            start = block.find('>', start);
            if (start == std::string::npos) return {};
            ++start;
        } else {
            start += open.size();
        }
        const size_t end = block.find(close, start);
        if (end == std::string::npos) return {};
        return trim(xml_decode(block.substr(start, end - start)));
    }

    bool http_url(const std::string& s)
    {
        return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
    }

    bool fetch_icecast_xml(abort_callback& abort, std::string& body, std::string& error)
    {
        try {
            static_api_ptr_t<http_client> client;
            auto request = client->create_request("GET");
            request->add_header("Accept: application/xml, text/xml, */*");
            auto stream = request->run_ex("https://dir.xiph.org/yp.xml", abort);

            constexpr size_t maxBytes = 64u * 1024u * 1024u;
            std::vector<char> buffer(16384);
            std::string data;
            for (;;) {
                abort.check();
                const t_size got = stream->read(buffer.data(), buffer.size(), abort);
                if (got == 0) break;
                data.append(buffer.data(), static_cast<size_t>(got));
                if (data.size() > maxBytes) {
                    error = "Icecast directory XML is larger than 64 MB.";
                    return false;
                }
            }
            if (data.empty()) {
                error = "The Icecast directory returned an empty response.";
                return false;
            }
            body = std::move(data);
            return true;
        }
        catch (const exception_aborted&) { throw; }
        catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "Could not download the Icecast directory."; }
        return false;
    }

    std::vector<ice_station> parse_icecast_xml(const std::string& xml)
    {
        std::vector<ice_station> result;
        size_t pos = 0;
        while ((pos = xml.find("<entry", pos)) != std::string::npos) {
            size_t begin = xml.find('>', pos);
            if (begin == std::string::npos) break;
            ++begin;
            size_t end = xml.find("</entry>", begin);
            if (end == std::string::npos) break;
            const std::string block = xml.substr(begin, end - begin);
            pos = end + 8;

            ice_station s;
            s.name = xml_field(block, "server_name");
            if (s.name.empty()) s.name = xml_field(block, "name");
            s.url = xml_field(block, "listen_url");
            if (s.url.empty()) s.url = xml_field(block, "listenurl");
            if (s.name.empty() || !http_url(s.url)) continue;

            s.genre = xml_field(block, "genre");
            s.codec = xml_field(block, "server_type");
            if (s.codec.empty()) s.codec = xml_field(block, "type");
            s.description = xml_field(block, "server_description");
            if (s.description.empty()) s.description = xml_field(block, "description");
            s.current_song = xml_field(block, "current_song");
            if (s.current_song.empty()) s.current_song = xml_field(block, "song");
            s.bitrate = xml_field(block, "bitrate");

            std::string meta;
            if (!s.genre.empty()) meta = s.genre;
            if (!s.codec.empty()) { if (!meta.empty()) meta += " | "; meta += s.codec; }
            if (!s.bitrate.empty()) { if (!meta.empty()) meta += " | "; meta += s.bitrate + " kbps"; }
            s.label = s.name + (meta.empty() ? "" : "  [" + meta + "]");

            s.as_json = json::object();
            s.as_json["name"] = s.name;
            s.as_json["url"] = s.url;
            s.as_json["url_resolved"] = s.url;
            s.as_json["homepage"] = xml_field(block, "server_url");
            s.as_json["tags"] = s.genre;
            s.as_json["codec"] = s.codec;
            s.as_json["bitrate"] = std::atoi(s.bitrate.c_str());
            s.as_json["description"] = s.description;
            s.as_json["current_song"] = s.current_song;
            result.push_back(std::move(s));
        }

        std::stable_sort(result.begin(), result.end(),
            [](const ice_station& a, const ice_station& b) {
                return compare_names(a.name, b.name) < 0;
            });
        return result;
    }

    class icecast_load_task : public threaded_process_callback
    {
    public:
        void run(threaded_process_status& status, abort_callback& abort) override
        {
            status.set_item("Downloading Icecast directory...");
            std::string xml;
            if (!fetch_icecast_xml(abort, xml, m_error)) return;
            status.set_item("Parsing Icecast stations...");
            try {
                m_stations = parse_icecast_xml(xml);
                if (m_stations.empty() && m_error.empty())
                    m_error = "No stations found. The directory XML format may have changed.";
            }
            catch (const std::exception& e) { m_error = e.what(); }
            catch (...) { m_error = "Could not parse the Icecast directory XML."; }
        }
        void on_done(HWND, bool) override {}
        const std::string& error() const { return m_error; }
        std::vector<ice_station> take() { return std::move(m_stations); }
    private:
        std::string m_error;
        std::vector<ice_station> m_stations;
    };

    // Returns the directory from the session cache, or downloads it (modal, with
    // an Abort button) and caches it. `force` skips the cache. Returns false when
    // cancelled (`error` empty) or on failure (`error` set).
    bool get_directory(HWND parent, const char* progressTitle, bool force,
        std::vector<ice_station>& out, std::string& error, bool& fromCache)
    {
        error.clear();
        fromCache = false;

        if (!force && g_iceCache.valid)
        {
            out = g_iceCache.stations;
            fromCache = true;
            return true;
        }

        service_ptr_t<icecast_load_task> task = new service_impl_t<icecast_load_task>();
        const bool completed = threaded_process::g_run_modal(task,
            threaded_process::flag_show_abort | threaded_process::flag_show_delayed,
            parent, progressTitle);
        if (!completed) return false;
        if (!task->error().empty()) { error = task->error(); return false; }

        out = task->take();
        g_iceCache.stations = out;
        g_iceCache.valid = true;
        return true;
    }

    void set_status(HWND dlg, const std::string& s)
    {
        SetDlgItemTextW(dlg, kIdIceStatus, to_wide_safe(s).c_str());
    }

    std::string read_search_box(HWND dlg)
    {
        wchar_t buf[512] = {};
        GetDlgItemTextW(dlg, kIdIceSearch, buf, _countof(buf));
        return trim(wide_to_utf8(buf));
    }

    void render(HWND dlg, ice_state& state)
    {
        HWND list = GetDlgItem(dlg, kIdIceList);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        state.visible.clear();

        const std::string needle = lowercase_ascii(state.filter);
        for (size_t i = 0; i < state.all.size(); ++i) {
            const auto& s = state.all[i];
            const std::string haystack = lowercase_ascii(
                s.name + " " + s.genre + " " + s.codec + " " + s.description + " " + s.current_song);
            if (!needle.empty() && haystack.find(needle) == std::string::npos) continue;
            state.visible.push_back(i);
            SendMessageW(list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(to_wide_safe(s.label).c_str()));
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        set_status(dlg, std::to_string(state.visible.size()) + " stations shown (" +
            std::to_string(state.all.size()) + " loaded" +
            (state.fromCache ? ", cached this session - Refresh to reload" : "") + ").");
    }

    std::vector<int> selected_indices(HWND dlg)
    {
        HWND list = GetDlgItem(dlg, kIdIceList);
        LRESULT count = SendMessageW(list, LB_GETSELCOUNT, 0, 0);
        if (count == LB_ERR || count <= 0) return {};
        std::vector<int> indices(static_cast<size_t>(count));
        LRESULT got = SendMessageW(list, LB_GETSELITEMS, static_cast<WPARAM>(indices.size()),
            reinterpret_cast<LPARAM>(indices.data()));
        if (got == LB_ERR) return {};
        indices.resize(static_cast<size_t>(got));
        return indices;
    }

    void load_directory(HWND dlg, ice_state& state, bool force)
    {
        std::vector<ice_station> stations;
        std::string error;
        bool fromCache = false;
        const bool ok = get_directory(dlg, "Loading Icecast directory...", force,
            stations, error, fromCache);
        if (!IsWindow(dlg)) return;
        if (!ok)
        {
            set_status(dlg, error.empty() ? std::string("Cancelled.")
                                          : "Could not load directory: " + error);
            return;
        }
        state.all = std::move(stations);
        state.fromCache = fromCache;
        state.filter = read_search_box(dlg);
        render(dlg, state);
    }

    // Re-filters the list from the search box; only redraws when the text
    // actually changed (the directory is in memory, so this is instant).
    void apply_search_box(HWND dlg, ice_state& state)
    {
        const std::string query = read_search_box(dlg);
        if (query == state.filter) return;
        state.filter = query;
        render(dlg, state);
    }

    void import_selected(HWND dlg, ice_state& state)
    {
        std::vector<json> stations;
        for (int index : selected_indices(dlg)) {
            if (index < 0 || static_cast<size_t>(index) >= state.visible.size()) continue;
            stations.push_back(state.all[state.visible[static_cast<size_t>(index)]].as_json);
        }
        if (stations.empty()) { set_status(dlg, "Select one or more stations first."); return; }

        std::wstring folder;
        EnableWindow(dlg, FALSE);
        const bool haveFolder = get_default_folder(folder, false);
        EnableWindow(dlg, TRUE);
        if (!haveFolder) return;

        const size_t count = stations.size();
        radio_start_import_stations("Icecast Directory", "Icecast Directory", std::move(stations), folder, true);
        set_status(dlg, "Importing " + std::to_string(count) + " station(s)...");
    }

    INT_PTR CALLBACK icecast_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<ice_state*>(GetWindowLongPtrW(dlg, DWLP_USER));
        switch (msg) {
        case WM_INITDIALOG:
            state = reinterpret_cast<ice_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg);
            SendDlgItemMessageW(dlg, kIdIceSearch, EM_SETCUEBANNER, TRUE,
                reinterpret_cast<LPARAM>(L"Search name, genre, codec, description or current song"));
            SetFocus(GetDlgItem(dlg, kIdIceSearch));
            return FALSE;

        case WM_COMMAND:
            if (!state) break;
            if (LOWORD(wp) == kIdIceSearch) {
                if (HIWORD(wp) == EN_CHANGE) apply_search_box(dlg, *state);
                return TRUE;
            }
            {
                const bool inSearch = GetFocus() == GetDlgItem(dlg, kIdIceSearch);
                if (LOWORD(wp) == IDCANCEL && inSearch && !read_search_box(dlg).empty()) {
                    SetDlgItemTextW(dlg, kIdIceSearch, L""); // Esc clears the box first
                    return TRUE;
                }
                if (LOWORD(wp) == IDOK && inSearch) {
                    apply_search_box(dlg, *state); // Enter applies and moves to the list
                    SetFocus(GetDlgItem(dlg, kIdIceList));
                    return TRUE;
                }
            }
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(dlg); return TRUE; }
            if (LOWORD(wp) == kIdIceRefresh) { load_directory(dlg, *state, true); return TRUE; }
            if (LOWORD(wp) == kIdIceImport) { import_selected(dlg, *state); return TRUE; }
            if (LOWORD(wp) == IDOK || (LOWORD(wp) == kIdIceList && HIWORD(wp) == LBN_DBLCLK)) {
                import_selected(dlg, *state); return TRUE;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(dlg);
            return TRUE;

        case WM_NCDESTROY:
            modeless_dialog_manager::g_remove(dlg);
            SetWindowLongPtrW(dlg, DWLP_USER, 0);
            if (g_iceWnd == dlg) g_iceWnd = nullptr;
            delete state;
            break;
        }
        return FALSE;
    }

    void open_icecast_browser(HWND owner)
    {
        if (g_iceWnd && IsWindow(g_iceWnd)) {
            ShowWindow(g_iceWnd, SW_RESTORE);
            SetForegroundWindow(g_iceWnd);
            return;
        }
        constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082, kListBox = 0x0083;
        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 430, 270, L"Browse Icecast Directory", L"Segoe UI", 8);
        t.item(kStatic, SS_LEFT, 8, 6, 414, 12, kIdIceTitle, L"Icecast Directory");
        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
            8, 20, 414, 13, kIdIceSearch, L"");
        t.item(kListBox, LBS_EXTENDEDSEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            WS_VSCROLL | WS_HSCROLL | WS_BORDER | WS_TABSTOP,
            8, 37, 414, 190, kIdIceList, L"");
        t.item(kStatic, SS_LEFT, 8, 230, 414, 12, kIdIceStatus, L"Loading...");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 8, 248, 56, 14, kIdIceRefresh, L"Refresh");
        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 68, 248, 100, 14, kIdIceImport, L"Import selected");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 372, 248, 50, 14, IDCANCEL, L"Close");

        auto* state = new ice_state;
        HWND dlg = CreateDialogIndirectParamW(core_api::get_my_instance(), t.data(),
            owner, icecast_proc, reinterpret_cast<LPARAM>(state));
        if (!dlg) { delete state; return; }
        g_iceWnd = dlg;
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
        load_directory(dlg, *state, false);
    }

    struct ice_import_prompt_state
    {
        fb2k::CCoreDarkModeHooks darkHooks;
    };

    INT_PTR CALLBACK icecast_import_prompt_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<ice_import_prompt_state*>(GetWindowLongPtrW(dlg, DWLP_USER));
        switch (msg) {
        case WM_INITDIALOG:
            state = reinterpret_cast<ice_import_prompt_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg);
            SetFocus(GetDlgItem(dlg, kIdIceImportQuery));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) { DestroyWindow(dlg); return TRUE; }
            if (LOWORD(wp) == IDOK || LOWORD(wp) == kIdIceImportGo) {
                wchar_t queryBuf[512] = {};
                wchar_t limitBuf[32] = {};
                GetDlgItemTextW(dlg, kIdIceImportQuery, queryBuf, _countof(queryBuf));
                GetDlgItemTextW(dlg, kIdIceImportLimit, limitBuf, _countof(limitBuf));
                long limit = 100;
                if (limitBuf[0]) {
                    wchar_t* end = nullptr;
                    const long parsed = std::wcstol(limitBuf, &end, 10);
                    if (!end || *end || parsed < 1 || parsed > 5000) {
                        MessageBoxW(dlg, L"Maximum stations must be between 1 and 5000.", L"Icecast import", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                    limit = parsed;
                }
                const std::string query = lowercase_ascii(wide_to_utf8(queryBuf));
                DestroyWindow(dlg);

                std::vector<ice_station> all;
                std::string loadError;
                bool fromCache = false;
                if (!get_directory(core_api::get_main_window(),
                        "Loading Icecast directory for import...", false,
                        all, loadError, fromCache)) {
                    if (!loadError.empty())
                        popup_message::g_show(loadError.c_str(), "Icecast import", popup_message::icon_error);
                    return TRUE;
                }

                std::vector<json> selected;
                for (const auto& station : all) {
                    const std::string haystack = lowercase_ascii(station.name + " " + station.genre + " " +
                        station.codec + " " + station.description + " " + station.current_song);
                    if (!query.empty() && haystack.find(query) == std::string::npos) continue;
                    selected.push_back(station.as_json);
                    if (selected.size() >= static_cast<size_t>(limit)) break;
                }
                if (selected.empty()) {
                    popup_message::g_show("No Icecast stations matched the search.", "Icecast import", popup_message::icon_information);
                    return TRUE;
                }

                std::wstring folder;
                if (!get_default_folder(folder, false)) return TRUE;
                const size_t count = selected.size();
                radio_start_import_stations("Icecast Directory",
                    query.empty() ? std::string("Icecast Directory")
                                  : "Icecast Directory - " + query,
                    std::move(selected), folder, false);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(dlg);
            return TRUE;
        case WM_NCDESTROY:
            modeless_dialog_manager::g_remove(dlg);
            SetWindowLongPtrW(dlg, DWLP_USER, 0);
            delete state;
            break;
        }
        return FALSE;
    }

    void open_icecast_import(HWND owner)
    {
        constexpr WORD kButton = 0x0080, kStatic = 0x0082, kEdit = 0x0081;
        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 330, 112, L"Import stations from Icecast", L"Segoe UI", 8);
        t.item(kStatic, SS_LEFT, 8, 8, 100, 12, 0, L"Search terms:");
        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 22, 314, 18,
            kIdIceImportQuery, L"");
        t.item(kStatic, SS_LEFT, 8, 45, 150, 12, 0, L"Maximum matching stations (1-5000):");
        t.item(kEdit, ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER | WS_TABSTOP, 8, 59, 70, 18,
            kIdIceImportLimit, L"100");
        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 190, 82, 65, 18, kIdIceImportGo, L"Import");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 260, 82, 62, 18, IDCANCEL, L"Cancel");
        auto* state = new ice_import_prompt_state;
        HWND dlg = CreateDialogIndirectParamW(core_api::get_my_instance(), t.data(),
            owner, icecast_import_prompt_proc, reinterpret_cast<LPARAM>(state));
        if (!dlg) { delete state; return; }
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
    }
}

void utag_open_icecast_browse(HWND owner)
{
    open_icecast_browser(owner);
}

void utag_import_from_icecast(HWND owner)
{
    open_icecast_import(owner);
}
