#pragma once

// Shared includes and declarations for the foo_utag component.

#include <SDK/foobar2000.h>
#include <SDK/coreDarkMode.h>
#include <nlohmann/json.hpp>

#include <Windows.h>
#include <shlobj.h>
#include <urlmon.h>
#include <initializer_list>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>
#include <cmath>
#include <utility>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <fstream>
#include <thread>
#include <set>
#include <sstream>
#include <stdexcept>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Urlmon.lib")

using json = nlohmann::json;

static constexpr size_t kMaxStubSize = 4 * 1024 * 1024;

// Component GUID (defined in utag_component.cpp).
extern const GUID g_utag_guid;

// ---- utag_config.cpp: persisted options
extern cfg_bool g_cfg_handoff;
extern cfg_bool g_cfg_favicon;
extern cfg_bool g_cfg_logo_fileops;
extern cfg_bool g_cfg_logging;
extern cfg_bool g_cfg_result_popups;
extern cfg_bool g_cfg_tunein_details;
extern cfg_bool g_cfg_add_rb_lookup;
extern cfg_bool g_cfg_ctx_rb_lookup;
extern cfg_string g_cfg_default_dir;
extern cfg_string g_cfg_tunein_username;
extern cfg_string g_cfg_playlist_radiobrowser;
extern cfg_string g_cfg_playlist_tunein;
extern cfg_string g_cfg_playlist_icecast;

bool handoff_enabled();
bool favicon_download_enabled();
bool logo_fileops_enabled();
bool utag_console_logging_enabled();

// Convenience logging macro used throughout this file. Replace existing
// direct `console::printf(...)` calls with `UTAG_LOG(...)` to respect the preference.
#define UTAG_LOG(...) \
    do { if (utag_console_logging_enabled()) console::printf(__VA_ARGS__); } while(0)

// ---- utag_util.cpp: path, JSON and filename helpers
std::string to_native_windows_path(const std::string& path);
std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);
std::string basename_from_path(const std::string& path);
std::string lowercase_ascii(std::string value);
bool is_reserved_key(const std::string& key);
bool is_standard_tag_key(const std::string& key);
std::string json_value_to_string(const json& value);

// "GR" -> the flag emoji (two regional indicator symbols, UTF-8). Empty unless
// the code is exactly two ASCII letters.
std::string country_code_to_flag(const std::string& countryCode);

// When the station object has a "countrycode" field (any letter case), adds a
// "countryflag" field with the matching flag emoji, e.g. GR -> the Greek flag.
// An existing non-empty "countryflag" is left alone, unless `overwrite` is true
// (used when saving tags, so the flag always follows the country code).
void add_country_flag(json& station, bool overwrite = false);
double json_duration_to_seconds(const json& value);
bool is_local_file_reference(const std::string& url);
bool reference_is_utag(const std::string& ref);
bool is_utag_path(const std::string& path);
std::wstring make_safe_filename_base(const std::string& label);
std::string percent_encode(const std::string& text);

// Case-insensitive, locale-aware comparison of two UTF-8 names: <0, 0 or >0.
int compare_names(const std::string& a, const std::string& b);

// ---- utag_favicon.cpp: sidecar image helpers and favicon download
// Station artwork is a "sidecar" image beside the .utag file (same folder, same
// basename, image extension). A manually assigned logo and a downloaded favicon
// are the same kind of file. All `utagPath` parameters take a foobar2000 path
// ("file://..." or native) as returned by metadb_handle::get_path().
std::string favicon_extension_for_url(const std::string& url);
void schedule_favicon_download_for_playback(const std::string& utagPath, const json& station);
std::wstring image_extension_by_magic(const std::wstring& nativePath); // ".png" etc., empty = not an image
std::wstring station_artwork_stem(const std::string& utagPath);        // path without extension
std::wstring find_station_artwork(const std::string& utagPath);        // existing sidecar or empty
unsigned delete_station_artwork_except(const std::string& utagPath, const std::wstring& keep);
void notify_utag_artwork_changed(const std::string& utagPath);

// ---- utag_artwork.cpp: manually assigned station logos
bool set_station_logo_from_file(const std::string& utagPath, const std::wstring& imageFile, std::string& error);
bool remove_station_logo(const std::string& utagPath, std::string& error);

// ---- utag_handoff.cpp: stream handoff and technical-info cache
namespace utag_state
{
    void handoff_store(const std::string& url, const service_ptr_t<input_decoder>& decoder);
    bool handoff_take(const std::string& url, service_ptr_t<input_decoder>& out);
    void tech_store(const std::string& url, const file_info& src);
    bool tech_load(const std::string& url, file_info& dst);
    void tech_refresh_metadb(const std::string& path, const file_info& decoderInfo);
    void tech_clear(const std::string& url);
}
