#include "utag_ui.h"

// -----------------------------------------------------------------------------
// Main menu command: File > Import stations from Radio Browser...
// Asks for a few filters, downloads matching stations from the Radio Browser
// directory (radio-browser.info) and writes one .utag file per station into the
// default utag folder.
// -----------------------------------------------------------------------------

namespace
{
    constexpr int kIdRbName = 1101;
    constexpr int kIdRbTag = 1102;
    constexpr int kIdRbCountry = 1103;
    constexpr int kIdRbBitrate = 1104;
    constexpr int kIdRbLimit = 1105;
    constexpr int kIdRbStatus = 1106;
    constexpr int kIdRbCodec = 1107;
    constexpr WORD kIdRbShowTags = 2106;
    constexpr WORD kIdRbTagFilter = 2107;
    constexpr WORD kIdRbTagList = 2108;
    constexpr WORD kIdRbShowCountries = 2109;
    constexpr WORD kIdRbShowCodecs = 2110;


    constexpr size_t kRbMaxDetailedEntries = 60; // results popup stays readable

    struct radio_query
    {
        std::string name;
        std::string tag;
        std::string countryCode;
        std::string codec;
        long minBitrate = 0;
        long limit = 100;
    };

    // The search window is modeless (foobar2000 stays usable while it is
    // open), so its state lives on the heap and is freed in WM_NCDESTROY.
    struct radio_prompt_state
    {
        fb2k::CCoreDarkModeHooks darkHooks;
    };

    HWND g_radioPromptWnd = nullptr; // the one open search window, if any

    bool is_ascii_letter(wchar_t c)
    {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
    }

    // Parses a non-negative whole number; returns false when it is not one.
    bool parse_whole_number(const std::wstring& text, long& value)
    {
        if (text.empty())
            return false;

        wchar_t* end = nullptr;
        const long parsed = std::wcstol(text.c_str(), &end, 10);
        if (end == nullptr || *end != L'\0' || parsed < 0)
            return false;

        value = parsed;
        return true;
    }

    bool choose_radio_browser_tag(HWND owner, std::string& selected);
    bool choose_radio_browser_country(HWND owner, std::string& selected);
    bool choose_radio_browser_codec(HWND owner, std::string& selected);
    void start_radio_import(const radio_query& query, const std::wstring& parentFolder);


