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

DECLARE_COMPONENT_VERSION(
    "utag Input",
    "1.0.6",
    "Adds internet radio streams and other remote URLs to the Media Library.\n"
    "\n"
    "Each .utag file is a small JSON stub that stores a stream URL (\"@\") together\n"
    "with its tags. foobar2000 treats the stub like a normal track: it can be\n"
    "scanned into the library, tagged, and played, while the audio is decoded\n"
    "from the original URL.\n"
    "\n"
    "Features:\n"
    "- Library-friendly: scanning never connects to the stream\n"
    "- Editable tags, saved back into the .utag file\n"
    "- Stream URL shown in Properties under Other (%__@%)\n"
    "- Playlist context menu: Tagging > Create utag files...\n"
    "- Set default utag folder... / Create utag files in default folder: no prompt\n"
    "- Main menu: File > Add internet radio url and create utag (enter a URL, get a .utag)\n"
    "- Main menu: File > Import stations from Radio Browser... (creates many .utag files)\n"
    "- Optional playback-time favicon download (Preferences > Advanced > Tools)\n"
    "\n"
    "utag format: [ { \"@\": \"http://host:port/stream\", \"name\": \"Station\", ... } ]"
);


VALIDATE_COMPONENT_FILENAME("foo_utag.dll");

static constexpr size_t kMaxStubSize = 4 * 1024 * 1024;

// {C46BA00F-5FB9-4E95-9A9C-990B65400232}
static const GUID g_utag_guid =
{ 0xc46ba00f, 0x5fb9, 0x4e95, { 0x9a, 0x9c, 0x99, 0xb, 0x65, 0x40, 0x2, 0x32 } };



// -----------------------------------------------------------------------------
// Path helpers
// -----------------------------------------------------------------------------

static std::string to_native_windows_path(const std::string& path)
{
    constexpr char prefix[] = "file://";

    if (path.compare(0, sizeof(prefix) - 1, prefix) == 0)
        return path.substr(sizeof(prefix) - 1);

    return path;
}

static std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty())
        return {};

    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        text.c_str(), -1, nullptr, 0
    );

    if (count <= 0)
        throw exception_io_data();

    std::wstring result(static_cast<size_t>(count), L'\0');

    if (MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        text.c_str(), -1, result.data(), count
    ) <= 0)
    {
        throw exception_io_data();
    }

    result.resize(static_cast<size_t>(count - 1));
    return result;
}

static std::string basename_from_path(const std::string& path)
{
    const std::string nativePath = to_native_windows_path(path);
    const auto pos = nativePath.find_last_of("/\\");

    return pos == std::string::npos
        ? nativePath
        : nativePath.substr(pos + 1);
}

// -----------------------------------------------------------------------------
// JSON helpers
// -----------------------------------------------------------------------------

static std::string lowercase_ascii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    return value;
}

static bool is_reserved_key(const std::string& key)
{
    const std::string lower = lowercase_ascii(key);

    return lower == "@";
}

static bool is_standard_tag_key(const std::string& key)
{
    static const char* const keys[] = {
        "title",
        "artist",
        "album",
        "album artist",
        "album_artist",
        "date",
        "year",
        "genre",
        "tracknumber",
        "track number",
        "discnumber",
        "disc number",
        "composer",
        "performer",
        "comment",
        "copyright",
        "publisher",
        "label",
        "lyrics",
        "bpm",
        "rating",
        "replaygain_track_gain",
        "replaygain_track_peak",
        "replaygain_album_gain",
        "replaygain_album_peak",
        "totaltracks",
        "totaldiscs",
        "tracktotal",
        "disctotal"
    };

    const std::string lower = lowercase_ascii(key);

    for (const auto* candidate : keys)
    {
        if (lower == candidate)
            return true;
    }

    return false;
}

static std::string json_value_to_string(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();

    if (value.is_null())
        return {};

    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";

    if (value.is_number())
        return value.dump();

    return value.dump();
}

static double json_duration_to_seconds(const json& value)
{
    try
    {
        if (value.is_number())
        {
            const double result = value.get<double>();

            return std::isfinite(result) && result > 0.0
                ? result
                : 0.0;
        }

        if (value.is_string())
        {
            const std::string text = value.get<std::string>();
            size_t consumed = 0;
            const double result = std::stod(text, &consumed);

            if (consumed == text.size() &&
                std::isfinite(result) &&
                result > 0.0)
            {
                return result;
            }
        }
    }
    catch (...)
    {
    }

    return 0.0;
}

