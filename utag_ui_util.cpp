#include "utag_ui.h"

bool pick_folder(
    HWND owner,
    const std::wstring& startDir,
    std::wstring& result)
{
    IFileOpenDialog* dialog = nullptr;

    if (FAILED(CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog))))
    {
        return false;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(
        options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose a folder for the station files");

    // Open at the remembered folder.
    if (!startDir.empty())
    {
        IShellItem* folder = nullptr;

        if (SUCCEEDED(SHCreateItemFromParsingName(
            startDir.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    bool ok = false;

    if (SUCCEEDED(dialog->Show(owner)))
    {
        IShellItem* item = nullptr;

        if (SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;

            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                result = path;
                CoTaskMemFree(path);
                ok = !result.empty();
            }

            item->Release();
        }
    }

    dialog->Release();
    return ok;
}

static void write_new_utf8_file(
    const std::wstring& fullPath,
    const std::string& contents)
{
    HANDLE file = CreateFileW(
        fullPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        throw exception_io_data();

    DWORD written = 0;
    const BOOL ok = WriteFile(
        file,
        contents.data(),
        static_cast<DWORD>(contents.size()),
        &written,
        nullptr);

    CloseHandle(file);

    if (!ok || written != contents.size())
    {
        DeleteFileW(fullPath.c_str());
        throw exception_io_data();
    }
}

static bool is_existing_directory(const std::wstring& path)
{
    if (path.empty())
        return false;

    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Returns the saved default folder, asking the user to choose one when none
// is saved yet (or the saved one no longer exists). `forcePick` always asks.
bool get_default_folder(std::wstring& folder, bool forcePick)
{
    std::wstring saved;

    try
    {
        if (g_cfg_default_dir.length() > 0)
            saved = utf8_to_wide(g_cfg_default_dir.get_ptr());
    }
    catch (...)
    {
    }

    if (!forcePick && is_existing_directory(saved))
    {
        folder = saved;
        return true;
    }

    if (!pick_folder(core_api::get_main_window(), saved, folder))
        return false;

    g_cfg_default_dir.set_string(
        pfc::stringcvt::string_utf8_from_wide(folder.c_str()).get_ptr());
    return true;
}

// Writes `contents` to "<folder>\<safeName>.utag", adding " (2)", " (3)", ...
// when the name is taken (CREATE_NEW never overwrites). Returns false when no
// free name was found; throws std::runtime_error on I/O errors. `outputPath`
// always holds the last path that was tried.
bool write_unique_utag(
    const std::wstring& folder,
    const std::wstring& safeName,
    const std::string& contents,
    std::string& outputPath)
{
    for (unsigned suffix = 1; suffix <= 9999; ++suffix)
    {
        std::wstring filename = safeName;
        if (suffix > 1) filename += L" (" + std::to_wstring(suffix) + L")";
        filename += L".utag";

        std::wstring fullPath = folder;
        if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/')
            fullPath += L'\\';
        fullPath += filename;
        outputPath = wide_to_utf8(fullPath);

        HANDLE file = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            std::ostringstream message;
            message << "Could not create output file (Windows error " << error << ").";
            throw std::runtime_error(message.str());
        }

        DWORD written = 0;
        const BOOL ok = WriteFile(file, contents.data(),
            static_cast<DWORD>(contents.size()), &written, nullptr);
        const DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!ok || written != contents.size())
        {
            DeleteFileW(fullPath.c_str());
            std::ostringstream message;
            message << "Could not write output file (Windows error " << writeError << ").";
            throw std::runtime_error(message.str());
        }

        return true;
    }

    return false;
}

std::wstring read_dialog_text(HWND dlg, int id)
{
    const HWND edit = GetDlgItem(dlg, id);
    const int len = GetWindowTextLengthW(edit);
    if (len <= 0)
        return {};

    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(edit, text.data(), len + 1);
    text.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return text;
}

std::wstring trim_wide(const std::wstring& text)
{
    const wchar_t* ws = L" \t\r\n";
    const size_t first = text.find_first_not_of(ws);
    if (first == std::wstring::npos)
        return {};
    const size_t last = text.find_last_not_of(ws);
    return text.substr(first, last - first + 1);
}

// Creates a new sub folder of `parent` named after the search plus the date
// and time, e.g. "Radio Browser - jazz - top 100 - 2026-10-18 14-32-05", so
// every search gets its own folder. (In the unlikely case that two searches
// start within the same second, " (2)" is added.)
bool create_search_folder(const std::wstring& parent, const std::string& description,
    std::wstring& folder, DWORD& error)
{
    std::wstring base = make_safe_filename_base(description);
    if (base.size() > 100)
        base.resize(100);
    while (!base.empty() && (base.back() == L'.' || base.back() == L' '))
        base.pop_back();
    if (base.empty())
        base = L"Radio Browser";

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02u %02u-%02u-%02u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    base += L" - ";
    base += stamp;

    std::wstring root = parent;
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
        root += L'\\';

    for (unsigned suffix = 1; suffix <= 99; ++suffix)
    {
        std::wstring name = base;
        if (suffix > 1)
            name += L" (" + std::to_wstring(suffix) + L")";

        const std::wstring path = root + name;
        if (CreateDirectoryW(path.c_str(), nullptr))
        {
            folder = path;
            return true;
        }

        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            return false;
    }

    return false;
}

// ---- small modal text prompt (shared by the browse windows)

namespace
{
    constexpr int kIdPrEdit = 1351;

    struct prompt_state
    {
        fb2k::CCoreDarkModeHooks darkHooks;
        std::wstring text;
    };

    INT_PTR CALLBACK prompt_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<prompt_state*>(GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
            state = reinterpret_cast<prompt_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            state->darkHooks.AddDialogWithControls(dlg);
            SetDlgItemTextW(dlg, kIdPrEdit, state->text.c_str());
            SendDlgItemMessageW(dlg, kIdPrEdit, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, kIdPrEdit));
            return FALSE; // focus set manually

        case WM_COMMAND:
            if (LOWORD(wp) == IDOK && state != nullptr)
            {
                state->text = trim_wide(read_dialog_text(dlg, kIdPrEdit));
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL)
            {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;

        default:
            break;
        }

        return FALSE;
    }
}

// Shows a one-line prompt. `value` is the initial text and receives the
// (trimmed) answer. Returns false if cancelled.
bool prompt_text(HWND owner, const wchar_t* title, const wchar_t* label, std::string& value)
{
    constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;

    dialog_template t;
    t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
        0, 0, 260, 72, title, L"Segoe UI", 8);

    t.item(kStatic, SS_LEFT, 8, 8, 244, 20, 0xFFFF, label);
    t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 30, 244, 13, kIdPrEdit, L"");
    t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 50, 50, 14, IDOK, L"OK");
    t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 202, 50, 50, 14, IDCANCEL, L"Cancel");

    prompt_state state;
    try { state.text = utf8_to_wide(value); } catch (...) {}

    const INT_PTR result = DialogBoxIndirectParamW(
        core_api::get_my_instance(), t.data(), owner,
        prompt_proc, reinterpret_cast<LPARAM>(&state));

    if (result != IDOK)
        return false;

    value = wide_to_utf8(state.text);
    return true;
}

