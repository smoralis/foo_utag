#pragma once

// Radio Browser services shared between utag_radio_browser.cpp (search import)
// and utag_radio_browse.cpp (browse window).

#include "utag_ui.h"

// Downloads a Radio Browser API path (e.g. "/json/tags"), trying the mirrors in
// turn. Returns false with `error` set on failure; throws exception_aborted
// when the user aborts. Safe to call from a worker thread.
bool radio_fetch(const std::string& path, abort_callback& abort,
    std::string& body, std::string& error);

// Pickers with a filter box. Return false if cancelled or on error.
bool radio_pick_tag(HWND owner, std::string& selected);
bool radio_pick_country(HWND owner, std::string& selected); // two-letter code
bool radio_pick_codec(HWND owner, std::string& selected);
bool radio_pick_language(HWND owner, std::string& selected);

// Creates one .utag per station object in a new sub folder of `parentFolder`
// (background task with progress window, results popup and playlist).
// `source` is the service name shown in the progress window, popups and results
// log ("Radio Browser", "Icecast Directory", ...); `description` names the folder.
// `browseWindow` true: the files are appended to the service's playlist from
// preferences (browse windows). false: a new playlist named like the folder is
// created (the "Import from..." commands).
void radio_start_import_stations(const std::string& source,
    const std::string& description, std::vector<json> stations,
    const std::wstring& parentFolder, bool browseWindow);

// Looks a stream URL up in Radio Browser (/json/stations/byurl) and returns a
// .utag station object for it ("@" is `url`, other fields from the directory).
// Returns true when found. Returns false with `error` empty when Radio Browser
// has no such station, or with `error` set when the lookup itself failed.
// Throws exception_aborted on abort. Safe to call from a worker thread.
bool radio_lookup_station_by_url(const std::string& url, abort_callback& abort,
    json& entry, std::string& error);