static bool is_local_file_reference(const std::string& url)
{
    // file:// references, including file://C:\Music\track.m4a
    if (url.size() >= 7) {
        const char prefix[] = "file://";
        bool matches = true;

        for (size_t i = 0; i < 7; ++i) {
            char c = url[i];

            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }

            if (c != prefix[i]) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return true;
        }
    }

    // Windows drive paths, e.g. C:\Music\track.m4a
    if (url.size() >= 3) {
        const char drive = url[0];

        const bool is_drive_letter =
            (drive >= 'A' && drive <= 'Z') ||
            (drive >= 'a' && drive <= 'z');

        if (is_drive_letter &&
            url[1] == ':' &&
            (url[2] == '\\' || url[2] == '/')) {
            return true;
        }
    }

    // UNC paths, e.g. \\server\share\track.m4a
    if (url.size() >= 2 &&
        url[0] == '\\' &&
        url[1] == '\\') {
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// When the playing item is retagged, foobar2000 closes the playing input and
// opens a new one, which reconnects the live stream (audible pause). With this
// option on, the closing instance parks its running decoder here for a few
// seconds, and a new instance for the same URL picks it up instead of
// reconnecting.
//
// Preferences > Advanced > Tools. On by default.
// -----------------------------------------------------------------------------

// {2A6D5E81-93C4-4B7F-A108-6ED34C9B5720}
static const GUID g_guid_adv_handoff =
{ 0x2a6d5e81, 0x93c4, 0x4b7f, { 0xa1, 0x08, 0x6e, 0xd3, 0x4c, 0x9b, 0x57, 0x20 } };

static advconfig_checkbox_factory g_adv_handoff(
    "utag: reuse stream when playing item is retagged",
    g_guid_adv_handoff,
    advconfig_branch::guid_branch_tools,
    0,
    true);

static bool handoff_enabled()
{
    return g_adv_handoff.get_static_instance().get_state();
}


// Optional playback-time favicon download. The image is stored beside the
// .utag file, using the same basename and the favicon URL's file extension.
static const GUID g_guid_adv_favicon =
{ 0x6f4f9b12, 0x8a3d, 0x4d2c, { 0x91, 0x70, 0x2b, 0x4c, 0x61, 0x7e, 0x35, 0xa9 } };

static advconfig_checkbox_factory g_adv_favicon(
    "utag: download favicon image during playback",
    g_guid_adv_favicon,
    advconfig_branch::guid_branch_tools,
    1,
    false);

static bool favicon_download_enabled()
{
    return g_adv_favicon.get_static_instance().get_state();
}

static const GUID g_guid_adv_logging =
{ 0x9d8c1a2b, 0x1234, 0x4a2b, { 0x91, 0x70, 0x2b, 0x4c, 0x61, 0x7e, 0x35, 0xb1 } };

static advconfig_checkbox_factory g_adv_logging(
    "utag: enable console logging",
    g_guid_adv_logging,
    advconfig_branch::guid_branch_tools,
    2,
    true);

static bool utag_console_logging_enabled()
{
    return g_adv_logging.get_static_instance().get_state();
}

// Convenience logging macro used throughout this file. Replace existing
// direct `console::printf(...)` calls with `UTAG_LOG(...)` to respect the preference.
#define UTAG_LOG(...) \
    do { if (utag_console_logging_enabled()) console::printf(__VA_ARGS__); } while(0)


static std::mutex g_faviconMutex;
static std::set<std::wstring> g_faviconInFlight;

// Helper: accepted URL file extensions (lowercase, include leading dot).
static bool is_known_image_ext(const std::string& ext)
{
    const std::string lower = lowercase_ascii(ext);
    static const char* const allowed[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg"
    };
    for (const char* a : allowed)
        if (lower == a) return true;
    return false;
}

static bool is_image_file_by_magic(const std::wstring& path)
{
    // Read a few leading bytes and check common image signatures.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    unsigned char buf[16] = { 0 };
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return false;

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (got >= 8 &&
        buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G' &&
        buf[4] == 0x0D && buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A)
        return true;

    // JPEG: FF D8 FF
    if (got >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)
        return true;

    // GIF: "GIF87a" or "GIF89a"
    if (got >= 6 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F' &&
        buf[3] == '8' && (buf[4] == '7' || buf[4] == '9') && buf[5] == 'a')
        return true;

    // BMP: 'B' 'M'
    if (got >= 2 && buf[0] == 'B' && buf[1] == 'M')
        return true;

    // WebP: "RIFF"...."WEBP" (bytes 0..3 == 'R','I','F','F' and 8..11 == 'W','E','B','P')
    if (got >= 12 &&
        buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
        buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P')
        return true;

    // Heuristic fallback: SVG / XML (starts with '<') � treat as image if it looks like an SVG tag.
    if (buf[0] == '<')
    {
        // check for "<?xml" or "<svg"
        const std::string s(reinterpret_cast<char*>(buf), static_cast<size_t>(got));
        auto pos = s.find("svg");
        if (pos != std::string::npos) return true;
        pos = s.find("<?xml");
        if (pos != std::string::npos) return true;
    }

    // NOTE: ICO support intentionally removed � do not accept ICO signature.

    return false;
}


// Ask foobar2000 to refresh the .utag metadata after its favicon sidecar
// has been successfully published. This gives artwork viewers a chance to
// query the extractor again; it is intentionally never called on download failure.
static void notify_utag_artwork_changed(const std::string& utagPath)
{
    try
    {
        metadb_handle_ptr handle = metadb::get()->handle_create(utagPath.c_str(), 0);
        file_info_impl info;
        if (!handle->get_info(info))
            return;

        const t_filestats stats = handle->get_filestats();
        fb2k::inMainThread([handle, info, stats]() {
            try
            {
                auto hints = metadb_hint_list_v3::create();
                hints->add_hint_forced(handle, info, stats, true);
                hints->on_done();
            }
            catch (...)
            {
            }
            });
    }
    catch (...)
    {
    }
}
static void schedule_favicon_download_for_playback(
    const std::string& utagPath,
    const json& station)
{
    if (!favicon_download_enabled())
        return;

    const auto it = station.find("favicon");
    if (it == station.end() || !it->is_string())
        return;

    const std::string url = it->get<std::string>();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return;

    try
    {
        // Use a native Windows path (strip "file://") before converting to wide.
        const std::string nativeUtagPath = to_native_windows_path(utagPath);
        const std::wstring widePath = utf8_to_wide(nativeUtagPath);

        const size_t slash = widePath.find_last_of(L"\\/");
        const size_t dot = widePath.find_last_of(L'.');
        if (dot == std::wstring::npos ||
            (slash != std::wstring::npos && dot < slash))
            return;

        // Preserve the original image extension from the favicon URL path.
        // Ignore query/fragment strings (for example, icon.png?v=2). Accept only
        // known image extensions; otherwise fall back to .png to avoid .ico.
        std::wstring extension = L".png";
        const size_t urlQuery = url.find_first_of("?#");
        const std::string urlPath = url.substr(0, urlQuery);
        const size_t urlDot = urlPath.find_last_of('.');
        const size_t urlSlash = urlPath.find_last_of('/');
        if (urlDot != std::string::npos &&
            (urlSlash == std::string::npos || urlDot > urlSlash))
        {
            const std::string ext = urlPath.substr(urlDot); // includes leading dot
            if (ext.size() >= 2 && ext.size() <= 12 && is_known_image_ext(ext))
                extension = utf8_to_wide(ext);
        }

        const std::wstring imagePath = widePath.substr(0, dot) + extension;

        // only skip when a file already exists at imagePath and it is not a directory.
        const DWORD attrs = GetFileAttributesW(imagePath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            UTAG_LOG("[foo_utag] favicon already exists, skipping download: %s",
                pfc::stringcvt::string_utf8_from_wide(imagePath.c_str()).get_ptr());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_faviconMutex);
            if (!g_faviconInFlight.insert(imagePath).second)
                return;
        }

        UTAG_LOG("[foo_utag] starting favicon download: %s -> %s",
            url.c_str(), pfc::stringcvt::string_utf8_from_wide(imagePath.c_str()).get_ptr());

        std::thread([url, imagePath, utagPath]() {
            HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const std::wstring tempPath = imagePath + L".download.tmp";
            try
            {
                // Another playback may have completed the same download meanwhile.
                const DWORD existing = GetFileAttributesW(imagePath.c_str());
                if (existing != INVALID_FILE_ATTRIBUTES &&
                    (existing & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    std::lock_guard<std::mutex> lock(g_faviconMutex);
                    g_faviconInFlight.erase(imagePath);
                    if (SUCCEEDED(comResult)) CoUninitialize();
                    return;
                }

                // Download to a temporary file so artwork readers never see a
                // partially-written image.
                DeleteFileW(tempPath.c_str());
                const std::wstring wideUrl = utf8_to_wide(url);
                const HRESULT hr = URLDownloadToFileW(
                    nullptr, wideUrl.c_str(), tempPath.c_str(), 0, nullptr);

                if (SUCCEEDED(hr))
                {
                    // Validate that the downloaded file actually looks like an image.
                    if (!is_image_file_by_magic(tempPath))
                    {
                        UTAG_LOG("[foo_utag] downloaded file is not a recognized image, discarding: %s",
                            pfc::stringcvt::string_utf8_from_wide(tempPath.c_str()).get_ptr());
                        DeleteFileW(tempPath.c_str());
                    }
                    else if (!MoveFileExW(tempPath.c_str(), imagePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    {
                        UTAG_LOG("[foo_utag] could not publish downloaded favicon (Win32 error %lu)",
                            static_cast<unsigned long>(GetLastError()));
                        DeleteFileW(tempPath.c_str());
                    }
                    else
                    {
                        UTAG_LOG("[foo_utag] saved favicon: %s",
                            pfc::stringcvt::string_utf8_from_wide(imagePath.c_str()).get_ptr());
                        notify_utag_artwork_changed(utagPath);

                    }
                }
                else
                {
                    UTAG_LOG("[foo_utag] favicon download failed (HRESULT 0x%08X)",
                        static_cast<unsigned>(hr));
                    DeleteFileW(tempPath.c_str());
                }
            }
            catch (...)
            {
                DeleteFileW(tempPath.c_str());
            }

            {
                std::lock_guard<std::mutex> lock(g_faviconMutex);
                g_faviconInFlight.erase(imagePath);
            }
            if (SUCCEEDED(comResult)) CoUninitialize();
            }).detach();
    }
    catch (...)
    {
        // Scheduling failure must never prevent station playback.
    }
}

namespace
{
    // Retagging reopens the track within a moment; anything later is a
    // manual stop/replay and must get a fresh connection.
    constexpr ULONGLONG kHandoffMaxAgeMs = 2000;

    std::mutex g_handoffMutex;
    std::string g_handoffUrl;
    service_ptr_t<input_decoder> g_handoffDecoder;
    ULONGLONG g_handoffTime = 0;

    // Drops the parked decoder if it is stale (or always, when forced).
    // The decoder is released outside the lock, because closing the
    // connection can take a moment.
    void handoff_clear(bool force)
    {
        service_ptr_t<input_decoder> doomed;

        {
            std::lock_guard<std::mutex> lock(g_handoffMutex);

            if (!g_handoffDecoder.is_valid())
                return;

            if (force ||
                GetTickCount64() - g_handoffTime >= kHandoffMaxAgeMs)
            {
                doomed = g_handoffDecoder;
                g_handoffDecoder.release();
                g_handoffUrl.clear();
            }
        }
    }

    VOID CALLBACK handoff_timer(PVOID, BOOLEAN)
    {
        try
        {
            handoff_clear(false);
        }
        catch (...)
        {
        }
    }

    void handoff_store(
        const std::string& url,
        const service_ptr_t<input_decoder>& decoder)
    {
        service_ptr_t<input_decoder> old;

        {
            std::lock_guard<std::mutex> lock(g_handoffMutex);

            old = g_handoffDecoder;
            g_handoffDecoder = decoder;
            g_handoffUrl = url;
            g_handoffTime = GetTickCount64();
        }

        old.release();

        // If nobody picks it up (playback really stopped), close the stream
        // shortly after the handoff window ends.
        HANDLE timer = nullptr;
        CreateTimerQueueTimer(
            &timer,
            nullptr,
            handoff_timer,
            nullptr,
            static_cast<DWORD>(kHandoffMaxAgeMs + 500),
            0,
            WT_EXECUTEONLYONCE);
    }

    bool handoff_take(
        const std::string& url,
        service_ptr_t<input_decoder>& out)
    {
        std::lock_guard<std::mutex> lock(g_handoffMutex);

        if (!g_handoffDecoder.is_valid())
            return false;

        if (g_handoffUrl != url ||
            GetTickCount64() - g_handoffTime >= kHandoffMaxAgeMs)
        {
            return false;
        }

        out = g_handoffDecoder;
        g_handoffDecoder.release();
        g_handoffUrl.clear();
        return true;
    }

    // Close any parked stream when foobar2000 shuts down.
    class handoff_initquit : public initquit
    {
    public:
        void on_init() override {}

        void on_quit() override
        {
            handoff_clear(true);
        }
    };

    initquit_factory_t<handoff_initquit> g_handoff_initquit_factory;

    // -------------------------------------------------------------------------
    // Last known technical info (codec, bitrate, ...) of the playing stream.
    // Retag / Properties / library reloads open the stub without a decoder
    // (input_open_info_*), which would otherwise wipe the codec info.
    // -------------------------------------------------------------------------
    std::mutex g_techMutex;
    std::string g_techUrl;
    file_info_impl g_techInfo;
    bool g_techValid = false;

    void tech_store(const std::string& url, const file_info& src)
    {
        // Only cache info that actually names a codec.
        if (src.info_get("codec") == nullptr)
            return;

        std::lock_guard<std::mutex> lock(g_techMutex);
        g_techInfo.reset();
        g_techInfo.copy_info(src);
        g_techUrl = url;
        g_techValid = true;
    }

    bool tech_load(const std::string& url, file_info& dst)
    {
        std::lock_guard<std::mutex> lock(g_techMutex);

        if (!g_techValid || g_techUrl != url)
        {
            return false;
        }

        dst.copy_info(g_techInfo);
        return true;
    }

    // Apply playback-discovered technical info to foobar2000's metadata DB.
    // Properties reads the metadb snapshot; changing our private cache alone
    // does not update that snapshot. The forced hint is committed on main thread.
    class tech_info_metadb_hint_callback : public main_thread_callback
    {
    public:
        tech_info_metadb_hint_callback(
            metadb_handle_ptr handle, const file_info& info, const t_filestats& stats)
            : m_handle(std::move(handle)), m_stats(stats)
        {
            m_info.copy(info);
        }

        void callback_run() override
        {
            try
            {
                auto hints = metadb_hint_list_v3::create();
                hints->add_hint_forced(m_handle, m_info, m_stats, true);
                hints->on_done();
            }
            catch (const std::exception&)
            {
            }
        }

    private:
        metadb_handle_ptr m_handle;
        file_info_impl m_info;
        t_filestats m_stats;
    };

    void tech_refresh_metadb(const std::string& path, const file_info& decoderInfo)
    {
        try
        {
            metadb_handle_ptr handle = metadb::get()->handle_create(path.c_str(), 0);
            file_info_impl mergedInfo;
            if (!handle->get_info(mergedInfo))
            {
                return;
            }

            // Don't dispatch a database update if technical info is already current.
            if (file_info::g_is_info_equal(mergedInfo, decoderInfo))
                return;

            // Preserve the reserved stream URL before replacing technical info.
            // copy_info() replaces the complete technical-info section, so without
            // restoring "@" the Properties snapshot loses the URL on playback.
            const char* reservedUrlPtr = mergedInfo.info_get("@");
            const std::string reservedUrl =
                reservedUrlPtr != nullptr ? reservedUrlPtr : std::string();

            // Metadata tags and ReplayGain are separate from the technical-info
            // section and remain intact; restore "@" explicitly after the copy.
            mergedInfo.copy_info(decoderInfo);
            if (!reservedUrl.empty())
                mergedInfo.info_set("@", reservedUrl.c_str());

            const t_filestats stats = handle->get_filestats();
            fb2k::inMainThread([handle, mergedInfo, stats]() {
                try
                {
                    auto hints = metadb_hint_list_v3::create();
                    hints->add_hint_forced(handle, mergedInfo, stats, true);
                    hints->on_done();
                }
                catch (const std::exception&)
                {
                }
                });
        }
        catch (const std::exception&)
        {
        }
    }

    void tech_clear(const std::string& url)
    {
        std::lock_guard<std::mutex> lock(g_techMutex);

        if (g_techValid && g_techUrl == url)
        {
            g_techInfo.reset();
            g_techUrl.clear();
            g_techValid = false;
        }
    }
}

// -----------------------------------------------------------------------------
// Input component
// -----------------------------------------------------------------------------

class input_utag : public input_stubs
{
public:
    static GUID g_get_guid()
    {
        return g_utag_guid;
    }

    static const char* g_get_name()
    {
        return "utag Input";
    }

    static bool g_is_our_path(const char* path, const char*)
    {
        if (path == nullptr)
            return false;

        const std::string value(path);

        if (value.size() < 5)
            return false;

        const std::string extension = value.substr(value.size() - 5);

        return extension == ".utag"
            || extension == ".UTAG";
    }

    static bool g_is_our_content_type(const char*)
    {
        return false;
    }

    ~input_utag()
    {
        if (m_initialized &&
            !m_aborted &&
            m_decoder.is_valid() &&
            m_reason == input_open_decode &&
            handoff_enabled() &&
            !is_local_file_reference(m_url))
        {
            if (utag_console_logging_enabled()) console::formatter() << "[foo_utag] handing off decoder: @ = "
                << m_url.c_str();

            handoff_store(m_url, m_decoder);
        }
    }

    // -------------------------------------------------------------------------
    // Open and parse the JSON array
    // -------------------------------------------------------------------------

    void open(
        service_ptr_t<file> fileHint,
        const char* path,
        t_input_open_reason reason,
        abort_callback& abort)
    {
        (void)fileHint;

        // Library scans use input_open_info_read; playback uses input_open_decode.
        m_reason = reason;

        if (path == nullptr || *path == '\0')
            throw exception_io_data();

        m_file.release();
        m_decoder.release();
        m_initialized = false;
        m_handedOff = false;
        m_reused = false;
        m_aborted = false;

        m_path = path;
        m_url.clear();
        m_displayName.clear();
        m_json = json::object();

        filesystem::g_open_read(m_file, path, abort);

        const t_filesize fileSize = m_file->get_size_ex(abort);

        if (fileSize == 0 || fileSize > kMaxStubSize)
            throw exception_io_data();

        std::string buffer(static_cast<size_t>(fileSize), '\0');

        m_file->seek(0, abort);
        m_file->read_object(
            buffer.data(),
            static_cast<t_size>(buffer.size()),
            abort
        );

        // Keep the stub's stats, then close it. Holding the .utag open for the
        // whole playback would make foobar2000 release/reopen playback when
        // the stub is rewritten.
        m_stats = m_file->get_stats2_(stats2_all, abort);
        m_file.release();

        // UTF-8 BOM support.
        if (buffer.size() >= 3 &&
            static_cast<unsigned char>(buffer[0]) == 0xEF &&
            static_cast<unsigned char>(buffer[1]) == 0xBB &&
            static_cast<unsigned char>(buffer[2]) == 0xBF)
        {
            buffer.erase(0, 3);
        }

        json parsed = json::parse(buffer, nullptr, false);

        // Required format: [ { station fields... } ]
        if (parsed.is_discarded() ||
            !parsed.is_array() ||
            parsed.size() != 1 ||
            !parsed[0].is_object())
        {
            throw exception_io_data();
        }

        // Keep the station object internally. Serialization wraps it in an array.
        m_json = std::move(parsed[0]);

        // "@" is the stream URL used for playback.
        const auto urlIt = m_json.find("@");

        if (urlIt == m_json.end() ||
            !urlIt->is_string() ||
            urlIt->get<std::string>().empty())
        {
            throw exception_io_data();
        }

        m_url = urlIt->get<std::string>();

        // Download optional station artwork only when this .utag is opened
        // for actual playback, never during library scans or Properties reads.
        if (m_reason == input_open_decode)
            schedule_favicon_download_for_playback(m_path, m_json);

        // Prefer "name" for the station name.
        m_displayName = basename_from_path(m_path);

        const auto nameIt = m_json.find("name");

        if (nameIt != m_json.end() &&
            nameIt->is_string() &&
            !nameIt->get<std::string>().empty())
        {
            m_displayName = nameIt->get<std::string>();
        }
        else
        {
            const auto displayIt = m_json.find("display_name");

            if (displayIt != m_json.end() &&
                displayIt->is_string() &&
                !displayIt->get<std::string>().empty())
            {
                m_displayName = displayIt->get<std::string>();
            }
        }
    }

    // -------------------------------------------------------------------------
    // Metadata
    // -------------------------------------------------------------------------

    void get_info(file_info& info, abort_callback& abort)
    {
        bool hasTitle = false;

        // foobar2000 keeps ReplayGain in file_info's replaygain_info, not in
        // the meta fields; that is what the Properties dialog edits.
        replaygain_info replayGain = info.get_replaygain();
        bool hasReplayGain = false;

        // Read metadata from the .utag JSON object.
        for (auto it = m_json.begin(); it != m_json.end(); ++it)
        {
            const std::string& key = it.key();
            const json& value = it.value();

            if (is_reserved_key(key) || value.is_null())
                continue;

            if (replaygain_info::g_is_meta_replaygain(key.c_str()))
            {
                if (!value.is_array() && !value.is_object())
                {
                    const std::string text = json_value_to_string(value);
                    if (replayGain.set_from_meta(key.c_str(), text.c_str()))
                        hasReplayGain = true;
                }
                continue;
            }

            if (lowercase_ascii(key) == "title")
                hasTitle = true;

            if (value.is_array())
            {
                // Support multiple metadata values.
                for (const auto& item : value)
                {
                    if (item.is_null() ||
                        item.is_object() ||
                        item.is_array())
                    {
                        continue;
                    }

                    const std::string text = json_value_to_string(item);
                    info.meta_add(key.c_str(), text.c_str());
                }
            }
            else if (!value.is_object())
            {
                // Store scalar JSON values as metadata.
                const std::string text = json_value_to_string(value);
                info.meta_set(key.c_str(), text.c_str());
            }
        }

        if (hasReplayGain)
            info.set_replaygain(replayGain);

        // Use the display name as a title if JSON has no title.
        if (!hasTitle && !m_displayName.empty())
        {
            info.meta_set("title", m_displayName.c_str());
        }

        // Read duration from JSON, if supplied.
        const auto durationIt = m_json.find("duration");

        if (durationIt != m_json.end())
        {
            const double duration =
                json_duration_to_seconds(*durationIt);

            if (duration > 0.0)
            {
                info.set_length(duration);
            }
        }

        // The incoming file_info can already contain technical details from an
        // earlier Properties/library snapshot. Always clear that section first;
        // otherwise a cache miss leaves those stale fields visible.
        info.info_remove_all();

        // Technical info (codec, bitrate, ...) during playback must come only
        // from the decoder opened for this playback attempt.
        if (m_reason == input_open_decode)
        {
            tech_clear(m_url);

            try
            {
                ensure_decoder(abort);

                file_info_impl decoderInfo;
                m_decoder->get_info(0, decoderInfo, abort);

                if (decoderInfo.info_get("codec") != nullptr)
                {
                    info.copy_info(decoderInfo);
                    tech_store(m_url, decoderInfo);
                }
                // If the current decoder reports no codec, leave technical
                // info empty; never retain or restore an earlier snapshot.
            }
            catch (const exception_io&)
            {
                // Leave technical fields unset rather than restoring stale data.
            }
        }
        else
        {
            // Retag / Properties / library reload: no decoder is opened,
            // reuse the last known technical info for this URL.
            tech_load(m_url, info);
        }

        // Set last so copy_info() above cannot overwrite it.
        // Shows up in Properties under "Other" (info section), %__@%.
        info.info_set("@", m_url.c_str());
    }

    t_filestats2 get_stats2(uint32_t flags, abort_callback& abort)
    {
        (void)flags;
        (void)abort;
        return m_stats;
    }

    static void log_utag_info(const char* stage, const char* url)
    {
        UTAG_LOG(
            "[foo_utag] %s: @ = %s",
            stage,
            url != nullptr ? url : "(null)"
        );
    }

    // -------------------------------------------------------------------------
    // Decode through the URL's decoder
    // -------------------------------------------------------------------------

    void ensure_decoder(abort_callback& abort, bool allowHandoff = true)
    {
        if (m_decoder.is_valid())
            return;

        // A new playback-open attempt must not inherit technical details from
        // an earlier connection that happened to use the same URL.
        if (m_reason == input_open_decode)
            tech_clear(m_url);

        // Reuse the running decoder of the instance that was
        // just closed (e.g. because the playing item was retagged).
        if (allowHandoff &&
            m_reason == input_open_decode &&
            handoff_enabled() &&
            handoff_take(m_url, m_decoder))
        {
            m_handedOff = true;
            m_reused = true;
            UTAG_LOG("[foo_utag] reusing running stream: @ = %s", m_url.c_str());
            return;
        }

        // Open the URL ourselves and pass it as the file hint so that
        // foobar2000 can pick the decoder from the HTTP content type.
        service_ptr_t<file> urlFile;
        filesystem::g_open(
            urlFile,
            m_url.c_str(),
            filesystem::open_mode_read,
            abort
        );

        try
        {
            input_entry::g_open_for_decoding(
                m_decoder,
                urlFile,
                m_url.c_str(),
                abort
            );
        }
        catch (const std::exception& e)
        {
            UTAG_LOG("[foo_utag] open_for_decoding failed: %s", e.what());
            throw;
        }

        if (!m_decoder.is_valid())
            throw exception_io_data();
    }

    // Refresh the URL's cached technical info from the decoder already in use.
    // This is best-effort: metadata refresh must not prevent playback.
    void refresh_technical_info_cache(abort_callback& abort)
    {
        if (!m_decoder.is_valid())
            return;

        try
        {
            file_info_impl decoderInfo;
            m_decoder->get_info(0, decoderInfo, abort);

            if (decoderInfo.info_get("codec") != nullptr)
            {
                tech_store(m_url, decoderInfo);
                tech_refresh_metadb(m_path, decoderInfo);
            }
            else
            {
            }
        }
        catch (const exception_aborted&)
        {
            throw;
        }
        catch (const std::exception&)
        {
        }
    }

    void decode_initialize(unsigned flags, abort_callback& abort)
    {
        // Reuses the decoder get_info() already opened during playback,
        // so the stream is only connected once.
        ensure_decoder(abort);
        m_flags = flags;

        log_utag_info("decode_initialize", m_url.c_str());

        // A handed-off decoder is already initialized and mid-stream:
        // initializing it again would restart it.
        if (m_handedOff)
        {
            m_handedOff = false;
            m_initialized = true;
            refresh_technical_info_cache(abort);
            return;
        }

        // Forward the caller's flags (playback, no_seeking, no_looping, ...)
        m_decoder->initialize(0, flags, abort);
        m_initialized = true;

        // Some decoders expose more complete format details after initialize.
        // Refresh the cache without opening another connection.
        refresh_technical_info_cache(abort);
    }

    bool decode_run(audio_chunk& chunk, abort_callback& abort)
    {
        if (!m_decoder.is_valid())
            throw exception_io_data();

        // A handed-off decoder may have been left in a bad state (for example
        // by an aborted read). If it fails on its first run, drop it and
        // continue with a fresh connection instead of failing playback.
        if (m_reused)
        {
            try
            {
                const bool more = m_decoder->run(chunk, abort);
                m_reused = false;
                return more;
            }
            catch (const exception_aborted&)
            {
                m_aborted = true;
                throw;
            }
            catch (const std::exception& e)
            {
                UTAG_LOG(
                    "[foo_utag] reused stream failed (%s), reconnecting",
                    e.what());

                m_reused = false;
                m_decoder.release();
                m_initialized = false;

                ensure_decoder(abort, false);
                m_decoder->initialize(0, m_flags, abort);
                m_initialized = true;
            }
        }

        try
        {
            return m_decoder->run(chunk, abort);
        }
        catch (const exception_aborted&)
        {
            // Stop aborts a pending read; such a decoder must not be reused.
            m_aborted = true;
            throw;
        }
    }

    void decode_seek(double seconds, abort_callback& abort)
    {
        if (!m_decoder.is_valid())
            throw exception_io_data();

        m_decoder->seek(seconds, abort);
    }

    bool decode_can_seek()
    {
        return m_decoder.is_valid() && m_decoder->can_seek();
    }

    bool decode_get_dynamic_info(file_info& info, double& timestamp)
    {
        return m_decoder.is_valid()
            && m_decoder->get_dynamic_info(info, timestamp);
    }

    bool decode_get_dynamic_info_track(file_info& info, double& timestamp)
    {
        return m_decoder.is_valid()
            && m_decoder->get_dynamic_info_track(info, timestamp);
    }

    void decode_on_idle(abort_callback& abort)
    {
        if (m_decoder.is_valid())
            m_decoder->on_idle(abort);
    }

    // -------------------------------------------------------------------------
    // Write updated tags to the JSON utag
    // -------------------------------------------------------------------------

    void retag(const file_info& info, abort_callback& abort)
    {
        if (m_path.empty())
            throw exception_tagging_unsupported();

        json updated = m_json;

        // The "@" field is a reserved URL key, not an editable metadata tag.
        // Preserve its original value regardless of what file_info contains.
        const auto reservedUrlIt = m_json.find("@");
        const json reservedUrl = reservedUrlIt != m_json.end()
            ? *reservedUrlIt
            : json(m_url);

        // Remove old standard tags regardless of key capitalization.
        for (auto it = updated.begin(); it != updated.end();)
        {
            if (is_standard_tag_key(it.key()))
                it = updated.erase(it);
            else
                ++it;
        }

        for (t_size i = 0; i < info.meta_get_count(); ++i)
        {
            abort.check();

            const char* name = info.meta_enum_name(i);

            if (name == nullptr)
                continue;

            const std::string key(name);

            // ReplayGain is written from replaygain_info below.
            if (is_reserved_key(key) ||
                replaygain_info::g_is_meta_replaygain(key.c_str()))
            {
                continue;
            }

            const t_size count = info.meta_enum_value_count(i);
            std::vector<std::string> values;

            for (t_size j = 0; j < count; ++j)
            {
                const char* value = info.meta_enum_value(i, j);

                if (value != nullptr)
                    values.emplace_back(value);
            }

            if (values.empty())
                continue;

            if (values.size() == 1)
            {
                updated[key] = values.front();
            }
            else
            {
                json valuesArray = json::array();

                for (const auto& value : values)
                    valuesArray.push_back(value);

                updated[key] = std::move(valuesArray);
            }
        }

        // ReplayGain values as edited in Properties (old ones were erased above,
        // so clearing a field removes it from the file).
        const replaygain_info replayGain = info.get_replaygain();
        replaygain_info::t_text_buffer buffer;

        if (replayGain.format_track_gain(buffer))
            updated["replaygain_track_gain"] = buffer;
        if (replayGain.format_track_peak(buffer))
            updated["replaygain_track_peak"] = buffer;
        if (replayGain.format_album_gain(buffer))
            updated["replaygain_album_gain"] = buffer;
        if (replayGain.format_album_peak(buffer))
            updated["replaygain_album_peak"] = buffer;

        // Restore the reserved URL after all metadata edits; Properties must
        // never overwrite it.
        updated["@"] = reservedUrl;

        commit_json(std::move(updated), abort);
    }

    void remove_tags(abort_callback& abort)
    {
        if (m_path.empty())
            throw exception_tagging_unsupported();

        json updated = m_json;
        const auto reservedUrlIt = m_json.find("@");
        const json reservedUrl = reservedUrlIt != m_json.end()
            ? *reservedUrlIt
            : json(m_url);

        for (auto it = updated.begin(); it != updated.end();)
        {
            if (is_standard_tag_key(it.key()))
                it = updated.erase(it);
            else
                ++it;
        }

        updated["@"] = reservedUrl;

        commit_json(std::move(updated), abort);
    }

private:
    // -------------------------------------------------------------------------
    // Release the read handle, write the file, then reopen it.
    // -------------------------------------------------------------------------

    void commit_json(json updated, abort_callback& abort)
    {
        // The stub is not held open, so it can be rewritten directly.

        write_json(updated, abort);

        // Refresh cached stats from the rewritten file, then close it again.
        service_ptr_t<file> statFile;
        filesystem::g_open_read(statFile, m_path.c_str(), abort);
        m_stats = statFile->get_stats2_(stats2_all, abort);
        statFile.release();

        m_json = std::move(updated);
    }

    // -------------------------------------------------------------------------
    // Serialize the station object as a one-element JSON array.
    // -------------------------------------------------------------------------

    void write_json(const json& value, abort_callback& abort)
    {
        abort.check();

        const std::string contents =
            json::array({ value }).dump(4) + "\n";

        service_ptr_t<file> output;

        // Overwrites the existing .utag in place.
        filesystem::g_open_write_new(
            output,
            m_path.c_str(),
            abort
        );

        output->write_object(
            contents.data(),
            static_cast<t_size>(contents.size()),
            abort
        );

        output.release();
    }

    std::string m_url;
    std::string m_path;
    std::string m_displayName;

    t_input_open_reason m_reason = input_open_info_read;
    t_filestats2 m_stats = {};

    bool m_initialized = false;   // decoder->initialize() has been called
    bool m_handedOff = false;     // decoder came from the handoff cache
    bool m_reused = false;        // reused decoder has not yet run successfully
    bool m_aborted = false;       // a read was aborted (Stop): never reuse
    unsigned m_flags = 0;         // flags from decode_initialize()

    json m_json;

    service_ptr_t<file> m_file;
    service_ptr_t<input_decoder> m_decoder;
};

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

static input_singletrack_factory_t<input_utag>
g_input_utag_factory;


// Convert a Windows wide string to UTF-8 for readable result logs.
static std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), count, nullptr, nullptr) <= 0)
        return {};
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

// Scrollable, read-only results window for the Create utag files command.
namespace
{
    constexpr int kUtagResultsEditId = 5101;
    constexpr int kUtagResultsCloseId = 5102;

    static LRESULT CALLBACK utag_results_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_SIZE:
        {
            const int width = LOWORD(lp);
            const int height = HIWORD(lp);
            HWND edit = GetDlgItem(hwnd, kUtagResultsEditId);
            HWND closeButton = GetDlgItem(hwnd, kUtagResultsCloseId);
            if (edit) MoveWindow(edit, 12, 12, (std::max)(100, width - 24), (std::max)(100, height - 60), TRUE);
            if (closeButton) MoveWindow(closeButton, (std::max)(12, width - 100), (std::max)(12, height - 40), 88, 28, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == kUtagResultsCloseId || LOWORD(wp) == IDOK)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static void show_utag_results(HWND owner, const std::string& text)
    {
        static const wchar_t* className = L"FooUtagResultsWindow";
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = utag_results_wndproc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = className;
            if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
                registered = true;
        }
        if (!registered)
        {
            const std::wstring wide = utf8_to_wide(text);
            MessageBoxW(owner, wide.c_str(), L"Create utag files", MB_OK | MB_ICONINFORMATION);
            return;
        }

        HWND hwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME, className, L"Create utag files - Results",
            WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 760, 560, owner, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!hwnd)
        {
            const std::wstring wide = utf8_to_wide(text);
            MessageBoxW(owner, wide.c_str(), L"Create utag files", MB_OK | MB_ICONINFORMATION);
            return;
        }

        HWND edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            12, 12, 720, 480, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUtagResultsEditId)),
            GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            660, 500, 88, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUtagResultsCloseId)),
            GetModuleHandleW(nullptr), nullptr);
        if (edit)
        {
            const std::wstring wide = utf8_to_wide(text);
            SetWindowTextW(edit, wide.c_str());
            SendMessageW(edit, EM_SETSEL, 0, 0);
        }

        if (owner) EnableWindow(owner, FALSE);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        MSG msg;
        while (IsWindow(hwnd))
        {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0)
            {
                if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (!IsDialogMessageW(hwnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (owner)
        {
            EnableWindow(owner, TRUE);
            SetActiveWindow(owner);
        }
    }
}

// -----------------------------------------------------------------------------
// Playlist context-menu command: create one .utag file per selected track.
// -----------------------------------------------------------------------------

static const GUID g_create_utag_guid =
{ 0x4e9d17a2, 0x8c13, 0x4f26, { 0x91, 0x6b, 0x31, 0x82, 0x7a, 0x44, 0xc0, 0x19 } };

static std::string find_track_metadata(
    const file_info& info,
    std::initializer_list<const char*> keys)
{
    for (const char* wanted : keys)
    {
        for (t_size i = 0; i < info.meta_get_count(); ++i)
        {
            const char* name = info.meta_enum_name(i);
            if (name == nullptr)
                continue;

            if (lowercase_ascii(name) != lowercase_ascii(wanted))
                continue;

            if (info.meta_enum_value_count(i) == 0)
                continue;

            const char* value = info.meta_enum_value(i, 0);
            if (value != nullptr && *value != '\0')
                return value;
        }
    }

    return {};
}

static std::wstring make_safe_filename_base(const std::string& label)
{
    std::wstring result = utf8_to_wide(label);

    for (auto& ch : result)
    {
        if (ch < 32 || ch == L'<' || ch == L'>' ||
            ch == L':' || ch == L'"' || ch == L'/' ||
            ch == L'\\' || ch == L'|' || ch == L'?' ||
            ch == L'*')
        {
            ch = L'_';
        }
    }

    while (!result.empty() &&
        (result.back() == L'.' || result.back() == L' '))
    {
        result.pop_back();
    }

    if (result.empty())
        result = L"Station";

    if (result.size() >= 5 &&
        _wcsicmp(result.c_str() + result.size() - 5, L".utag") == 0)
    {
        result.resize(result.size() - 5);
    }

    if (result.empty())
        result = L"Station";

    return result;
}

static bool is_utag_path(const std::string& path)
{
    return path.size() >= 5 &&
        _stricmp(path.c_str() + path.size() - 5, ".utag") == 0;
}

// Last folder used by "Create utag files..." (persisted in foobar2000's config).
// {7B3F9C1E-2D54-4A8B-9E61-5C0D84F2A3B7}
static const GUID g_guid_cfg_last_dir =
{ 0x7b3f9c1e, 0x2d54, 0x4a8b, { 0x9e, 0x61, 0x5c, 0x0d, 0x84, 0xf2, 0xa3, 0xb7 } };

static cfg_string g_cfg_last_dir(g_guid_cfg_last_dir, "");

static bool pick_folder(
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

// Menu item: create utag files in the default folder, no prompt.
// {6D0AFA80-DC5A-4B66-AD50-545251FDA004}
static const GUID g_create_utag_auto_guid =
{ 0x6d0afa80, 0xdc5a, 0x4b66, { 0xad, 0x50, 0x54, 0x52, 0x51, 0xfd, 0xa0, 0x04 } };

// Menu item: choose the default utag folder.
// {545B0497-BF41-4F62-A79B-7DFE5A40E421}
static const GUID g_set_utag_folder_guid =
{ 0x545b0497, 0xbf41, 0x4f62, { 0xa7, 0x9b, 0x7d, 0xfe, 0x5a, 0x40, 0xe4, 0x21 } };

// Default output folder for automatic utag creation (persisted).
// {23AB3FAC-61AE-4376-A99C-0F044D60697F}
static const GUID g_guid_cfg_default_dir =
{ 0x23ab3fac, 0x61ae, 0x4376, { 0xa9, 0x9c, 0x0f, 0x04, 0x4d, 0x60, 0x69, 0x7f } };

static cfg_string g_cfg_default_dir(g_guid_cfg_default_dir, "");

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
static bool get_default_folder(std::wstring& folder, bool forcePick)
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
static bool write_unique_utag(
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

// Creates one .utag file per selected track inside `folder` and shows the
// results popup.
static void create_utag_files_in_folder(
    metadb_handle_list_cref data,
    const std::wstring& folder)
{
    unsigned created = 0;
    unsigned skipped = 0;
    unsigned failed = 0;
    std::vector<std::string> logEntries;
    logEntries.emplace_back("Create utag files - Results");
    logEntries.emplace_back("Output folder: " + wide_to_utf8(folder));
    logEntries.emplace_back(std::string(64, '-'));

    for (t_size i = 0; i < data.get_count(); ++i)
    {
        const char* path = data[i]->get_path();
        if (path == nullptr || *path == '\0')
        {
            ++skipped;
            logEntries.emplace_back("[SKIPPED] Playlist entry has no source path.");
            continue;
        }

        const std::string url(path);
        if (is_utag_path(url))
        {
            ++skipped;
            logEntries.emplace_back("[SKIPPED] Existing .utag input (not a stream URL): " + url);
            continue;
        }

        std::string filenameLabel;
        std::string outputPath;
        try
        {
            file_info_impl trackInfo;
            std::string stationName;
            std::string trackTitle;

            // Read metadata stored for this playlist entry.
            try
            {
                data[i]->get_info(trackInfo);
                stationName = find_track_metadata(
                    trackInfo,
                    { "station", "station name", "stationname", "icy-name", "radio", "name" });
                trackTitle = find_track_metadata(trackInfo, { "title" });
            }
            catch (...)
            {
                // Some entries may not expose metadata; use URL fallback.
            }

            filenameLabel = stationName;
            if (filenameLabel.empty()) filenameLabel = trackTitle;
            if (filenameLabel.empty()) filenameLabel = basename_from_path(url);
            if (filenameLabel.empty()) filenameLabel = "Station";

            const std::wstring safeName = make_safe_filename_base(filenameLabel);
            json station = json::object();
            station["name"] = stationName.empty() ? filenameLabel : stationName;
            if (!trackTitle.empty()) station["title"] = trackTitle;

            // Copy any other metadata fields and preserve multi-values.
            for (t_size m = 0; m < trackInfo.meta_get_count(); ++m)
            {
                const char* metaName = trackInfo.meta_enum_name(m);
                if (metaName == nullptr || *metaName == '\0') continue;
                const std::string key(metaName);
                const std::string lower = lowercase_ascii(key);
                if (is_reserved_key(key) || lower == "title") continue;
                const t_size count = trackInfo.meta_enum_value_count(m);
                if (count == 0) continue;
                json values = json::array();
                for (t_size v = 0; v < count; ++v)
                {
                    const char* value = trackInfo.meta_enum_value(m, v);
                    if (value != nullptr) values.push_back(value);
                }
                if (values.size() == 1) station[key] = values[0];
                else if (!values.empty()) station[key] = std::move(values);
            }

            station["@"] = url;
            const std::string contents = json::array({ station }).dump(4) + "\n";
            const bool saved = write_unique_utag(folder, safeName, contents, outputPath);
            if (saved)
            {
                ++created;
                logEntries.emplace_back("[CREATED] " + filenameLabel +
                    "\n          @  : " + url +
                    "\n          -> " + outputPath);
            }

            if (!saved)
            {
                ++skipped;
                logEntries.emplace_back("[SKIPPED] " + filenameLabel +
                    "\n          @  : " + url +
                    "\n          -> Tried 9999 filename variants; no available filename.");
            }
        }
        catch (const std::exception& e)
        {
            ++failed;
            std::string entry = "[ERROR] ";
            entry += filenameLabel.empty() ? url : filenameLabel;
            entry += "\n        Source: " + url;
            if (!outputPath.empty()) entry += "\n        Output: " + outputPath;
            entry += "\n        Reason: ";
            entry += e.what();
            logEntries.emplace_back(std::move(entry));
        }
        catch (...)
        {
            ++failed;
            std::string entry = "[ERROR] ";
            entry += filenameLabel.empty() ? url : filenameLabel;
            entry += "\n        Source: " + url + "\n        Reason: Unknown error.";
            if (!outputPath.empty()) entry += "\n        Output: " + outputPath;
            logEntries.emplace_back(std::move(entry));
        }
    }

    logEntries.emplace_back(std::string(64, '-'));
    std::ostringstream summary;
    summary << "Summary: Created " << created << " | Skipped " << skipped
        << " | Failed " << failed;
    logEntries.emplace_back(summary.str());

    std::ostringstream results;
    for (size_t i = 0; i < logEntries.size(); ++i)
    {
        if (i != 0) results << "\r\n\r\n";
        results << logEntries[i];
    }
    // Show the per-entry log in foobar2000's existing popup message,
    // rather than opening a separate custom results window.
    const std::string resultsText = results.str();
    popup_message::g_show(
        resultsText.c_str(),
        "Create utag files",
        popup_message::icon_information);
}

class create_utag_context_menu : public contextmenu_item_simple
{
public:
    enum { cmd_create = 0, cmd_create_auto, cmd_set_folder, cmd_count };

    unsigned get_num_items() override
    {
        return cmd_count;
    }

    void get_item_name(unsigned index, pfc::string_base& out) override
    {
        switch (index)
        {
        case cmd_create:     out = "Create utag files..."; break;
        case cmd_create_auto: out = "Create utag files in default folder"; break;
        case cmd_set_folder: out = "Set default utag folder..."; break;
        }
    }

    GUID get_item_guid(unsigned index) override
    {
        switch (index)
        {
        case cmd_create_auto: return g_create_utag_auto_guid;
        case cmd_set_folder:  return g_set_utag_folder_guid;
        default:              return g_create_utag_guid;
        }
    }

    GUID get_parent() override
    {
        return contextmenu_groups::tagging;
    }

    bool get_item_description(unsigned index, pfc::string_base& out) override
    {
        switch (index)
        {
        case cmd_create:
            out = "Create one .utag station file for each selected playlist track.";
            return true;
        case cmd_create_auto:
            out = "Create one .utag file per selected track in the default folder, without asking.";
            return true;
        case cmd_set_folder:
            out = "Choose the folder used by \"Create utag files in default folder\".";
            return true;
        }
        return false;
    }

    void context_command(
        unsigned index,
        metadb_handle_list_cref data,
        const GUID&) override
    {
        if (index == cmd_set_folder)
        {
            std::wstring folder;
            get_default_folder(folder, true);
            return;
        }

        if (data.get_count() == 0)
            return;

        if (index == cmd_create_auto)
        {
            std::wstring folder;

            // Cancelled: nothing to do.
            if (!get_default_folder(folder, false))
                return;

            create_utag_files_in_folder(data, folder);
            return;
        }

        std::wstring lastDir;

        try
        {
            if (g_cfg_last_dir.length() > 0)
                lastDir = utf8_to_wide(g_cfg_last_dir.get_ptr());
        }
        catch (...)
        {
        }

        std::wstring folder;

        // Cancelled or failed: nothing to do.
        if (!pick_folder(core_api::get_main_window(), lastDir, folder))
            return;

        // Remember this folder for next time.
        g_cfg_last_dir.set_string(
            pfc::stringcvt::string_utf8_from_wide(folder.c_str()).get_ptr());

        create_utag_files_in_folder(data, folder);
    }
};

// -----------------------------------------------------------------------------
// Main menu command: File > Add internet radio url and create utag
// Asks for a stream URL (and an optional station name) in a small input box,
// then writes a .utag file for it into the default utag folder.
// -----------------------------------------------------------------------------

// Main menu group that hosts the command. If it lands in the wrong block of the
// File menu, point this at another group from mainmenu_groups (mainmenu.h).
#define UTAG_MAINMENU_GROUP mainmenu_groups::file_add

// {A3C5E7F1-4B28-4D9A-8E36-1F7B92D0C456}
static const GUID g_add_url_utag_guid =
{ 0xa3c5e7f1, 0x4b28, 0x4d9a, { 0x8e, 0x36, 0x1f, 0x7b, 0x92, 0xd0, 0xc4, 0x56 } };

namespace
{
    constexpr int kIdUrlEdit = 1001;
    constexpr int kIdNameEdit = 1002;
    constexpr int kIdStatus = 1003;

    struct url_prompt_state
    {
        std::wstring url;
        std::wstring name;
        modal_dialog_scope scope;
        // Follows foobar2000's own dark mode setting (no-op on pre-2.0 builds).
        fb2k::CCoreDarkModeHooks darkHooks;
    };

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
            if (state->scope.can_create())
                state->scope.initialize(dlg);

            // Dark mode for the dialog and all of its controls.
            state->darkHooks.AddDialogWithControls(dlg);

            SetFocus(GetDlgItem(dlg, kIdUrlEdit));
            return FALSE; // focus set manually
        }

        case WM_COMMAND:
        {
            const int id = LOWORD(wp);

            if (id == IDCANCEL)
            {
                EndDialog(dlg, IDCANCEL);
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

                state->url = url;
                state->name = trim_wide(read_dialog_text(dlg, kIdNameEdit));
                EndDialog(dlg, IDOK);
                return TRUE;
            }

            break;
        }

        default:
            break;
        }

        return FALSE;
    }

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

    bool prompt_for_station(HWND owner, std::wstring& url, std::wstring& name)
    {
        constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;

        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 260, 96, L"Add internet radio url and create utag", L"Segoe UI", 9);
        t.item(kStatic, SS_LEFT, 8, 8, 244, 10, 0xFFFF, L"Station URL:");
        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 20, 244, 13,
            kIdUrlEdit, L"");
        t.item(kStatic, SS_LEFT, 8, 40, 244, 10, 0xFFFF, L"Station name (optional, looked up from the stream if empty):");
        t.item(kEdit, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 52, 244, 13,
            kIdNameEdit, L"");
        t.item(kStatic, SS_LEFT, 8, 77, 136, 10, kIdStatus, L"");
        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 74, 50, 14, IDOK, L"Create");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 202, 74, 50, 14, IDCANCEL, L"Cancel");

        url_prompt_state state;
        const INT_PTR result = DialogBoxIndirectParamW(
            core_api::get_my_instance(), t.data(), owner,
            url_prompt_proc, reinterpret_cast<LPARAM>(&state));

        if (result != IDOK)
            return false;

        url = state.url;
        name = state.name;
        return true;
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

    // Connects to the stream just long enough to read its response headers
    // and pulls the station name out of "icy-name" / "ice-name".
    class station_probe_task : public threaded_process_callback
    {
    public:
        station_probe_task(const std::string& url,
            std::shared_ptr<station_probe_result> out)
            : m_url(url), m_out(std::move(out)) {
        }

        void run(threaded_process_status&, abort_callback& abort) override
        {
            try
            {
                static_api_ptr_t<http_client> client;
                auto request = client->create_request("GET");
                request->add_header("Icy-MetaData: 1");

                // run_ex() hands back the stream as a file object; the HTTP
                // response headers are exposed through its http_reply service.
                auto stream = request->run_ex(m_url.c_str(), abort);

                http_reply::ptr reply;
                if (stream->service_query_t(reply))
                {
                    static const char* const headers[] =
                    { "icy-name", "ice-name", "Icy-Name", "Ice-Name" };

                    for (const char* header : headers)
                    {
                        pfc::string8 value;
                        reply->get_http_header(header, value);

                        const std::string text = trim_ascii(value.get_ptr());
                        if (!text.empty())
                        {
                            m_out->name = text;
                            break;
                        }
                    }
                }

                m_out->finished = true;
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

    private:
        void fail(const char* what)
        {
            m_out->error = (what != nullptr && *what != '\0')
                ? what
                : "no response from the server";
            m_out->finished = true;
        }

        static std::string trim_ascii(const char* text)
        {
            std::string result = text != nullptr ? text : "";
            const char* ws = " \t\r\n";
            const size_t first = result.find_first_not_of(ws);
            if (first == std::string::npos)
                return {};
            const size_t last = result.find_last_not_of(ws);
            return result.substr(first, last - first + 1);
        }

        std::string m_url;
        std::shared_ptr<station_probe_result> m_out;
    };

    // Looks the station up on the stream. `error` is set when the server is
    // unreachable; `finished` stays false when the user aborts.
    station_probe_result probe_station_name(HWND owner, const std::string& url)
    {
        const auto result = std::make_shared<station_probe_result>();
        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<station_probe_task>(url, result);

        threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort | threaded_process::flag_show_delayed,
            owner,
            "Looking up station name...");

        return *result;
    }
}