// ---- result reporting

namespace
{
    // Prints `text` to the console, one line per call; `title` leads the first.
    void console_lines(const char* title, const std::string& text)
    {
        bool first = true;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty())
                continue;

            if (first)
                console::formatter() << "foo_utag: " << title << ": " << line.c_str();
            else
                console::formatter() << "foo_utag:   " << line.c_str();
            first = false;
        }
    }
}

void report_result(const char* title, const std::string& details, const std::string& summary)
{
    if (g_cfg_result_popups)
    {
        popup_message::g_show(details.c_str(), title, popup_message::icon_information);
        return;
    }

    console_lines(title, utag_console_logging_enabled() ? details : summary);
}

// -----------------------------------------------------------------------------
// Service playlists
// -----------------------------------------------------------------------------

const char* utag_default_playlist_name(utag_service service)
{
    switch (service)
    {
    case utag_service::tunein:  return "Tunein";
    case utag_service::icecast: return "Icecast";
    default:                    return "RadioBrowser";
    }
}

namespace
{
    // playlist_add_locations() works asynchronously, so the items only exist once
    // the locations have been resolved. This notify appends them to the playlist
    // and then focuses (and selects) the first appended item.
    class append_and_focus_notify : public process_locations_notify
    {
    public:
        explicit append_and_focus_notify(std::string playlistName)
            : m_name(std::move(playlistName)) {
        }