    INT_PTR CALLBACK radio_prompt_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<radio_prompt_state*>(
            GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<radio_prompt_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);

            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg); // Tab / Enter / Esc keys

            SetFocus(GetDlgItem(dlg, kIdRbName));
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

            // NEW: Show all Radio Browser tags.
            if (id == kIdRbShowTags && HIWORD(wp) == BN_CLICKED)
            {
                if (state != nullptr)
                {
                    std::string selected;

                    if (choose_radio_browser_tag(dlg, selected))
                    {
                        const std::wstring wideTag = utf8_to_wide(selected);

                        SetDlgItemTextW(
                            dlg, kIdRbTag, wideTag.c_str());
                    }
                }

                return TRUE;
            }

            // Show all Radio Browser codecs.
            if (id == kIdRbShowCodecs && HIWORD(wp) == BN_CLICKED)
            {
                if (state != nullptr)
                {
                    std::string selected;

                    if (choose_radio_browser_codec(dlg, selected))
                    {
                        const std::wstring wideCodec = utf8_to_wide(selected);

                        SetDlgItemTextW(
                            dlg, kIdRbCodec, wideCodec.c_str());
                    }
                }

                return TRUE;
            }

            // Show all Radio Browser country codes.
            if (id == kIdRbShowCountries && HIWORD(wp) == BN_CLICKED)
            {
                if (state != nullptr)
                {
                    std::string selected;

                    if (choose_radio_browser_country(dlg, selected))
                    {
                        const std::wstring wideCode = utf8_to_wide(selected);

                        SetDlgItemTextW(
                            dlg, kIdRbCountry, wideCode.c_str());
                    }
                }

                return TRUE;
            }

            if (id == IDOK && state != nullptr)
            {
                const std::wstring country =
                    trim_wide(read_dialog_text(dlg, kIdRbCountry));

                if (!country.empty() &&
                    (country.size() != 2 ||
                        !is_ascii_letter(country[0]) ||
                        !is_ascii_letter(country[1])))
                {
                    SetDlgItemTextW(dlg, kIdRbStatus,
                        L"Country code must be two letters, e.g. DE.");
                    SetFocus(GetDlgItem(dlg, kIdRbCountry));
                    return TRUE;
                }

                long minBitrate = 0;
                const std::wstring bitrateText =
                    trim_wide(read_dialog_text(dlg, kIdRbBitrate));

                if (!bitrateText.empty() &&
                    !parse_whole_number(bitrateText, minBitrate))
                {
                    SetDlgItemTextW(dlg, kIdRbStatus,
                        L"Minimum bitrate must be a whole number.");
                    SetFocus(GetDlgItem(dlg, kIdRbBitrate));
                    return TRUE;
                }

                long limit = 100;
                const std::wstring limitText =
                    trim_wide(read_dialog_text(dlg, kIdRbLimit));

                if (!limitText.empty() &&
                    (!parse_whole_number(limitText, limit) ||
                        limit < 1 || limit > 5000))
                {
                    SetDlgItemTextW(dlg, kIdRbStatus,
                        L"Maximum stations must be between 1 and 5000.");
                    SetFocus(GetDlgItem(dlg, kIdRbLimit));
                    return TRUE;
                }

                radio_query query;

                query.name =
                    wide_to_utf8(trim_wide(
                        read_dialog_text(dlg, kIdRbName)));

                query.tag =
                    wide_to_utf8(trim_wide(
                        read_dialog_text(dlg, kIdRbTag)));

                query.countryCode = wide_to_utf8(country);
                query.codec =
                    wide_to_utf8(trim_wide(
                        read_dialog_text(dlg, kIdRbCodec)));
                query.minBitrate = minBitrate;
                query.limit = limit;

                // May ask for the default folder the first time; keep this
                // window open if that is cancelled.
                std::wstring parentFolder;
                EnableWindow(dlg, FALSE);
                const bool haveFolder = get_default_folder(parentFolder, false);
                EnableWindow(dlg, TRUE);

                if (!haveFolder)
                    return TRUE;

                DestroyWindow(dlg);
                start_radio_import(query, parentFolder);
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
            if (g_radioPromptWnd == dlg)
                g_radioPromptWnd = nullptr;
            delete state;
            break;

        default:
            break;
        }

        return FALSE;
    }


    // Opens the (modeless) search window, or brings the open one to the front.
    void open_radio_query_dialog(HWND owner)
    {
        if (g_radioPromptWnd != nullptr && IsWindow(g_radioPromptWnd))
        {
            ShowWindow(g_radioPromptWnd, SW_RESTORE);
            SetForegroundWindow(g_radioPromptWnd);
            return;
        }

        constexpr WORD kButton = 0x0080;
        constexpr WORD kEdit = 0x0081;
        constexpr WORD kStatic = 0x0082;

        constexpr DWORD kEditStyle =
            ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP;

        dialog_template t;

        t.begin(
            WS_POPUP | WS_CAPTION | WS_SYSMENU |
            DS_MODALFRAME | DS_CENTER,
            0, 0, 300, 160,
            L"Import stations from Radio Browser",
            L"Segoe UI", 8);

        t.item(kStatic, SS_LEFT,
            8, 12, 78, 10, 0xFFFF,
            L"Name contains:");

        t.item(kEdit, kEditStyle,
            90, 10, 202, 13, kIdRbName, L"");

        t.item(kStatic, SS_LEFT,
            8, 30, 78, 10, 0xFFFF,
            L"Tag / genre:");

        t.item(kEdit, kEditStyle,
            90, 28, 126, 13, kIdRbTag, L"");

        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP,
            220, 28, 72, 14, kIdRbShowTags,
            L"Show all tags");

        t.item(kStatic, SS_LEFT,
            8, 48, 78, 10, 0xFFFF,
            L"Country code:");

        t.item(kEdit, kEditStyle,
            90, 46, 126, 13, kIdRbCountry, L"");

        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP,
            220, 46, 72, 14, kIdRbShowCountries,
            L"Show all countries");

        t.item(kStatic, SS_LEFT,
            8, 66, 78, 10, 0xFFFF,
            L"Codec:");

        t.item(kEdit, kEditStyle,
            90, 64, 126, 13, kIdRbCodec, L"");

        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP,
            220, 64, 72, 14, kIdRbShowCodecs,
            L"Show all codecs");

        t.item(kStatic, SS_LEFT,
            8, 84, 78, 10, 0xFFFF,
            L"Min. bitrate:");

        t.item(kEdit, kEditStyle | ES_NUMBER,
            90, 82, 202, 13, kIdRbBitrate, L"");

        t.item(kStatic, SS_LEFT,
            8, 102, 78, 10, 0xFFFF,
            L"Max stations:");

        t.item(kEdit, kEditStyle | ES_NUMBER,
            90, 100, 202, 13, kIdRbLimit, L"100");

        t.item(kStatic, SS_LEFT,
            8, 121, 284, 10, kIdRbStatus, L"");

        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP,
            180, 138, 52, 14, IDOK, L"Import");

        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP,
            240, 138, 52, 14, IDCANCEL, L"Cancel");

        auto* state = new radio_prompt_state;

        const HWND dlg = CreateDialogIndirectParamW(
            core_api::get_my_instance(),
            t.data(),
            owner,
            radio_prompt_proc,
            reinterpret_cast<LPARAM>(state));

        if (dlg == nullptr)
        {
            delete state;
            return;
        }

        g_radioPromptWnd = dlg;
        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
    }


    // Most-clicked working stations first; empty filters are left out.
    std::string build_radio_browser_path(const radio_query& q)
    {
        std::string path =
            "/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=" +
            std::to_string(q.limit);

        if (!q.name.empty())
            path += "&name=" + percent_encode(q.name);
        if (!q.tag.empty())
            path += "&tag=" + percent_encode(q.tag);
        if (!q.countryCode.empty())
        {
            std::string code = q.countryCode;
            std::transform(code.begin(), code.end(), code.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            path += "&countrycode=" + percent_encode(code);
        }
        if (!q.codec.empty())
            path += "&codec=" + percent_encode(q.codec);
        if (q.minBitrate > 0)
            path += "&bitrateMin=" + std::to_string(q.minBitrate);

        return path;
    }

    struct radio_fetch_result
    {
        std::string body;
        std::string error;
        bool finished = false; // false when the user aborted
    };

    // Downloads `path`, trying the Radio Browser mirrors in turn. Returns false
    // with `error` set when none answers; throws exception_aborted when the
    // user aborts. Safe to call from a worker thread.
    bool radio_download(const std::string& path, abort_callback& abort,
        std::string& body, std::string& error)
    {
        static const char* const servers[] =
        {
            "https://de1.api.radio-browser.info",
            "https://de2.api.radio-browser.info",
            "https://nl1.api.radio-browser.info",
            "https://at1.api.radio-browser.info",
        };

        std::string lastError;

        for (const char* server : servers)
        {
            try
            {
                static_api_ptr_t<http_client> client;
                auto request = client->create_request("GET");
                request->add_header("Accept: application/json");

                const std::string url = std::string(server) + path;
                auto stream = request->run_ex(url.c_str(), abort);

                constexpr size_t kMaxResponse = static_cast<size_t>(64) * 1024 * 1024;

                std::string data;
                std::vector<char> buffer(16384); // heap, not stack
                for (;;)
                {
                    abort.check();
                    const t_size got = stream->read(buffer.data(), buffer.size(), abort);
                    if (got == 0)
                        break;

                    data.append(buffer.data(), static_cast<size_t>(got));
                    if (data.size() > kMaxResponse)
                        throw std::runtime_error("The response is too large.");
                }

                if (data.empty())
                {
                    lastError = "empty response";
                    continue;
                }

                body = std::move(data);
                return true;
            }
            catch (const exception_aborted&)
            {
                throw; // user pressed Abort
            }
            catch (const std::exception& e) // also covers pfc::exception
            {
                lastError = e.what();
            }
            catch (...)
            {
                lastError = "no response from the server";
            }
        }

        error = lastError.empty() ? "no response from the server" : lastError;
        return false;
    }

    // Short modal download used by the tag and country pickers.
    class radio_fetch_task : public threaded_process_callback
    {
    public:
        radio_fetch_task(const std::string& path, std::shared_ptr<radio_fetch_result> out)
            : m_path(path), m_out(std::move(out)) {
        }

        void run(threaded_process_status&, abort_callback& abort) override
        {
            std::string body;
            std::string error;

            if (radio_download(m_path, abort, body, error))
                m_out->body = std::move(body);
            else
                m_out->error = std::move(error);

            m_out->finished = true;
        }

    private:
        std::string m_path;
        std::shared_ptr<radio_fetch_result> m_out;
    };

    std::string trim_text(const std::string& text)
    {
        const char* ws = " \t\r\n";
        const size_t first = text.find_first_not_of(ws);
        if (first == std::string::npos)
            return {};
        const size_t last = text.find_last_not_of(ws);
        return text.substr(first, last - first + 1);
    }

    std::string json_text(const json& object, const char* key)
    {
        const auto it = object.find(key);
        if (it == object.end() || !it->is_string())
            return {};
        return trim_text(it->get<std::string>());
    }

    // Reads the "stationcount" member (number or numeric string); 0 if absent.
    static unsigned long long radio_json_count(const json& item)
    {
        const auto count = item.find("stationcount");
        if (count == item.end())
            return 0;

        try
        {
            if (count->is_number_unsigned())
                return count->get<unsigned long long>();

            if (count->is_number_integer())
            {
                const auto n = count->get<long long>();
                return n > 0 ? static_cast<unsigned long long>(n) : 0;
            }

            if (count->is_string())
                return std::stoull(count->get<std::string>());
        }
        catch (...)
        {
        }

        return 0;
    }

    // Used by both pickers. `name` is the value returned to the dialog (a tag
    // or a country code); `title` is optional extra text (the country name)
    // that is shown and searched too.
    struct radio_tag_item
    {
        std::string name;
        std::string title;
        unsigned long long stationCount = 0;
    };

    struct radio_tag_picker_state
    {
        std::vector<radio_tag_item> tags;
        std::vector<size_t> visible;
        std::string selected;
        fb2k::CCoreDarkModeHooks darkHooks;
    };

    static std::string radio_tag_lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });

        return text;
    }

    static void rebuild_radio_tag_list(
        HWND dlg, radio_tag_picker_state* state)
    {
        if (!state)
            return;

        const std::string filter = radio_tag_lower(trim_text(
            wide_to_utf8(read_dialog_text(dlg, kIdRbTagFilter))));

        SendDlgItemMessageW(dlg, kIdRbTagList, LB_RESETCONTENT, 0, 0);
        state->visible.clear();

        for (size_t i = 0; i < state->tags.size(); ++i)
        {
            const auto& tag = state->tags[i];

            if (!filter.empty() &&
                radio_tag_lower(tag.name).find(filter) == std::string::npos &&
                radio_tag_lower(tag.title).find(filter) == std::string::npos)
                continue;

            const std::string label =
                (tag.title.empty()
                    ? tag.name
                    : tag.title + " [" + tag.name + "]") +
                " (" + std::to_string(tag.stationCount) + " stations)";

            const std::wstring wideLabel = utf8_to_wide(label);

            SendDlgItemMessageW(
                dlg, kIdRbTagList, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(wideLabel.c_str()));

            state->visible.push_back(i);
        }

        if (!state->visible.empty())
            SendDlgItemMessageW(dlg, kIdRbTagList, LB_SETCURSEL, 0, 0);
    }

    static INT_PTR CALLBACK radio_tag_picker_proc(
        HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<radio_tag_picker_state*>(
            GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<radio_tag_picker_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);

            if (state != nullptr)
            {
                state->darkHooks.AddDialogWithControls(dlg);
                rebuild_radio_tag_list(dlg, state);
            }

            return TRUE;
        }

        case WM_COMMAND:
            if (!state)
                return FALSE;

            if (LOWORD(wp) == kIdRbTagFilter &&
                HIWORD(wp) == EN_CHANGE)
            {
                rebuild_radio_tag_list(dlg, state);
                return TRUE;
            }

            if (LOWORD(wp) == kIdRbTagList &&
                HIWORD(wp) == LBN_DBLCLK)
            {
                SendMessageW(dlg, WM_COMMAND, IDOK, 0);
                return TRUE;
            }

            if (LOWORD(wp) == IDOK)
            {
                const LRESULT selection = SendDlgItemMessageW(
                    dlg, kIdRbTagList, LB_GETCURSEL, 0, 0);

                if (selection == LB_ERR ||
                    static_cast<size_t>(selection) >= state->visible.size())
                    return TRUE;

                state->selected =
                    state->tags[state->visible[
                        static_cast<size_t>(selection)]].name;

                EndDialog(dlg, IDOK);
                return TRUE;
            }

            if (LOWORD(wp) == IDCANCEL)
            {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;

        case WM_NCDESTROY:
            SetWindowLongPtrW(dlg, DWLP_USER, 0);
            break;
        }

        return FALSE;
    }

    static bool show_radio_tag_picker(
        HWND owner,
        const std::vector<radio_tag_item>& tags,
        std::string& selected,
        const wchar_t* dialogTitle,
        const wchar_t* filterPrompt)
    {
        radio_tag_picker_state state;
        state.tags = tags;

        constexpr WORD kButton = 0x0080;
        constexpr WORD kEdit = 0x0081;
        constexpr WORD kStatic = 0x0082;
        constexpr WORD kListBox = 0x0083;

        dialog_template t;

        t.begin(
            WS_POPUP | WS_CAPTION | WS_SYSMENU |
            DS_MODALFRAME | DS_CENTER,
            0, 0, 300, 224,
            dialogTitle, L"Segoe UI", 8);

        t.item(kStatic, SS_LEFT,
            8, 10, 284, 12, 0xFFFF,
            filterPrompt);

        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
            8, 24, 284, 16, kIdRbTagFilter, L"");

        t.item(kListBox,
            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            WS_VSCROLL | WS_BORDER | WS_TABSTOP,
            8, 44, 284, 148, kIdRbTagList, L"");

        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP,
            180, 200, 52, 16, IDOK, L"Select");

        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP,
            240, 200, 52, 16, IDCANCEL, L"Cancel");

        const INT_PTR result = DialogBoxIndirectParamW(
            core_api::get_my_instance(),
            t.data(),
            owner,
            radio_tag_picker_proc,
            reinterpret_cast<LPARAM>(&state));

        if (result != IDOK || state.selected.empty())
            return false;

        selected = std::move(state.selected);
        return true;
    }

    bool choose_radio_browser_tag(HWND owner, std::string& selected)
    {
        static const char* const kTitle = "Radio Browser tags";

        const auto fetched = std::make_shared<radio_fetch_result>();

        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<radio_fetch_task>(
                "/json/tags?order=stationcount&reverse=true",
                fetched);

        threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort,
            owner,
            "Fetching Radio Browser tags...");

        if (!fetched->error.empty())
        {
            const std::string message =
                "Could not fetch Radio Browser tags:\r\n" + fetched->error;

            popup_message::g_show(
                message.c_str(), kTitle, popup_message::icon_error);
            return false;
        }

        if (!fetched->finished)
        {
            popup_message::g_show(
                "Tag loading cancelled.",
                kTitle, popup_message::icon_information);
            return false;
        }

        const json response = json::parse(fetched->body, nullptr, false);

        if (response.is_discarded() || !response.is_array())
        {
            popup_message::g_show(
                "Radio Browser returned an unexpected tag response.",
                kTitle, popup_message::icon_error);
            return false;
        }

        std::vector<radio_tag_item> tags;
        tags.reserve(response.size());

        for (const auto& item : response)
        {
            if (!item.is_object())
                continue;

            radio_tag_item tag;
            tag.name = json_text(item, "name");

            if (tag.name.empty())
                continue;

            tag.stationCount = radio_json_count(item);

            tags.push_back(std::move(tag));
        }

        std::sort(tags.begin(), tags.end(),
            [](const radio_tag_item& a, const radio_tag_item& b)
            {
                if (a.stationCount != b.stationCount)
                    return a.stationCount > b.stationCount;

                return radio_tag_lower(a.name) < radio_tag_lower(b.name);
            });

        if (tags.empty())
        {
            popup_message::g_show(
                "Radio Browser returned no tags.",
                kTitle, popup_message::icon_information);
            return false;
        }

        return show_radio_tag_picker(
            owner, tags, selected,
            L"Select Radio Browser tag", L"Filter tags by name:");
    }

    // Lists every country Radio Browser knows (name + two-letter code) and
    // returns the chosen code, e.g. "DE".
    bool choose_radio_browser_country(HWND owner, std::string& selected)
    {
        static const char* const kTitle = "Radio Browser countries";

        const auto fetched = std::make_shared<radio_fetch_result>();

        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<radio_fetch_task>(
                "/json/countries?hidebroken=true&order=name",
                fetched);

        threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort,
            owner,
            "Fetching Radio Browser countries...");

        if (!fetched->error.empty())
        {
            const std::string message =
                "Could not fetch Radio Browser countries:\r\n" +
                fetched->error;

            popup_message::g_show(
                message.c_str(), kTitle, popup_message::icon_error);
            return false;
        }

        if (!fetched->finished)
        {
            popup_message::g_show(
                "Country loading cancelled.",
                kTitle, popup_message::icon_information);
            return false;
        }

        const json response = json::parse(fetched->body, nullptr, false);

        if (response.is_discarded() || !response.is_array())
        {
            popup_message::g_show(
                "Radio Browser returned an unexpected country response.",
                kTitle, popup_message::icon_error);
            return false;
        }

        std::vector<radio_tag_item> countries;
        countries.reserve(response.size());

        for (const auto& item : response)
        {
            if (!item.is_object())
                continue;

            radio_tag_item country;
            country.name = json_text(item, "iso_3166_1");

            // Only entries with a usable two-letter code can be searched for.
            if (country.name.size() != 2 ||
                !is_ascii_letter(static_cast<wchar_t>(country.name[0])) ||
                !is_ascii_letter(static_cast<wchar_t>(country.name[1])))
                continue;

            std::transform(country.name.begin(), country.name.end(),
                country.name.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::toupper(c));
                });

            country.title = json_text(item, "name");
            country.stationCount = radio_json_count(item);
            countries.push_back(std::move(country));
        }

        std::sort(countries.begin(), countries.end(),
            [](const radio_tag_item& a, const radio_tag_item& b)
            {
                const std::string an = radio_tag_lower(
                    a.title.empty() ? a.name : a.title);
                const std::string bn = radio_tag_lower(
                    b.title.empty() ? b.name : b.title);

                if (an != bn)
                    return an < bn;

                return a.name < b.name;
            });

        if (countries.empty())
        {
            popup_message::g_show(
                "Radio Browser returned no countries.",
                kTitle, popup_message::icon_information);
            return false;
        }

        return show_radio_tag_picker(
            owner, countries, selected,
            L"Select Radio Browser country",
            L"Filter by country name or code:");
    }

    // Lists the codecs Radio Browser knows (most used first) and returns the
    // chosen one, e.g. "MP3" or "AAC+".
    bool choose_radio_browser_codec(HWND owner, std::string& selected)
    {
        static const char* const kTitle = "Radio Browser codecs";

        const auto fetched = std::make_shared<radio_fetch_result>();

        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<radio_fetch_task>(
                "/json/codecs?hidebroken=true&order=stationcount&reverse=true",
                fetched);

        threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort,
            owner,
            "Fetching Radio Browser codecs...");

        if (!fetched->error.empty())
        {
            const std::string message =
                "Could not fetch Radio Browser codecs:\r\n" +
                fetched->error;

            popup_message::g_show(
                message.c_str(), kTitle, popup_message::icon_error);
            return false;
        }

        if (!fetched->finished)
        {
            popup_message::g_show(
                "Codec loading cancelled.",
                kTitle, popup_message::icon_information);
            return false;
        }

        const json response = json::parse(fetched->body, nullptr, false);

        if (response.is_discarded() || !response.is_array())
        {
            popup_message::g_show(
                "Radio Browser returned an unexpected codec response.",
                kTitle, popup_message::icon_error);
            return false;
        }

        std::vector<radio_tag_item> codecs;
        codecs.reserve(response.size());

        for (const auto& item : response)
        {
            if (!item.is_object())
                continue;

            radio_tag_item codec;
            codec.name = json_text(item, "name");

            if (codec.name.empty())
                continue;

            codec.stationCount = radio_json_count(item);
            codecs.push_back(std::move(codec));
        }

        std::sort(codecs.begin(), codecs.end(),
            [](const radio_tag_item& a, const radio_tag_item& b)
            {
                if (a.stationCount != b.stationCount)
                    return a.stationCount > b.stationCount;

                return radio_tag_lower(a.name) < radio_tag_lower(b.name);
            });

        if (codecs.empty())
        {
            popup_message::g_show(
                "Radio Browser returned no codecs.",
                kTitle, popup_message::icon_information);
            return false;
        }

        return show_radio_tag_picker(
            owner, codecs, selected,
            L"Select Radio Browser codec",
            L"Filter codecs by name:");
    }

    // "Radio Browser - jazz - DE - 128+ kbps - top 100": describes a search.
    // Lists the languages Radio Browser knows (most used first) and returns
    // the chosen one, e.g. "english".
    bool choose_radio_browser_language(HWND owner, std::string& selected)
    {
        static const char* const kTitle = "Radio Browser languages";

        const auto fetched = std::make_shared<radio_fetch_result>();

        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<radio_fetch_task>(
                "/json/languages?hidebroken=true&order=stationcount&reverse=true",
                fetched);

        threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort,
            owner,
            "Fetching Radio Browser languages...");

        if (!fetched->error.empty())
        {
            const std::string message =
                "Could not fetch Radio Browser languages:\r\n" +
                fetched->error;

            popup_message::g_show(
                message.c_str(), kTitle, popup_message::icon_error);
            return false;
        }

        if (!fetched->finished)
        {
            popup_message::g_show(
                "Language loading cancelled.",
                kTitle, popup_message::icon_information);
            return false;
        }

        const json response = json::parse(fetched->body, nullptr, false);

        if (response.is_discarded() || !response.is_array())
        {
            popup_message::g_show(
                "Radio Browser returned an unexpected language response.",
                kTitle, popup_message::icon_error);
            return false;
        }

        std::vector<radio_tag_item> languages;
        languages.reserve(response.size());

        for (const auto& item : response)
        {
            if (!item.is_object())
                continue;

            radio_tag_item language;
            language.name = json_text(item, "name");

            if (language.name.empty())
                continue;

            language.stationCount = radio_json_count(item);
            languages.push_back(std::move(language));
        }

        std::sort(languages.begin(), languages.end(),
            [](const radio_tag_item& a, const radio_tag_item& b)
            {
                if (a.stationCount != b.stationCount)
                    return a.stationCount > b.stationCount;

                return radio_tag_lower(a.name) < radio_tag_lower(b.name);
            });

        if (languages.empty())
        {
            popup_message::g_show(
                "Radio Browser returned no languages.",
                kTitle, popup_message::icon_information);
            return false;
        }

        return show_radio_tag_picker(
            owner, languages, selected,
            L"Select Radio Browser language",
            L"Filter languages by name:");
    }

    // "Radio Browser - jazz - DE - 128+ kbps - top 100": describes a search.

    std::string describe_radio_query(const radio_query& q)
    {
        std::string text = "Radio Browser";

        if (!q.tag.empty())
            text += " - " + q.tag;
        if (!q.name.empty())
            text += " - name " + q.name;
        if (!q.countryCode.empty())
        {
            std::string code = q.countryCode;
            std::transform(code.begin(), code.end(), code.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            text += " - " + code;
        }
        if (!q.codec.empty())
            text += " - " + q.codec;
        if (q.minBitrate > 0)
            text += " - " + std::to_string(q.minBitrate) + "+ kbps";

        text += " - top " + std::to_string(q.limit);
        return text;
    }


    // Radio Browser station -> .utag station object.
    json radio_station_to_utag(const json& station, const std::string& label,
        const std::string& url)
    {
        json entry = json::object();
        entry["@"] = url;
        entry["name"] = label;
        entry["title"] = label;

        // Comma separated tags become (multi-value) genre.
        std::vector<std::string> genres;
        std::stringstream tags(json_text(station, "tags"));
        for (std::string tag; std::getline(tags, tag, ',');)
        {
            tag = trim_text(tag);
            if (!tag.empty())
                genres.push_back(tag);
        }
        if (genres.size() == 1)
        {
            entry["genre"] = genres.front();
        }
        else if (genres.size() > 1)
        {
            json array = json::array();
            for (const auto& genre : genres)
                array.push_back(genre);
            entry["genre"] = std::move(array);
        }

        for (const char* key : { "country", "countrycode", "language", "codec",
            "homepage", "favicon", "stationuuid" })
        {
            const std::string value = json_text(station, key);
            if (!value.empty())
                entry[key] = value;
        }

        const auto bitrate = station.find("bitrate");
        if (bitrate != station.end() && bitrate->is_number() && bitrate->get<double>() > 0)
            entry["bitrate"] = std::to_string(static_cast<long long>(bitrate->get<double>()));

        return entry;
    }

    // Runs the whole import in the background so foobar2000 stays usable:
    // download, create the folder, write the .utag files. The playlist and the
    // results popup are done in on_done(), which runs on the main thread.
    class radio_import_task : public threaded_process_callback
    {
    public:
        radio_import_task(radio_query query, std::wstring parentFolder)
            : m_query(std::move(query)), m_parent(std::move(parentFolder)) {
        }

        // Import of stations already chosen (the browse window).
        // `source` names the service the stations came from (used in the
        // window titles and the results log), e.g. "Icecast Directory".
        radio_import_task(std::string source, std::string description,
            std::vector<json> stations, std::wstring parentFolder, bool browseWindow)
            : m_preset(true), m_source(std::move(source)),
            m_description(std::move(description)),
            m_presetStations(std::move(stations)), m_parent(std::move(parentFolder)),
            m_browseWindow(browseWindow) {
        }

        void run(threaded_process_status& status, abort_callback& abort) override
        {
            try
            {
                do_import(status, abort);
            }
            catch (const exception_aborted&)
            {
                throw; // reported through on_done(aborted = true)
            }
            catch (const std::exception& e)
            {
                m_error = std::string("Import failed: ") + e.what();
            }
            catch (...)
            {
                m_error = "Import failed: unexpected error.";
            }
        }

        void on_done(HWND, bool aborted) override
        {
            const std::string kTitle = "Import stations from " + m_source;
            const HWND owner = core_api::get_main_window();

            if (aborted)
            {
                std::string text = "Import cancelled; no files created.";
                if (m_created > 0)
                {
                    text = "Import cancelled; " + std::to_string(m_created) +
                        " file(s) were already created in:\r\n" + wide_to_utf8(m_folder);
                }
                report_result(kTitle.c_str(), text, text);
                return;
            }

            if (!m_error.empty())
            {
                popup_message::g_show(m_error.c_str(), kTitle.c_str(), popup_message::icon_error);
                return;
            }

            if (!m_info.empty())
            {
                report_result(kTitle.c_str(), m_info, m_info);
                return;
            }

            // Browse windows append to the service's playlist (set in
            // preferences); the "Import from..." commands create a new playlist
            // named like the search folder (search term and date).
            const std::string playlistNote = m_browseWindow
                ? add_to_service_playlist(
                    m_source == "Icecast Directory" ? utag_service::icecast : utag_service::radio_browser,
                    m_createdPaths, owner)
                : add_to_new_folder_playlist(m_folder, m_createdPaths, owner);

            std::ostringstream results;
            results << kTitle << " - Results"
                << "\r\n\r\n" << "Output folder: " << wide_to_utf8(m_folder)
                << "\r\n" << "Stations returned: " << m_stationCount;

            if (!playlistNote.empty())
                results << "\r\n" << playlistNote;

            results << "\r\n\r\n" << std::string(64, '-');

            for (const auto& entry : m_logEntries)
                results << "\r\n\r\n" << entry;

            if (m_loggedCount > kRbMaxDetailedEntries)
            {
                results << "\r\n\r\n... and " << (m_loggedCount - kRbMaxDetailedEntries)
                    << " more (details are shown for the first "
                    << kRbMaxDetailedEntries << " stations).";
            }

            results << "\r\n\r\n" << std::string(64, '-')
                << "\r\n\r\n" << "Summary: Created " << m_created << " | Skipped " << m_skipped
                << " | Failed " << m_failed;

            std::ostringstream shortSummary;
            shortSummary << "Created " << m_created << " | Skipped " << m_skipped << " | Failed " << m_failed << " - " << wide_to_utf8(m_folder);

            const std::string resultsText = results.str();
            report_result(kTitle.c_str(), resultsText, shortSummary.str());
        }

    private:
        // Worker thread: no UI, playlist or config access in here.
        void do_import(threaded_process_status& status, abort_callback& abort)
        {
            json stations = json::array();

            if (m_preset)
            {
                for (auto& station : m_presetStations)
                    stations.push_back(std::move(station));
                m_presetStations.clear();
            }
            else
            {
                status.set_item("Downloading the station list...");

                std::string body;
                std::string error;
                if (!radio_download(build_radio_browser_path(m_query), abort, body, error))
                {
                    m_error = "Could not fetch stations from Radio Browser:\r\n" + error;
                    return;
                }

                stations = json::parse(body, nullptr, false);
                body.clear();

                if (stations.is_discarded() || !stations.is_array())
                {
                    m_error = "Radio Browser returned an unexpected response; no files created.";
                    return;
                }
            }

            if (stations.empty())
            {
                m_info = "No stations matched the search; no folder created.";
                return;
            }

            // Every search gets its own folder, named after the search.
            DWORD folderError = 0;
            if (!create_search_folder(m_parent, m_preset ? m_description : describe_radio_query(m_query),
                m_folder, folderError))
            {
                std::ostringstream message;
                message << "Could not create a folder in " << wide_to_utf8(m_parent)
                    << " (Windows error " << folderError << ").";
                m_error = message.str();
                return;
            }

            m_stationCount = stations.size();
            status.set_item("Creating .utag files...");

            // Stations that repeat a stream URL within this search are skipped.
            std::set<std::string> knownUrls;

            // Only the first entries are listed in detail; counts cover them all.
            const auto addLog = [this](std::string text)
                {
                    if (m_loggedCount++ < kRbMaxDetailedEntries)
                        m_logEntries.emplace_back(std::move(text));
                };

            size_t done = 0;

            for (const auto& station : stations)
            {
                abort.check();
                status.set_progress(done++, stations.size());

                std::string label;
                std::string url;
                std::string outputPath;

                try
                {
                    if (!station.is_object())
                        continue;

                    label = json_text(station, "name");
                    url = json_text(station, "url_resolved");
                    if (url.empty())
                        url = json_text(station, "url");

                    const bool httpUrl =
                        url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;

                    if (label.empty() || !httpUrl)
                    {
                        ++m_skipped;
                        addLog("[SKIPPED] " + (label.empty() ? std::string("(no name)") : label) +
                            "\n          No usable name or http(s) stream URL.");
                        continue;
                    }

                    if (!knownUrls.insert(url).second)
                    {
                        ++m_skipped;
                        addLog("[SKIPPED] " + label +
                            "\n          @  : " + url +
                            "\n          -> Another station in this search already uses this URL.");
                        continue;
                    }

                    const std::string contents =
                        json::array({ radio_station_to_utag(station, label, url) })
                        .dump(4, ' ', false, json::error_handler_t::replace) + "\n";

                    if (write_unique_utag(m_folder, make_safe_filename_base(label),
                        contents, outputPath))
                    {
                        ++m_created;
                        m_createdPaths.push_back(outputPath);
                        addLog("[CREATED] " + label +
                            "\n          @  : " + url +
                            "\n          -> " + outputPath);
                    }
                    else
                    {
                        ++m_skipped;
                        addLog("[SKIPPED] " + label +
                            "\n          @  : " + url +
                            "\n          -> Tried 9999 filename variants; no available filename.");
                    }
                }
                catch (const std::exception& e)
                {
                    ++m_failed;
                    std::string entry = "[ERROR] " + (label.empty() ? url : label);
                    entry += "\n        Source: " + url;
                    if (!outputPath.empty()) entry += "\n        Output: " + outputPath;
                    entry += "\n        Reason: ";
                    entry += e.what();
                    addLog(std::move(entry));
                }
            }
        }

        radio_query m_query;
        bool m_preset = false;                  // stations given, no search
        std::string m_source = "Radio Browser"; // service the stations come from
        bool m_browseWindow = false; // true: service playlist; false: new playlist per import
        std::string m_description;              // folder name for a preset import
        std::vector<json> m_presetStations;
        std::wstring m_parent;

        std::wstring m_folder;
        std::string m_error;
        std::string m_info;
        size_t m_stationCount = 0;
        std::vector<std::string> m_logEntries;
        std::vector<std::string> m_createdPaths; // for the playlist
        size_t m_loggedCount = 0;
        unsigned m_created = 0;
        unsigned m_skipped = 0;
        unsigned m_failed = 0;
    };

    void start_radio_import(const radio_query& query, const std::wstring& parentFolder)
    {
        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<radio_import_task>(query, parentFolder);

        // Modeless: a small progress window with an Abort button, while the
        // rest of foobar2000 stays usable.
        threaded_process::g_run_modeless(
            task,
            threaded_process::flag_show_abort | threaded_process::flag_show_progress,
            core_api::get_main_window(),
            "Importing stations from Radio Browser...");
    }

    void import_from_radio_browser(HWND owner)
    {
        open_radio_query_dialog(owner);
    }
}

