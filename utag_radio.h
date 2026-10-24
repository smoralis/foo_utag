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
void radio_start_import_stations(const std::string& description,
    std::vector<json> stations, const std::wstring& parentFolder);
