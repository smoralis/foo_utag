#include "utag_ui.h"
#include "utag_radio.h"

// -----------------------------------------------------------------------------
// Main menu command: File > Add internet radio url and create utag
// Asks for a stream URL (and an optional station name) in a small input box,
// then writes a .utag file for it into the default utag folder.
// -----------------------------------------------------------------------------

namespace
{
    constexpr int kIdUrlEdit = 1001;
    constexpr int kIdNameEdit = 1002;
    constexpr int kIdStatus = 1003;
    constexpr int kIdLookupCheck = 1004;

    // The window is modeless (foobar2000 stays usable while it is open), so
    // its state lives on the heap and is freed in WM_NCDESTROY.
    struct url_prompt_state
    {
        // Follows foobar2000's own dark mode setting (no-op on pre-2.0 builds).
        fb2k::CCoreDarkModeHooks darkHooks;
    };

    HWND g_stationPromptWnd = nullptr; // the one open window, if any

    // Defined further down: creates the .utag in the background.
    void start_station_creation(const std::wstring& url, const std::wstring& name,
        const std::wstring& folder, bool radioBrowserLookup);

    INT_PTR CALLBACK url_prompt_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<url_prompt_state*>(
            GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<url_prompt_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            // Dark mode for the dialog and all of its controls.
            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg); // Tab / Enter / Esc keys

            CheckDlgButton(dlg, kIdLookupCheck,
                g_cfg_add_rb_lookup ? BST_CHECKED : BST_UNCHECKED);

            SetFocus(GetDlgItem(dlg, kIdUrlEdit));
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
                const std::wstring url = trim_wide(read_dialog_text(dlg, kIdUrlEdit));

                if (url.find(L"://") == std::wstring::npos)
                {
                    // Inline hint instead of a (light-themed) message box.
                    SetDlgItemTextW(dlg, kIdStatus,
                        L"Enter a full URL, e.g. http://host:8000/stream");
                    SetFocus(GetDlgItem(dlg, kIdUrlEdit));
                    return TRUE;
                }

                const std::wstring name = trim_wide(read_dialog_text(dlg, kIdNameEdit));

                // May ask for the default folder the first time; keep this
                // window open if that is cancelled.
                std::wstring folder;
                EnableWindow(dlg, FALSE);
                const bool haveFolder = get_default_folder(folder, false);
                EnableWindow(dlg, TRUE);

                if (!haveFolder)
                    return TRUE;

                const bool lookup = IsDlgButtonChecked(dlg, kIdLookupCheck) == BST_CHECKED;
                g_cfg_add_rb_lookup = lookup; // remembered for next time

                DestroyWindow(dlg);
                start_station_creation(url, name, folder, lookup);
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
            if (g_stationPromptWnd == dlg)
                g_stationPromptWnd = nullptr;
            delete state;
            break;