// ---- shared with the browse window (utag_radio_browse.cpp) -------------------

bool radio_fetch(const std::string& path, abort_callback& abort,
    std::string& body, std::string& error)
{
    return radio_download(path, abort, body, error);
}

bool radio_pick_tag(HWND owner, std::string& selected)
{
    return choose_radio_browser_tag(owner, selected);
}

bool radio_pick_country(HWND owner, std::string& selected)
{
    return choose_radio_browser_country(owner, selected);
}

bool radio_pick_codec(HWND owner, std::string& selected)
{
    return choose_radio_browser_codec(owner, selected);
}

bool radio_pick_language(HWND owner, std::string& selected)
{
    return choose_radio_browser_language(owner, selected);
}

void radio_start_import_stations(const std::string& source,
    const std::string& description, std::vector<json> stations,
    const std::wstring& parentFolder, bool browseWindow)
{
    service_ptr_t<threaded_process_callback> task =
        new service_impl_t<radio_import_task>(source, description, std::move(stations),
            parentFolder, browseWindow);

    const std::string title = "Importing stations from " + source + "...";
    threaded_process::g_run_modeless(
        task,
        threaded_process::flag_show_abort | threaded_process::flag_show_progress,
        core_api::get_main_window(),
        title.c_str());
}

void utag_import_from_radio_browser(HWND owner)
{
    import_from_radio_browser(owner);
}

bool radio_lookup_station_by_url(const std::string& url, abort_callback& abort,
    json& entry, std::string& error)
{
    error.clear();

    std::string body;
    if (!radio_download("/json/stations/byurl?url=" + percent_encode(url), abort, body, error))
        return false;

    const json stations = json::parse(body, nullptr, false);
    if (stations.is_discarded() || !stations.is_array())
    {
        error = "Radio Browser returned an unexpected response.";
        return false;
    }

    const auto normalize = [](std::string text)
        {
            text = lowercase_ascii(text);
            while (!text.empty() && text.back() == '/')
                text.pop_back();
            return text;
        };

    const std::string wanted = normalize(url);
    const json* best = nullptr;

    for (const auto& station : stations)
    {
        if (!station.is_object() || trim_text(json_text(station, "name")).empty())
            continue;

        if (best == nullptr)
            best = &station; // fallback: first usable result

        if (normalize(json_text(station, "url")) == wanted ||
            normalize(json_text(station, "url_resolved")) == wanted)
        {
            best = &station;
            break;
        }
    }

    if (best == nullptr)
        return false; // not listed in Radio Browser

    entry = radio_station_to_utag(*best, trim_text(json_text(*best, "name")), url);
    return true;
}