// -----------------------------------------------------------------------------
// Main menu command: File > Import stations from Radio Browser...
// Asks for a few filters, downloads matching stations from the Radio Browser
// directory (radio-browser.info) and writes one .utag file per station into the
// default utag folder.
// -----------------------------------------------------------------------------

// {53CE13D0-118F-4D5B-B106-E4845184780C}
static const GUID g_import_radio_browser_guid =
{ 0x53ce13d0, 0x118f, 0x4d5b, { 0xb1, 0x06, 0xe4, 0x84, 0x51, 0x84, 0x78, 0x0c } };

namespace
{
    constexpr int kIdRbName = 1101;
    constexpr int kIdRbTag = 1102;
    constexpr int kIdRbCountry = 1103;
    constexpr int kIdRbBitrate = 1104;
    constexpr int kIdRbLimit = 1105;
    constexpr int kIdRbStatus = 1106;

    constexpr size_t kRbMaxDetailedEntries = 60; // results popup stays readable

    struct radio_query
    {
        std::string name;
        std::string tag;
        std::string countryCode;
        long minBitrate = 0;
        long limit = 100;
    };

    struct radio_prompt_state
    {
        radio_query query;
        modal_dialog_scope scope;
        fb2k::CCoreDarkModeHooks darkHooks;
    };

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
            if (state->scope.can_create())
                state->scope.initialize(dlg);