        default:
            break;
        }

        return FALSE;
    }

    // Opens the (modeless) window, or brings the open one to the front.
    void open_station_dialog(HWND owner)
    {
        if (g_stationPromptWnd != nullptr && IsWindow(g_stationPromptWnd))
        {
            ShowWindow(g_stationPromptWnd, SW_RESTORE);
            SetForegroundWindow(g_stationPromptWnd);
            return;
        }

        constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;

        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 260, 126, L"Add internet radio url and create utag", L"Segoe UI", 9);
        t.item(kStatic, SS_LEFT, 8, 8, 244, 10, 0xFFFF, L"Station URL:");
        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 20, 244, 13,
            kIdUrlEdit, L"");
        t.item(kStatic, SS_LEFT, 8, 40, 244, 10, 0xFFFF, L"Station name (optional; read from stream if empty):");
        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 52, 244, 13,
            kIdNameEdit, L"");
        t.item(kButton, BS_AUTOCHECKBOX | WS_TABSTOP, 8, 70, 244, 11, kIdLookupCheck,
            L"Search Radio Browser for this URL (use its station details)");
        t.item(kStatic, SS_LEFT, 8, 86, 244, 10, kIdStatus, L"");
        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 104, 50, 14, IDOK, L"Create");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 202, 104, 50, 14, IDCANCEL, L"Cancel");

        auto* state = new url_prompt_state;
        const HWND dlg = CreateDialogIndirectParamW(
            core_api::get_my_instance(), t.data(), owner,
            url_prompt_proc, reinterpret_cast<LPARAM>(state));

        if (dlg == nullptr)
        {
            delete state;
            return;
        }

        g_stationPromptWnd = dlg;
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
    }

    // "http://host:8000/stream" -> "host_8000" (used when no name is given).
    std::string station_label_from_url(const std::string& url)
    {
        std::string rest = url;
        const auto scheme = rest.find("://");
        if (scheme != std::string::npos)
            rest = rest.substr(scheme + 3);

        const auto at = rest.find('@');
        const auto slash = rest.find('/');
        if (at != std::string::npos && (slash == std::string::npos || at < slash))
            rest = rest.substr(at + 1); // drop user:pass@

        const auto end = rest.find_first_of("/?#");
        if (end != std::string::npos)
            rest = rest.substr(0, end);

        return rest.empty() ? std::string("Station") : rest;
    }

    struct station_probe_result
    {
        std::string name;   // station name from the stream headers, if any
        std::string error;  // set when the server could not be reached
        bool finished = false; // false when the user aborted the lookup
    };

    std::string trim_ascii_text(const char* text)
    {
        std::string result = text != nullptr ? text : "";
        const char* ws = " \t\r\n";
        const size_t first = result.find_first_not_of(ws);
        if (first == std::string::npos)
            return {};
        const size_t last = result.find_last_not_of(ws);
        return result.substr(first, last - first + 1);
    }

    // Connects to the stream just long enough to read its response headers
    // and pulls the station name out of "icy-name" / "ice-name". Safe to call
    // from a worker thread. `out.error` is set when the server is unreachable;
    // throws exception_aborted when the user aborts.
    void probe_station(const std::string& url, abort_callback& abort,
        station_probe_result& out)
    {
        const auto fail = [&out](const char* what)
            {
                out.error = (what != nullptr && *what != '\0')
                    ? what
                    : "no response from the server";
                out.finished = true;
            };

        try
        {
            static_api_ptr_t<http_client> client;
            auto request = client->create_request("GET");
            request->add_header("Icy-MetaData: 1");

            // run_ex() hands back the stream as a file object; the HTTP
            // response headers are exposed through its http_reply service.
            auto stream = request->run_ex(url.c_str(), abort);

            http_reply::ptr reply;
            if (stream->service_query_t(reply))
            {
                static const char* const headers[] =
                { "icy-name", "ice-name", "Icy-Name", "Ice-Name" };

                for (const char* header : headers)
                {
                    pfc::string8 value;
                    reply->get_http_header(header, value);

                    const std::string text = trim_ascii_text(value.get_ptr());
                    if (!text.empty())
                    {
                        out.name = text;
                        break;
                    }
                }
            }

            out.finished = true;
        }
        catch (const exception_aborted&)
        {
            throw; // user pressed Abort
        }
        catch (const std::exception& e) // also covers pfc::exception
        {
            fail(e.what());
        }
        catch (...)
        {
            fail("");
        }
    }

    // Creates the .utag in the background so foobar2000 stays usable while the
    // stream is asked for its name. The playlist and the results popup are
    // handled in on_done(), which runs on the main thread.
    class add_station_task : public threaded_process_callback
    {
    public:
        add_station_task(std::wstring url, std::wstring name, std::wstring folder,
            bool radioBrowserLookup)
            : m_wideUrl(std::move(url)), m_wideName(std::move(name)),
            m_folder(std::move(folder)), m_rbLookup(radioBrowserLookup) {
        }

        void run(threaded_process_status& status, abort_callback& abort) override
        {
            m_logEntries.emplace_back("Add internet radio url and create utag - Results");
            m_logEntries.emplace_back("Output folder: " + wide_to_utf8(m_folder));
            m_logEntries.emplace_back(std::string(64, '-'));

            try
            {
                do_create(status, abort);
            }
            catch (const exception_aborted&)
            {
                throw; // reported through on_done(aborted = true)
            }
            catch (const std::exception& e)
            {
                ++m_failed;
                std::string entry = "[ERROR] " + (m_label.empty() ? m_url : m_label);
                entry += "\n        Source: " + m_url;
                if (!m_outputPath.empty()) entry += "\n        Output: " + m_outputPath;
                entry += "\n        Reason: ";
                entry += e.what();
                m_logEntries.emplace_back(std::move(entry));
            }
            catch (...)
            {
                ++m_failed;
                std::string entry = "[ERROR] " + (m_label.empty() ? m_url : m_label);
                entry += "\n        Source: " + m_url + "\n        Reason: Unknown error.";
                if (!m_outputPath.empty()) entry += "\n        Output: " + m_outputPath;
                m_logEntries.emplace_back(std::move(entry));
            }
        }

        void on_done(HWND, bool aborted) override
        {
            static const char* const kTitle = "Add internet radio url and create utag";
            const HWND owner = core_api::get_main_window();

            if (aborted)
            {
                report_result(kTitle, "Station lookup was cancelled; no file created.",
                    "Station lookup was cancelled; no file created.");
                return;
            }

            // The new file goes into the "New utags" playlist, and that playlist
            // becomes the active one. The outcome is added to the log so a
            // failure is visible instead of silent.
            if (m_created > 0 && !m_outputPath.empty())
            {
                const std::string playlistName = "New utags";
                std::string playlistNote;

                try
                {
                    static_api_ptr_t<playlist_manager> playlists;

                    // Canonical (file://...) form of the created file.
                    pfc::string8 canonicalPath;
                    filesystem::g_get_canonical_path(m_outputPath.c_str(), canonicalPath);

                    pfc::list_t<const char*> locations;
                    locations.add_item(canonicalPath.get_ptr());

                    // Reuse the playlist if it exists, otherwise create it.
                    t_size index = playlists->find_or_create_playlist(
                        playlistName.c_str(), playlistName.length());
                    if (index == pfc_infinite)
                        index = playlists->create_playlist(
                            playlistName.c_str(), playlistName.length(), pfc_infinite);

                    if (index == pfc_infinite)
                        throw std::runtime_error("the playlist could not be created");

                    // Activate first so the added item is selected in the visible playlist.
                    playlists->set_active_playlist(index);
                    playlists->playlist_add_locations(index, locations, true, owner);

                    playlistNote = "Added to the \"" + playlistName +
                        "\" playlist and made it active.";
                }
                catch (const std::exception& e)
                {
                    playlistNote = "Could not add to the \"" + playlistName +
                        "\" playlist: " + e.what();
                }
                catch (...)
                {
                    playlistNote = "Could not add to the \"" + playlistName +
                        "\" playlist: unknown error.";
                }

                // Not fatal for the utag itself, but never silent.
                m_logEntries.emplace_back(playlistNote);
                UTAG_LOG("[foo_utag] %s", playlistNote.c_str());
            }

            m_logEntries.emplace_back(std::string(64, '-'));
            std::ostringstream summary;
            summary << "Summary: Created " << m_created << " | Skipped " << m_skipped
                << " | Failed " << m_failed;
            m_logEntries.emplace_back(summary.str());

            std::ostringstream results;
            for (size_t i = 0; i < m_logEntries.size(); ++i)
            {
                if (i != 0) results << "\r\n\r\n";
                results << m_logEntries[i];
            }

            const std::string resultsText = results.str();
            report_result(kTitle, resultsText, summary.str());
        }

    private:
        // Worker thread: no UI, playlist or config access in here.
        void do_create(threaded_process_status& status, abort_callback& abort)
        {
            m_url = wide_to_utf8(m_wideUrl);
            std::string name = wide_to_utf8(m_wideName);
            bool probed = false;

            // Optional: ask Radio Browser about this URL first. If it knows the
            // station, its details become the .utag; otherwise (or if the
            // lookup fails) carry on with the plain stream lookup below.
            json station = json::object();
            bool fromRadioBrowser = false;

            if (m_rbLookup && !is_utag_path(m_url))
            {
                status.set_item("Searching Radio Browser...");

                json found;
                std::string lookupError;
                if (radio_lookup_station_by_url(m_url, abort, found, lookupError))
                {
                    station = std::move(found);
                    fromRadioBrowser = true;
                    if (name.empty() && station.contains("name") && station["name"].is_string())
                        name = station["name"].get<std::string>();
                }
                else if (!lookupError.empty())
                {
                    m_logEntries.emplace_back(
                        "[NOTE] Radio Browser lookup failed (" + lookupError +
                        "); using the stream's own details instead.");
                }
                else
                {
                    m_logEntries.emplace_back(
                        "[NOTE] Radio Browser has no station with this URL; "
                        "using the stream's own details instead.");
                }
            }

            // No name typed in: ask the stream for it. An unreachable server
            // (or an abort) stops here and nothing is created.
            if (name.empty() && !is_utag_path(m_url))
            {
                status.set_item("Looking up station name...");

                station_probe_result probe;
                probe_station(m_url, abort, probe);

                if (!probe.error.empty())
                    throw std::runtime_error("Could not reach the stream: " + probe.error);

                name = probe.name;
                probed = !name.empty();
            }

            m_label = name.empty() ? station_label_from_url(m_url) : name;

            if (is_utag_path(m_url))
            {
                ++m_skipped;
                m_logEntries.emplace_back(
                    "[SKIPPED] That is a .utag file, not a stream URL: " + m_url);
                return;
            }

            station["name"] = m_label;
            station["@"] = m_url;
            if (fromRadioBrowser)
                station["title"] = m_label;
            add_country_flag(station);
            const std::string contents = json::array({ station }).dump(4, ' ', false, json::error_handler_t::replace) + "\n";

            if (write_unique_utag(m_folder, make_safe_filename_base(m_label),
                contents, m_outputPath))
            {
                ++m_created;
                m_logEntries.emplace_back("[CREATED] " + m_label +
                    (fromRadioBrowser ? " (details from Radio Browser)" : "") +
                    (probed ? " (name from stream)" : "") +
                    "\n          @  : " + m_url +
                    "\n          -> " + m_outputPath);
            }
            else
            {
                ++m_skipped;
                m_logEntries.emplace_back("[SKIPPED] " + m_label +
                    "\n          @  : " + m_url +
                    "\n          -> Tried 9999 filename variants; no available filename.");
            }
        }

        std::wstring m_wideUrl;
        std::wstring m_wideName;
        std::wstring m_folder;
        bool m_rbLookup = false;

        std::string m_url;
        std::string m_label;
        std::string m_outputPath;
        std::vector<std::string> m_logEntries;
        unsigned m_created = 0;
        unsigned m_skipped = 0;
        unsigned m_failed = 0;
    };

    void start_station_creation(const std::wstring& url, const std::wstring& name,
        const std::wstring& folder, bool radioBrowserLookup)
    {
        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<add_station_task>(url, name, folder, radioBrowserLookup);

        // Modeless: foobar2000 stays usable; the progress window only shows up
        // when the lookup takes a moment.
        threaded_process::g_run_modeless(
            task,
            threaded_process::flag_show_abort | threaded_process::flag_show_delayed,
            core_api::get_main_window(),
            radioBrowserLookup ? "Searching Radio Browser..." : "Looking up station name...");
    }
}

void utag_open_station_dialog(HWND owner)
{
    open_station_dialog(owner);
}
