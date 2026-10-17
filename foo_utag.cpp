#include <SDK/foobar2000.h>
#include <nlohmann/json.hpp>

#include <Windows.h>
#include <shlobj.h>
#include <initializer_list>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <cmath>
#include <utility>
#include <algorithm>
#include <cctype>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

using json = nlohmann::json;

DECLARE_COMPONENT_VERSION(
    "utag Input",
    "1.0.0",
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
    "- Playlist context menu: Utilities > Create utag files...\n"
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
        // Park the running decoder so a new instance for the
        // same URL can continue the stream instead of reconnecting.
        try
        {
            if (m_initialized &&
                !m_aborted &&
                m_decoder.is_valid() &&
                m_reason == input_open_decode &&
                handoff_enabled())
            {
                handoff_store(m_url, m_decoder);
            }
        }
        catch (...)
        {
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

        // Read metadata from the .utag JSON object.
        for (auto it = m_json.begin(); it != m_json.end(); ++it)
        {
            const std::string& key = it.key();
            const json& value = it.value();

            if (is_reserved_key(key) || value.is_null())
                continue;

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

        // Technical info (codec, bitrate, ...) only during playback, taken from
        // the decoder that playback uses anyway. Library scans / Properties
        // (input_open_info_read) never touch the network.
        if (m_reason == input_open_decode)
        {
            try
            {
                ensure_decoder(abort);

                file_info_impl decoderInfo;
                m_decoder->get_info(0, decoderInfo, abort);

                // copy_info() replaces the whole info section.
                info.copy_info(decoderInfo);
            }
            catch (const exception_io&)
            {
            }
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
        console::printf(
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

        // Reuse the running decoder of the instance that was
        // just closed (e.g. because the playing item was retagged).
        if (allowHandoff &&
            m_reason == input_open_decode &&
            handoff_enabled() &&
            handoff_take(m_url, m_decoder))
        {
            m_handedOff = true;
            m_reused = true;
            console::printf("[foo_utag] reusing running stream: @ = %s", m_url.c_str());
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
            console::printf("[foo_utag] open_for_decoding failed: %s", e.what());
            throw;
        }

        if (!m_decoder.is_valid())
            throw exception_io_data();
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
            return;
        }

        // Forward the caller's flags (playback, no_seeking, no_looping, ...)
        m_decoder->initialize(0, flags, abort);
        m_initialized = true;
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
                console::printf(
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

            if (is_reserved_key(key))
                continue;

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

        commit_json(std::move(updated), abort);
    }

    void remove_tags(abort_callback& abort)
    {
        if (m_path.empty())
            throw exception_tagging_unsupported();

        json updated = m_json;

        for (auto it = updated.begin(); it != updated.end();)
        {
            if (is_standard_tag_key(it.key()))
                it = updated.erase(it);
            else
                ++it;
        }

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

class create_utag_context_menu : public contextmenu_item_simple
{
public:
    unsigned get_num_items() override
    {
        return 1;
    }

    void get_item_name(unsigned, pfc::string_base& out) override
    {
        out = "Create utag files...";
    }

    GUID get_item_guid(unsigned) override
    {
        return g_create_utag_guid;
    }

    GUID get_parent() override
    {
        return contextmenu_groups::tagging;
    }

    bool get_item_description(unsigned, pfc::string_base& out) override
    {
        out = "Create one .utag station file for each selected playlist track.";
        return true;
    }

    void context_command(
        unsigned,
        metadb_handle_list_cref data,
        const GUID&) override
    {
        if (data.get_count() == 0)
            return;

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

        unsigned created = 0;
        unsigned skipped = 0;

        for (t_size i = 0; i < data.get_count(); ++i)
        {
            const char* path = data[i]->get_path();
            if (path == nullptr || *path == '\0')
            {
                ++skipped;
                continue;
            }

            const std::string url(path);

            // A .utag path points to a metadata file, not necessarily the stream.
            if (is_utag_path(url))
            {
                ++skipped;
                continue;
            }

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
                        { "station", "station name", "stationname",
                          "icy-name", "radio", "name" });

                    trackTitle = find_track_metadata(trackInfo, { "title" });
                }
                catch (...)
                {
                    // Some entries may not expose metadata; use URL fallback.
                }


                std::string filenameLabel = stationName;
                if (filenameLabel.empty())
                    filenameLabel = trackTitle;
                if (filenameLabel.empty())
                    filenameLabel = basename_from_path(url);
                if (filenameLabel.empty())
                    filenameLabel = "Station";

                const std::wstring safeName =
                    make_safe_filename_base(filenameLabel);

                json station = json::object();
                station["name"] = stationName.empty()
                    ? filenameLabel
                    : stationName;

                if (!trackTitle.empty())
                    station["title"] = trackTitle;

                // Copy any other metadata fields and preserve multi-values.
                for (t_size m = 0; m < trackInfo.meta_get_count(); ++m)
                {
                    const char* metaName = trackInfo.meta_enum_name(m);
                    if (metaName == nullptr || *metaName == '\0')
                        continue;

                    const std::string key(metaName);
                    const std::string lower = lowercase_ascii(key);

                    if (is_reserved_key(key) || lower == "title")
                        continue;

                    const t_size count = trackInfo.meta_enum_value_count(m);
                    if (count == 0)
                        continue;

                    json values = json::array();

                    for (t_size v = 0; v < count; ++v)
                    {
                        const char* value = trackInfo.meta_enum_value(m, v);
                        if (value != nullptr)
                            values.push_back(value);
                    }

                    if (values.size() == 1)
                        station[key] = values[0];
                    else if (!values.empty())
                        station[key] = std::move(values);
                }

                station["@"] = url;

                const std::string contents =
                    json::array({ station }).dump(4) + "\n";

                bool saved = false;

                // CREATE_NEW prevents overwriting. Add a suffix for duplicates.
                for (unsigned suffix = 1; suffix <= 9999; ++suffix)
                {
                    std::wstring filename = safeName;

                    if (suffix > 1)
                        filename += L" (" + std::to_wstring(suffix) + L")";

                    filename += L".utag";

                    std::wstring fullPath = folder;
                    if (!fullPath.empty() &&
                        fullPath.back() != L'\\' &&
                        fullPath.back() != L'/')
                    {
                        fullPath += L'\\';
                    }
                    fullPath += filename;

                    HANDLE file = CreateFileW(
                        fullPath.c_str(),
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr);

                    if (file == INVALID_HANDLE_VALUE)
                    {
                        const DWORD error = GetLastError();

                        if (error == ERROR_FILE_EXISTS ||
                            error == ERROR_ALREADY_EXISTS)
                        {
                            continue;
                        }

                        throw exception_io_data();
                    }

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

                    saved = true;
                    ++created;
                    break;
                }

                if (!saved)
                    ++skipped;
            }
            catch (...)
            {
                ++skipped;
            }
        }

        char summary[256] = {};
        if (skipped == 0)
        {
            sprintf_s(summary, "Created %u utag file(s).", created);
        }
        else
        {
            sprintf_s(
                summary,
                "Created %u utag file(s).\nSkipped %u track(s) or file(s).",
                created,
                skipped);
        }

        popup_message::g_show(
            summary,
            "Create utag files",
            popup_message::icon_information);
    }
};

static contextmenu_item_factory_t<create_utag_context_menu>
g_create_utag_context_menu_factory;