            state->darkHooks.AddDialogWithControls(dlg);

            SetFocus(GetDlgItem(dlg, kIdRbName));
            return FALSE; // focus set manually
        }

        case WM_COMMAND:
        {
            const int id = LOWORD(wp);

            if (id == IDCANCEL)
            {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }

            if (id == IDOK && state != nullptr)
            {
                const std::wstring country = trim_wide(read_dialog_text(dlg, kIdRbCountry));
                if (!country.empty() &&
                    (country.size() != 2 || !is_ascii_letter(country[0]) ||
                        !is_ascii_letter(country[1])))
                {
                    SetDlgItemTextW(dlg, kIdRbStatus,
                        L"Country code must be two letters, e.g. DE.");
                    SetFocus(GetDlgItem(dlg, kIdRbCountry));
                    return TRUE;
                }

                long minBitrate = 0;
                const std::wstring bitrateText = trim_wide(read_dialog_text(dlg, kIdRbBitrate));
                if (!bitrateText.empty() && !parse_whole_number(bitrateText, minBitrate))
                {
                    SetDlgItemTextW(dlg, kIdRbStatus,
                        L"Minimum bitrate must be a whole number.");
                    SetFocus(GetDlgItem(dlg, kIdRbBitrate));
                    return TRUE;
                }

                long limit = 100;
                const std::wstring limitText = trim_wide(read_dialog_text(dlg, kIdRbLimit));
                if (!limitText.empty() &&
                    (!parse_whole_number(limitText, limit) || limit < 1 || limit > 5000))
                {
                    SetDlgItemTextW(dlg, kIdRbStatus,
                        L"Maximum stations must be between 1 and 5000.");
                    SetFocus(GetDlgItem(dlg, kIdRbLimit));
                    return TRUE;
                }

                state->query.name = wide_to_utf8(trim_wide(read_dialog_text(dlg, kIdRbName)));
                state->query.tag = wide_to_utf8(trim_wide(read_dialog_text(dlg, kIdRbTag)));
                state->query.countryCode = wide_to_utf8(country);
                state->query.minBitrate = minBitrate;
                state->query.limit = limit;
                EndDialog(dlg, IDOK);
                return TRUE;
            }

            break;
        }

        default:
            break;
        }

        return FALSE;
    }

    bool prompt_for_radio_query(HWND owner, radio_query& query)
    {
        constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;
        constexpr DWORD kEditStyle = ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP;

        dialog_template t;
        t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
            0, 0, 260, 142, L"Import stations from Radio Browser", L"Segoe UI", 9);

        t.item(kStatic, SS_LEFT, 8, 12, 100, 10, 0xFFFF, L"Name contains:");
        t.item(kEdit, kEditStyle, 112, 10, 140, 13, kIdRbName, L"");
        t.item(kStatic, SS_LEFT, 8, 30, 100, 10, 0xFFFF, L"Tag / genre:");
        t.item(kEdit, kEditStyle, 112, 28, 140, 13, kIdRbTag, L"");
        t.item(kStatic, SS_LEFT, 8, 48, 100, 10, 0xFFFF, L"Country code (e.g. DE):");
        t.item(kEdit, kEditStyle, 112, 46, 140, 13, kIdRbCountry, L"");
        t.item(kStatic, SS_LEFT, 8, 66, 100, 10, 0xFFFF, L"Min. bitrate (kbps):");
        t.item(kEdit, kEditStyle | ES_NUMBER, 112, 64, 140, 13, kIdRbBitrate, L"");
        t.item(kStatic, SS_LEFT, 8, 84, 100, 10, 0xFFFF, L"Maximum stations:");
        t.item(kEdit, kEditStyle | ES_NUMBER, 112, 82, 140, 13, kIdRbLimit, L"100");
        t.item(kStatic, SS_LEFT, 8, 103, 244, 10, kIdRbStatus, L"");
        t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 120, 50, 14, IDOK, L"Import");
        t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 202, 120, 50, 14, IDCANCEL, L"Cancel");

        radio_prompt_state state;
        const INT_PTR result = DialogBoxIndirectParamW(
            core_api::get_my_instance(), t.data(), owner,
            radio_prompt_proc, reinterpret_cast<LPARAM>(&state));

        if (result != IDOK)
            return false;

        query = state.query;
        return true;
    }

    std::string percent_encode(const std::string& text)
    {
        static const char* const hex = "0123456789ABCDEF";
        std::string out;
        for (const unsigned char c : text)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                out += static_cast<char>(c);
            }
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0x0F];
            }
        }
        return out;
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

    // Downloads the station list, trying the Radio Browser mirrors in turn.
    class radio_fetch_task : public threaded_process_callback
    {
    public:
        radio_fetch_task(const std::string& path, std::shared_ptr<radio_fetch_result> out)
            : m_path(path), m_out(std::move(out)) {
        }

        void run(threaded_process_status&, abort_callback& abort) override
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

                    const std::string url = std::string(server) + m_path;
                    auto stream = request->run_ex(url.c_str(), abort);

                    constexpr size_t kMaxResponse = static_cast<size_t>(64) * 1024 * 1024;

                    std::string body;
                    std::vector<char> buffer(16384); // heap, not stack
                    for (;;)
                    {
                        abort.check();
                        const t_size got = stream->read(buffer.data(), buffer.size(), abort);
                        if (got == 0)
                            break;

                        body.append(buffer.data(), static_cast<size_t>(got));
                        if (body.size() > kMaxResponse)
                            throw std::runtime_error("The response is too large.");
                    }

                    if (body.empty())
                    {
                        lastError = "empty response";
                        continue;
                    }

                    m_out->body = std::move(body);
                    m_out->finished = true;
                    return;
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

            m_out->error = lastError.empty() ? "no response from the server" : lastError;
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
        if (q.minBitrate > 0)
            text += " - " + std::to_string(q.minBitrate) + "+ kbps";

        text += " - top " + std::to_string(q.limit);
        return text;
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

    void import_from_radio_browser(HWND owner)
    {
        static const char* const kTitle = "Import stations from Radio Browser";

        radio_query query;
        if (!prompt_for_radio_query(owner, query))
            return;

        std::wstring parentFolder;
        if (!get_default_folder(parentFolder, false))
            return;

        // Download the station list (background task with an Abort button).
        const auto fetched = std::make_shared<radio_fetch_result>();
        service_ptr_t<threaded_process_callback> task =
            new service_impl_t<radio_fetch_task>(build_radio_browser_path(query), fetched);

        threaded_process::g_run_modal(
            task,
            threaded_process::flag_show_abort,
            owner,
            "Fetching stations from Radio Browser...");

        if (!fetched->error.empty())
        {
            const std::string message =
                "Could not fetch stations from Radio Browser:\r\n" + fetched->error;
            popup_message::g_show(message.c_str(), kTitle, popup_message::icon_error);
            return;
        }

        if (!fetched->finished)
        {
            popup_message::g_show("Import cancelled; no files created.", kTitle,
                popup_message::icon_information);
            return;
        }

        const json stations = json::parse(fetched->body, nullptr, false);
        if (stations.is_discarded() || !stations.is_array())
        {
            popup_message::g_show(
                "Radio Browser returned an unexpected response; no files created.",
                kTitle, popup_message::icon_error);
            return;
        }

        if (stations.empty())
        {
            popup_message::g_show("No stations matched the search; no folder created.",
                kTitle, popup_message::icon_information);
            return;
        }

        // Every search gets its own folder, named after the search.
        std::wstring folder;
        DWORD folderError = 0;
        if (!create_search_folder(parentFolder, describe_radio_query(query), folder, folderError))
        {
            std::ostringstream message;
            message << "Could not create a folder in " << wide_to_utf8(parentFolder)
                << " (Windows error " << folderError << ").";
            popup_message::g_show(message.str().c_str(), kTitle, popup_message::icon_error);
            return;
        }

        // Stations that repeat a stream URL within this search are skipped.
        std::set<std::string> knownUrls;

        std::vector<std::string> logEntries;
        std::vector<std::string> createdPaths; // for the playlist
        size_t loggedCount = 0;
        unsigned created = 0;
        unsigned skipped = 0;
        unsigned failed = 0;

        // Only the first entries are listed in detail; counts cover them all.
        const auto addLog = [&](std::string text)
            {
                if (loggedCount++ < kRbMaxDetailedEntries)
                    logEntries.emplace_back(std::move(text));
            };

        for (const auto& station : stations)
        {
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
                    ++skipped;
                    addLog("[SKIPPED] " + (label.empty() ? std::string("(no name)") : label) +
                        "\n          No usable name or http(s) stream URL.");
                    continue;
                }

                if (!knownUrls.insert(url).second)
                {
                    ++skipped;
                    addLog("[SKIPPED] " + label +
                        "\n          @  : " + url +
                        "\n          -> Another station in this search already uses this URL.");
                    continue;
                }

                const std::string contents =
                    json::array({ radio_station_to_utag(station, label, url) })
                    .dump(4, ' ', false, json::error_handler_t::replace) + "\n";

                if (write_unique_utag(folder, make_safe_filename_base(label),
                    contents, outputPath))
                {
                    ++created;
                    createdPaths.push_back(outputPath);
                    addLog("[CREATED] " + label +
                        "\n          @  : " + url +
                        "\n          -> " + outputPath);
                }
                else
                {
                    ++skipped;
                    addLog("[SKIPPED] " + label +
                        "\n          @  : " + url +
                        "\n          -> Tried 9999 filename variants; no available filename.");
                }
            }
            catch (const std::exception& e)
            {
                ++failed;
                std::string entry = "[ERROR] " + (label.empty() ? url : label);
                entry += "\n        Source: " + url;
                if (!outputPath.empty()) entry += "\n        Output: " + outputPath;
                entry += "\n        Reason: ";
                entry += e.what();
                addLog(std::move(entry));
            }
        }

        // Put the new files into a playlist named like the search folder.
        std::string playlistNote;
        if (!createdPaths.empty())
        {
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
                playlists->set_active_playlist(index);
                playlists->playlist_add_locations(index, locations, true, owner);

                playlistNote = "Playlist: " + playlistName + " (" +
                    std::to_string(createdPaths.size()) + " entries)";
            }
            catch (const std::exception& e)
            {
                playlistNote = std::string("Could not create the playlist: ") + e.what();
            }
        }

        std::ostringstream results;
        results << "Import stations from Radio Browser - Results"
            << "\r\n\r\n" << "Output folder: " << wide_to_utf8(folder)
            << "\r\n" << "Stations returned: " << stations.size();

        if (!playlistNote.empty())
            results << "\r\n" << playlistNote;

        results << "\r\n\r\n" << std::string(64, '-');

        for (const auto& entry : logEntries)
            results << "\r\n\r\n" << entry;

        if (loggedCount > kRbMaxDetailedEntries)
        {
            results << "\r\n\r\n... and " << (loggedCount - kRbMaxDetailedEntries)
                << " more (details are shown for the first "
                << kRbMaxDetailedEntries << " stations).";
        }

        results << "\r\n\r\n" << std::string(64, '-')
            << "\r\n\r\n" << "Summary: Created " << created << " | Skipped " << skipped
            << " | Failed " << failed;

        const std::string resultsText = results.str();
        popup_message::g_show(resultsText.c_str(), kTitle, popup_message::icon_information);
    }
}

