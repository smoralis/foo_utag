#include "utag_common.h"

using namespace utag_state;

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

        return _stricmp(value.c_str() + value.size() - 5, ".utag") == 0;
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

        // A stub that points at another .utag would recurse forever.
        if (reference_is_utag(m_url))
            throw exception_io_data();

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
        erase_tag_fields(updated);

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

        erase_tag_fields(updated);

        updated["@"] = reservedUrl;

        commit_json(std::move(updated), abort);
    }

private:
    // True when get_info() exposes this JSON field as a metadata tag.
    static bool is_exposed_tag(const std::string& key, const json& value)
    {
        if (is_reserved_key(key) || value.is_null())
            return false;

        if (value.is_array())
        {
            for (const auto& item : value)
            {
                if (!item.is_null() && !item.is_object() && !item.is_array())
                    return true;
            }
            return false;
        }

        return !value.is_object();
    }

    // Removes every field that Properties can show and edit, so that fields
    // the user cleared or renamed are really removed from the file. Only "@"
    // and nested (non-tag) data are kept.
    static void erase_tag_fields(json& station)
    {
        for (auto it = station.begin(); it != station.end();)
        {
            if (!is_reserved_key(it.key()) &&
                (is_standard_tag_key(it.key()) ||
                    is_exposed_tag(it.key(), it.value())))
            {
                it = station.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

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

    // Writes to "<file>.tmp" and swaps it in, so a failure or abort halfway
    // can never leave a truncated .utag behind. Returns false when this is
    // not possible (the caller then writes in place).
    bool write_json_atomic(const std::string& contents, abort_callback& abort)
    {
        if (m_path.rfind("file://", 0) != 0)
            return false;

        std::wstring target;
        try
        {
            target = utf8_to_wide(m_path.substr(7));
        }
        catch (...)
        {
            return false;
        }

        abort.check();

        const std::wstring temp = target + L".tmp";
        const HANDLE h = CreateFileW(
            temp.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (h == INVALID_HANDLE_VALUE)
            return false;

        DWORD written = 0;
        const BOOL ok = WriteFile(
            h, contents.data(), static_cast<DWORD>(contents.size()),
            &written, nullptr);
        CloseHandle(h);

        if (!ok || written != contents.size() ||
            !MoveFileExW(temp.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temp.c_str());
            return false;
        }

        return true;
    }

    // -------------------------------------------------------------------------
    // Serialize the station object as a one-element JSON array.
    // -------------------------------------------------------------------------

    void write_json(const json& value, abort_callback& abort)
    {
        abort.check();

        const std::string contents =
            json::array({ value }).dump(4, ' ', false, json::error_handler_t::replace) + "\n";

        if (write_json_atomic(contents, abort))
            return;

        // Fallback: overwrite the existing .utag in place.
        service_ptr_t<file> output;

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
