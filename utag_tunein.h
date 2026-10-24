#pragma once

// Internal declarations shared by utag_tunein_api.cpp, utag_tunein.cpp (search
// import window) and utag_tunein_browser.cpp (browser window).

#include "utag_ui.h"

enum class tunein_kind
{
    station,
    artist,
    song
};

// A station to turn into a .utag file.
struct tunein_candidate
{
    std::string name;
    std::string tuneUrl; // Tune.ashx URL
    std::string image;
    std::string guideId;
};

// One row of a TuneIn result list.
struct tunein_entry
{
    enum class kind_t
    {
        header,  // section title, not selectable
        note,    // informational text
        link,    // Browse.ashx level, open with Open / double-click
        station, // Tune.ashx item, can be imported
        action   // browser-defined command (home level only)
    };

    kind_t kind = kind_t::note;
    std::string label; // text shown in the list
    std::string name;  // plain station / category name
    std::string url;
    std::string image;
    std::string guideId;
    std::string item;  // TuneIn "item" field (station, show, ...)
    int action = 0;
};

struct tunein_search_request
{
    tunein_kind kind = tunein_kind::station;
    std::string text;
    long limit = 25;
};

// ---- utag_tunein_api.cpp
std::string ti_trim(const std::string& text);
std::string tunein_url(const std::string& pathAndQuery);
std::string tunein_search_url(tunein_kind kind, const std::string& text);

// Downloads and parses a JSON reply. Throws exception_aborted on abort.
bool tunein_fetch_json(const std::string& url, abort_callback& abort,
    json& out, std::string& error);

std::string tunein_head_title(const json& response);
void tunein_parse_entries(const json& body, std::vector<tunein_entry>& out);
void tunein_parse_genres(const json& body, std::vector<tunein_entry>& out);

// Background imports (modeless progress window, results popup, playlist).
void tunein_start_import(const std::string& description,
    std::vector<tunein_candidate> candidates, const std::wstring& parentFolder);
void tunein_start_search_import(const tunein_search_request& request,
    const std::wstring& parentFolder);