class add_url_utag_mainmenu : public mainmenu_commands
{
public:
    enum { cmd_add_url = 0, cmd_import_radio_browser, cmd_count };

    t_uint32 get_command_count() override
    {
        return cmd_count;
    }

    GUID get_command(t_uint32 index) override
    {
        return index == cmd_import_radio_browser
            ? g_import_radio_browser_guid
            : g_add_url_utag_guid;
    }

    void get_name(t_uint32 index, pfc::string_base& out) override
    {
        if (index == cmd_import_radio_browser)
            out = "Import stations from Radio Browser...";
        else
            out = "Add internet radio url and create utag";
    }

    bool get_description(t_uint32 index, pfc::string_base& out) override
    {
        if (index == cmd_import_radio_browser)
            out = "Download stations from the Radio Browser directory and create .utag files in the default utag folder.";
        else
            out = "Enter a stream URL and create a .utag file for it in the default utag folder.";
        return true;
    }

    GUID get_parent() override
    {
        // "file_add" is the File menu block with Add files / Add folder / Add location.
        return UTAG_MAINMENU_GROUP;
    }

    // Sort after the built-in items of the group (i.e. after "Add location...").
    t_uint32 get_sort_priority() override
    {
        return sort_priority_last;
    }

    void execute(t_uint32 index, service_ptr_t<service_base>) override
    {
        HWND owner = core_api::get_main_window();

        if (index == cmd_import_radio_browser)
        {
            import_from_radio_browser(owner);
            return;
        }

        std::wstring wideUrl;
        std::wstring wideName;
        if (!prompt_for_station(owner, wideUrl, wideName))
            return;

        std::wstring folder;
        if (!get_default_folder(folder, false))
            return;

        std::vector<std::string> logEntries;
        logEntries.emplace_back("Add internet radio url and create utag - Results");
        logEntries.emplace_back("Output folder: " + wide_to_utf8(folder));
        logEntries.emplace_back(std::string(64, '-'));

        std::string url;
        std::string label;
        std::string outputPath;
        bool probed = false;
        unsigned created = 0;
        unsigned skipped = 0;
        unsigned failed = 0;

        try
        {
            url = wide_to_utf8(wideUrl);
            std::string name = wide_to_utf8(wideName);

            // No name typed in: ask the stream for it. An unreachable server
            // (or an abort) stops here and nothing is created.
            if (name.empty() && !is_utag_path(url))
            {
                const station_probe_result probe = probe_station_name(owner, url);

                if (!probe.error.empty())
                    throw std::runtime_error("Could not reach the stream: " + probe.error);
                if (!probe.finished)
                    throw std::runtime_error("Station lookup was cancelled; no file created.");

                name = probe.name;
                probed = !name.empty();
            }

            label = name.empty() ? station_label_from_url(url) : name;

            if (is_utag_path(url))
            {
                ++skipped;
                logEntries.emplace_back(
                    "[SKIPPED] That is a .utag file, not a stream URL: " + url);
            }
            else
            {
                json station = json::object();
                station["name"] = label;
                station["@"] = url;
                const std::string contents = json::array({ station }).dump(4) + "\n";

                if (write_unique_utag(folder, make_safe_filename_base(label),
                    contents, outputPath))
                {
                    ++created;
                    logEntries.emplace_back("[CREATED] " + label +
                        (probed ? " (name from stream)" : "") +
                        "\n          @  : " + url +
                        "\n          -> " + outputPath);

                    try
                    {
                        static_api_ptr_t<playlist_manager> playlists;
                        const std::string playlistName = "New utags";

                        // Make canonical path for the created file.
                        pfc::string8 canonicalPath;
                        filesystem::g_get_canonical_path(outputPath.c_str(), canonicalPath);

                        pfc::list_t<const char*> locations;
                        locations.add_item(canonicalPath.get_ptr());

                        // find_or_create_playlist returns index or pfc_infinite on failure.
                        const t_size plIndex = playlists->find_or_create_playlist(playlistName.c_str(), playlistName.length());
                        if (plIndex != pfc_infinite)
                        {
                            playlists->playlist_add_locations(plIndex, locations, true, owner);
                            playlists->set_active_playlist(plIndex);
                        }
                        else
                        {
                            // Fallback: try create_playlist if find_or_create failed.
                            const t_size createdIndex = playlists->create_playlist(playlistName.c_str(), playlistName.length(), pfc_infinite);
                            if (createdIndex != pfc_infinite)
                            {
                                playlists->playlist_add_locations(createdIndex, locations, true, owner);
                                playlists->set_active_playlist(createdIndex);
                            }
                        }
                    }
                    catch (...)
                    {
                        // Don't treat playlist failures as fatal for utag creation.
                    }
                }
                else
                {
                    ++skipped;
                    logEntries.emplace_back("[SKIPPED] " + label +
                        "\n          @  : " + url +
                        "\n          -> Tried 9999 filename variants; no available filename.");
                }
            }
        }
        catch (const std::exception& e)
        {
            ++failed;
            std::string entry = "[ERROR] " + (label.empty() ? url : label);
            entry += "\n        Source: " + url;
            if (!outputPath.empty()) entry += "\n        Output: " + outputPath;
            entry += "\n        Reason: ";
            entry += e.what();
            logEntries.emplace_back(std::move(entry));
        }
        catch (...)
        {
            ++failed;
            std::string entry = "[ERROR] " + (label.empty() ? url : label);
            entry += "\n        Source: " + url + "\n        Reason: Unknown error.";
            if (!outputPath.empty()) entry += "\n        Output: " + outputPath;
            logEntries.emplace_back(std::move(entry));
        }

        logEntries.emplace_back(std::string(64, '-'));
        std::ostringstream summary;
        summary << "Summary: Created " << created << " | Skipped " << skipped
            << " | Failed " << failed;
        logEntries.emplace_back(summary.str());

        std::ostringstream results;
        for (size_t i = 0; i < logEntries.size(); ++i)
        {
            if (i != 0) results << "\r\n\r\n";
            results << logEntries[i];
        }

        const std::string resultsText = results.str();
        popup_message::g_show(
            resultsText.c_str(),
            "Add internet radio url and create utag",
            popup_message::icon_information);
    }
};

