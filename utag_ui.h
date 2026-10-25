#pragma once

// Shared UI helpers for the foo_utag dialogs.

#include "utag_common.h"

// ---- utag_ui_util.cpp
bool pick_folder(HWND owner, const std::wstring& startDir, std::wstring& result);
bool get_default_folder(std::wstring& folder, bool forcePick);
bool write_unique_utag(const std::wstring& folder, const std::wstring& safeName,
    const std::string& contents, std::string& outputPath);
std::wstring read_dialog_text(HWND dlg, int id);
std::wstring trim_wide(const std::wstring& text);

// One-line modal prompt. `value` is the initial text and receives the trimmed
// answer; returns false if cancelled.
bool prompt_text(HWND owner, const wchar_t* title, const wchar_t* label, std::string& value);

// Informational result of an import / create command. Shows `details` in a
// popup, or, when result windows are switched off in preferences, writes the
// short `summary` to the console (the details too if console logging is on).
// Errors are not reported through this: they always get a popup.
void report_result(const char* title, const std::string& details, const std::string& summary);

// Import playlist used by the "Import from..." commands: a NEW playlist named
// like the search folder (search term and date). Returns a one-line note for
// the results log. Main thread only.
std::string add_to_new_folder_playlist(const std::wstring& folder,
    const std::vector<std::string>& createdPaths, HWND owner);

// Service playlists, used by the browse windows only (names are set in
// preferences; defaults RadioBrowser, Tunein, Icecast). Appends `createdPaths` (the new .utag files) to the
// service's playlist, creating it when missing, and makes it active. Returns a
// one-line note for the results log. Main thread only.
enum class utag_service { radio_browser, tunein, icecast };
const char* utag_default_playlist_name(utag_service service);
std::string add_to_service_playlist(utag_service service,
    const std::vector<std::string>& createdPaths, HWND owner);

bool create_search_folder(const std::wstring& parent, const std::string& description,
    std::wstring& folder, DWORD& error);

// ---- Dialog entry points (utag_add_station.cpp / utag_radio_browser.cpp)
void utag_open_station_dialog(HWND owner);
void utag_import_from_radio_browser(HWND owner);
void utag_open_tunein_dialog(HWND owner);   // utag_tunein.cpp (search import)
void utag_open_tunein_browser(HWND owner);  // utag_tunein_browser.cpp
void utag_open_radio_browse(HWND owner);    // utag_radio_browse.cpp
void utag_open_icecast_browse(HWND owner);  // utag_icecast_browse.cpp
// utag_artwork.cpp: modeless "Station logo" window (drop an image file or image
// URL on it, or Browse). `utagPaths` are foobar2000 paths of .utag files.
void utag_open_logo_dialog(HWND owner, const std::vector<std::string>& utagPaths);

// Small in-memory dialog template (no .rc file needed).
class dialog_template
{
public:
    void begin(DWORD style, short x, short y, short cx, short cy,
        const wchar_t* title, const wchar_t* font, WORD fontSize)
    {
        put<DWORD>(style | DS_SETFONT);
        put<DWORD>(0);
        countPos_ = buf_.size();
        put<WORD>(0); // item count, patched in data()
        put<short>(x); put<short>(y); put<short>(cx); put<short>(cy);
        put<WORD>(0); // no menu
        put<WORD>(0); // default dialog class
        put_str(title);
        put<WORD>(fontSize);
        put_str(font);
    }

    void item(WORD cls, DWORD style, short x, short y, short cx, short cy,
        WORD id, const wchar_t* text)
    {
        align(4);
        put<DWORD>(style | WS_CHILD | WS_VISIBLE);
        put<DWORD>(0);
        put<short>(x); put<short>(y); put<short>(cx); put<short>(cy);
        put<WORD>(id);
        put<WORD>(0xFFFF);
        put<WORD>(cls);
        put_str(text);
        put<WORD>(0); // no creation data
        ++count_;
    }

    const DLGTEMPLATE* data()
    {
        std::memcpy(buf_.data() + countPos_, &count_, sizeof(WORD));
        return reinterpret_cast<const DLGTEMPLATE*>(buf_.data());
    }

private:
    void align(size_t a)
    {
        while (buf_.size() % a) buf_.push_back(0);
    }

    template <typename T>
    void put(T value)
    {
        align(sizeof(T) < 4 ? sizeof(T) : 4);
        const BYTE* p = reinterpret_cast<const BYTE*>(&value);
        buf_.insert(buf_.end(), p, p + sizeof(T));
    }

    void put_str(const wchar_t* text)
    {
        align(2);
        do { put<WORD>(static_cast<WORD>(*text)); } while (*text++);
    }

    std::vector<BYTE> buf_;
    size_t countPos_ = 0;
    WORD count_ = 0;
};
