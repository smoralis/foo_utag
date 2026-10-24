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
extern cfg_bool g_cfg_logging;
extern cfg_bool g_cfg_result_popups;
extern cfg_bool g_cfg_tunein_details;
extern cfg_string g_cfg_default_dir;
extern cfg_string g_cfg_tunein_username;

bool handoff_enabled();
bool favicon_download_enabled();
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
double json_duration_to_seconds(const json& value);
bool is_local_file_reference(const std::string& url);
bool reference_is_utag(const std::string& ref);
bool is_utag_path(const std::string& path);
std::wstring make_safe_filename_base(const std::string& label);
std::string percent_encode(const std::string& text);

// Case-insensitive, locale-aware comparison of two UTF-8 names: <0, 0 or >0.
int compare_names(const std::string& a, const std::string& b);

// ---- utag_favicon.cpp
std::string favicon_extension_for_url(const std::string& url);
void schedule_favicon_download_for_playback(const std::string& utagPath, const json& station);

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