static mainmenu_commands_factory_t<add_url_utag_mainmenu>
g_add_url_utag_mainmenu_factory;

static contextmenu_item_factory_t<create_utag_context_menu>
g_create_utag_context_menu_factory;

// Extracts the saved favicon as front-cover artwork when available. Favicon
// downloading is asynchronous, so an initial artwork query may happen before the
// sidecar exists; later queries can use it. If it is not present, preserve the
// referenced-file fallback. The built-in viewer may cache artwork and may need
// its own refresh/track-change before it asks again.
// True for plain paths and file:// references. Remote streams (http, https,
// etc.) have no length and no embedded artwork, so artwork readers must not
// be pointed at them (doing so throws "Length of object is unknown").
static bool utag_is_local_reference(const std::string& ref)
{
    if (ref.empty()) return false;
    if (ref.rfind("file://", 0) == 0) return true;
    return ref.find("://") == std::string::npos;
}

class utag_album_art_extractor_instance : public album_art_extractor_instance
{
public:
    utag_album_art_extractor_instance(std::string imagePath, std::string reference)
        : m_imagePath(std::move(imagePath)), m_reference(std::move(reference)) {
    }

    album_art_data_ptr query(const GUID& what, abort_callback& abort) override
    {
        abort.check();
        if (what == album_art_ids::cover_front && !m_imagePath.empty())
        {
            try
            {
                service_ptr_t<file> imageFile;
                filesystem::g_open_read(imageFile, m_imagePath.c_str(), abort);
                const t_filesize size = imageFile->get_size_ex(abort);
                if (size > 0 && size <= 64 * 1024 * 1024)
                    return album_art_data_impl::g_create(
                        imageFile.get_ptr(), pfc::downcast_guarded<t_size>(size), abort);
            }
            catch (const exception_io&)
            {
                // Fall back to the referenced media file if the sidecar is absent/unreadable.
            }
        }

        // Only local references can hold embedded artwork.
        if (!utag_is_local_reference(m_reference))
            throw exception_album_art_not_found();

        try
        {
            auto fallback = album_art_extractor::g_open(nullptr, m_reference.c_str(), abort);
            return fallback->query(what, abort);
        }
        catch (const exception_album_art_not_found&)
        {
            throw;
        }
        catch (const exception_io&)
        {
            // Unreadable/unsized referenced item: report "no artwork" rather
            // than an unexpected error in the artwork viewer.
            throw exception_album_art_not_found();
        }
    }

private:
    std::string m_imagePath;
    std::string m_reference;
};