        void on_completion(metadb_handle_list_cref items) override
        {
            if (items.get_count() == 0)
                return;

            try
            {
                static_api_ptr_t<playlist_manager> playlists;

                // Looked up again by name: the playlist may have moved (or been
                // removed) while the locations were being resolved.
                const t_size index = playlists->find_or_create_playlist(
                    m_name.c_str(), m_name.length());
                if (index == pfc_infinite)
                    return;

                const t_size first = playlists->playlist_get_item_count(index);
                playlists->playlist_insert_items(index, first, items, bit_array_false());

                playlists->set_active_playlist(index);
                playlists->playlist_set_focus_item(index, first);
                playlists->playlist_set_selection(index, bit_array_true(), bit_array_one(first));
                playlists->playlist_ensure_visible(index, first);
            }
            catch (...)
            {
            }
        }

        void on_aborted() override {}

    private:
        std::string m_name;
    };
}

std::string add_to_service_playlist(utag_service service,
    const std::vector<std::string>& createdPaths, HWND owner)
{
    std::string name;
    switch (service)
    {
    case utag_service::tunein:  name = g_cfg_playlist_tunein.get_ptr(); break;
    case utag_service::icecast: name = g_cfg_playlist_icecast.get_ptr(); break;
    default:                    name = g_cfg_playlist_radiobrowser.get_ptr(); break;
    }

    const char* ws = " \t\r\n";
    const size_t first = name.find_first_not_of(ws);
    name = first == std::string::npos
        ? std::string()
        : name.substr(first, name.find_last_not_of(ws) - first + 1);
    if (name.empty())
        name = utag_default_playlist_name(service);

    if (createdPaths.empty())
        return {};

    try
    {
        std::vector<pfc::string8> canonical;
        canonical.reserve(createdPaths.size());
        for (const auto& path : createdPaths)
        {
            pfc::string8 canonicalPath;
            filesystem::g_get_canonical_path(path.c_str(), canonicalPath);
            canonical.push_back(canonicalPath);
        }

        pfc::list_t<const char*> locations;
        for (const auto& path : canonical)
            locations.add_item(path.get_ptr());

        static_api_ptr_t<playlist_manager> playlists;

        // Reuse the playlist when it exists, otherwise create it; new entries
        // are appended to whatever is already in it.
        t_size index = playlists->find_or_create_playlist(name.c_str(), name.length());
        if (index == pfc_infinite)
            index = playlists->create_playlist(name.c_str(), name.length(), pfc_infinite);
        if (index == pfc_infinite)
            throw std::runtime_error("the playlist could not be created");

        playlists->set_active_playlist(index);

        // Resolve the locations in the background; when done the items are
        // appended and the first one gets the focus.
        process_locations_notify::ptr notify =
            new service_impl_t<append_and_focus_notify>(name);

        static_api_ptr_t<playlist_incoming_item_filter_v2> filter;
        filter->process_locations_async(locations,
            playlist_incoming_item_filter_v2::op_flag_no_filter |
            playlist_incoming_item_filter_v2::op_flag_delay_ui,
            nullptr, nullptr, owner, notify);

        return "Playlist: " + name + " (" + std::to_string(createdPaths.size()) +
            " entries appended)";
    }
    catch (const std::exception& e)
    {
        return "Could not add to the \"" + name + "\" playlist: " + e.what();
    }
    catch (...)
    {
        return "Could not add to the \"" + name + "\" playlist: unknown error.";
    }
}

std::string add_to_new_folder_playlist(const std::wstring& folder,
    const std::vector<std::string>& createdPaths, HWND owner)
{
    if (createdPaths.empty())
        return {};

    try
    {
        const size_t slash = folder.find_last_of(L"\\/");
        const std::string playlistName = wide_to_utf8(
            slash == std::wstring::npos ? folder : folder.substr(slash + 1));

        std::vector<pfc::string8> canonical;
        canonical.reserve(createdPaths.size());
        for (const auto& path : createdPaths)
        {
            pfc::string8 canonicalPath;
            filesystem::g_get_canonical_path(path.c_str(), canonicalPath);
            canonical.push_back(canonicalPath);
        }

        pfc::list_t<const char*> locations;
        for (const auto& path : canonical)
            locations.add_item(path.get_ptr());

        static_api_ptr_t<playlist_manager> playlists;
        const t_size index = playlists->create_playlist(
            playlistName.c_str(), playlistName.length(), pfc_infinite);
        if (index == pfc_infinite)
            throw std::runtime_error("playlist creation failed");
        playlists->set_active_playlist(index);
        playlists->playlist_add_locations(index, locations, true, owner);

        return "Playlist: " + playlistName + " (" +
            std::to_string(createdPaths.size()) + " entries)";
    }
    catch (const std::exception& e)
    {
        return std::string("Could not create the playlist: ") + e.what();
    }
}