class utag_album_art_extractor : public album_art_extractor_v2
{
public:
    bool is_our_path(const char* path, const char* extension) override
    {
        (void)path;
        return extension != nullptr && pfc::stricmp_ascii(extension, "utag") == 0;
    }

    album_art_extractor_instance_ptr open(
        file_ptr fileHint, const char* path, abort_callback& abort) override
    {
        (void)fileHint;
        if (path == nullptr || *path == '\0') throw exception_io_data();

        service_ptr_t<file> file;
        filesystem::g_open_read(file, path, abort);
        const t_filesize size = file->get_size_ex(abort);
        if (size == 0 || size > kMaxStubSize) throw exception_io_data();

        std::string buffer(static_cast<size_t>(size), '\0');
        file->seek(0, abort);
        file->read_object(buffer.data(), static_cast<t_size>(buffer.size()), abort);
        file.release();

        if (buffer.size() >= 3 &&
            static_cast<unsigned char>(buffer[0]) == 0xEF &&
            static_cast<unsigned char>(buffer[1]) == 0xBB &&
            static_cast<unsigned char>(buffer[2]) == 0xBF)
            buffer.erase(0, 3);

        json parsed = json::parse(buffer, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array() ||
            parsed.size() != 1 || !parsed[0].is_object())
            throw exception_io_data();

        const auto refIt = parsed[0].find("@");
        if (refIt == parsed[0].end() || !refIt->is_string() ||
            refIt->get<std::string>().empty())
            throw exception_album_art_not_found();

        const std::string reference = to_native_windows_path(refIt->get<std::string>());
        std::string imagePath;
        const auto faviconIt = parsed[0].find("favicon");
        if (faviconIt != parsed[0].end() && faviconIt->is_string())
        {
            const std::string url = faviconIt->get<std::string>();
            if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
            {
                std::string extension = ".ico";
                const size_t query = url.find_first_of("?#");
                const std::string urlPath = url.substr(0, query);
                const size_t dot = urlPath.find_last_of('.');
                const size_t slash = urlPath.find_last_of('/');
                if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                {
                    const std::string candidate = urlPath.substr(dot);
                    if (candidate.size() <= 6) extension = candidate;
                }

                std::string nativePath = to_native_windows_path(path);
                const size_t pathSlash = nativePath.find_last_of("\\/");
                const size_t pathDot = nativePath.find_last_of('.');
                if (pathDot != std::string::npos &&
                    (pathSlash == std::string::npos || pathDot > pathSlash))
                {
                    imagePath = nativePath.substr(0, pathDot) + extension;
                    const std::wstring wideImage = utf8_to_wide(imagePath);
                    const DWORD attrs = GetFileAttributesW(wideImage.c_str());
                    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
                        imagePath.clear();
                }
            }
        }

        abort.check();
        return new service_impl_t<utag_album_art_extractor_instance>(
            std::move(imagePath), reference);
    }

    GUID get_guid() override { return g_utag_guid; }
};

static service_factory_single_t<utag_album_art_extractor>
g_utag_album_art_extractor_factory;

// Exposes the artwork editor for the audio file referenced by a .utag file.
// The artwork is edited in the referenced audio file, not in the .utag JSON.
class utag_album_art_editor : public album_art_editor_v2
{
public:
    bool is_our_path(
        const char* path,
        const char* extension) override
    {
        (void)path;

        return extension != nullptr
            && pfc::stricmp_ascii(extension, "utag") == 0;
    }

    album_art_editor_instance_ptr open(
        file_ptr fileHint,
        const char* path,
        abort_callback& abort) override
    {
        (void)fileHint;

        if (path == nullptr || *path == '\0') {
            throw exception_io_data();
        }

        // Read the .utag JSON file.
        service_ptr_t<file> input;
        filesystem::g_open_read(input, path, abort);

        const t_filesize fileSize = input->get_size_ex(abort);

        if (fileSize == 0 || fileSize > kMaxStubSize) {
            throw exception_io_data();
        }

        std::string buffer(
            static_cast<size_t>(fileSize), '\0');

        input->seek(0, abort);
        input->read_object(
            buffer.data(),
            static_cast<t_size>(buffer.size()),
            abort);

        input.release();

        // Accept a UTF-8 BOM if present.
        if (buffer.size() >= 3
            && static_cast<unsigned char>(buffer[0]) == 0xEF
            && static_cast<unsigned char>(buffer[1]) == 0xBB
            && static_cast<unsigned char>(buffer[2]) == 0xBF)
        {
            buffer.erase(0, 3);
        }

        // Validate the existing .utag structure:
        // [ { "@": "file://..." } ]
        json parsed = json::parse(buffer, nullptr, false);

        if (parsed.is_discarded()
            || !parsed.is_array()
            || parsed.size() != 1
            || !parsed[0].is_object())
        {
            throw exception_io_data();
        }

        const auto it = parsed[0].find("@");

        if (it == parsed[0].end()
            || !it->is_string()
            || it->get_ref<const std::string&>().empty())
        {
            throw exception_io_data();
        }

        // Resolve the referenced audio file.
        const std::string reference =
            to_native_windows_path(
                it->get_ref<const std::string&>());

        abort.check();

        // Remote streams have no length and cannot hold editable artwork.
        if (!utag_is_local_reference(reference)) {
            throw exception_io_data();
        }

        // Delegate to the registered editor for the actual audio format.
        // This may throw if that format has no artwork editor.
        try {
            return album_art_editor::g_open(
                nullptr, reference.c_str(), abort);
        }
        catch (const exception_io&) {
            throw exception_io_data();
        }
    }

    GUID get_guid() override
    {
        return g_utag_guid;
    }
};

static service_factory_single_t<utag_album_art_editor>
g_utag_album_art_editor_factory;