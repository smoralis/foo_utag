#include "utag_ui.h"
#include "utag_radio.h"

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

// Last folder used by "Create utag files..." (persisted in foobar2000's config).
// {7B3F9C1E-2D54-4A8B-9E61-5C0D84F2A3B7}
static const GUID g_guid_cfg_last_dir =
{ 0x7b3f9c1e, 0x2d54, 0x4a8b, { 0x9e, 0x61, 0x5c, 0x0d, 0x84, 0xf2, 0xa3, 0xb7 } };

static cfg_string g_cfg_last_dir(g_guid_cfg_last_dir, "");

// Menu item: create utag files in the default folder, no prompt.
// {6D0AFA80-DC5A-4B66-AD50-545251FDA004}
static const GUID g_create_utag_auto_guid =
{ 0x6d0afa80, 0xdc5a, 0x4b66, { 0xad, 0x50, 0x54, 0x52, 0x51, 0xfd, 0xa0, 0x04 } };

// Menu item: choose the default utag folder.
// {545B0497-BF41-4F62-A79B-7DFE5A40E421}
static const GUID g_set_utag_folder_guid =
{ 0x545b0497, 0xbf41, 0x4f62, { 0xa7, 0x9b, 0x7d, 0xfe, 0x5a, 0x40, 0xe4, 0x21 } };

namespace
{

// One selected playlist entry, captured on the main thread so the worker thread
// never touches metadb handles.
struct create_item
{
    std::string url;
    std::string filenameLabel;
    json station = json::object(); // built from the entry's own tags
};

// Creates the .utag files in the background so foobar2000 stays usable while
// Radio Browser is asked about each URL. The playlist and the results popup are
// handled in on_done(), which runs on the main thread.
class create_utag_task : public threaded_process_callback
{
public:
    create_utag_task(std::vector<create_item> items, std::wstring folder,
        bool radioBrowserLookup, std::vector<std::string> preLog,
        unsigned preSkipped, unsigned preFailed)
        : m_items(std::move(items)), m_folder(std::move(folder)),
        m_rbLookup(radioBrowserLookup), m_logEntries(std::move(preLog)),
        m_skipped(preSkipped), m_failed(preFailed) {
    }

    void run(threaded_process_status& status, abort_callback& abort) override
    {
        const size_t total = m_items.size();

        for (size_t i = 0; i < total; ++i)
        {
            try
            {
                abort.check();
                status.set_progress(i, total);
                process_item(m_items[i], status, abort);
            }
            catch (const exception_aborted&)
            {
                // Keep what was already created; on_done() still adds it to
                // the playlist and reports the outcome.
                m_cancelled = true;
                break;
            }
            catch (const std::exception& e)
            {
                ++m_failed;
                std::string entry = "[ERROR] ";
                entry += m_items[i].filenameLabel.empty() ? m_items[i].url : m_items[i].filenameLabel;
                entry += "\n        Source: " + m_items[i].url;
                if (!m_outputPath.empty()) entry += "\n        Output: " + m_outputPath;
                entry += "\n        Reason: ";
                entry += e.what();
                m_logEntries.emplace_back(std::move(entry));
            }
            catch (...)
            {
                ++m_failed;
                std::string entry = "[ERROR] ";
                entry += m_items[i].filenameLabel.empty() ? m_items[i].url : m_items[i].filenameLabel;
                entry += "\n        Source: " + m_items[i].url + "\n        Reason: Unknown error.";
                if (!m_outputPath.empty()) entry += "\n        Output: " + m_outputPath;
                m_logEntries.emplace_back(std::move(entry));
            }
        }
    }

    void on_done(HWND, bool) override
    {
        // Send successfully created files to the dedicated playlist and activate it.
        if (!m_createdPaths.empty())
        {
            const std::string playlistName = "New utags";
            try
            {
                static_api_ptr_t<playlist_manager> playlists;
                const HWND owner = core_api::get_main_window();
                pfc::list_t<const char*> locations;
                std::vector<pfc::string8> canonicalPaths;
                canonicalPaths.reserve(m_createdPaths.size());

                for (const auto& path : m_createdPaths)
                {
                    canonicalPaths.emplace_back();
                    filesystem::g_get_canonical_path(path.c_str(), canonicalPaths.back());
                }
                for (const auto& path : canonicalPaths)
                    locations.add_item(path.get_ptr());

                t_size index = playlists->find_or_create_playlist(
                    playlistName.c_str(), playlistName.length());
                if (index == pfc_infinite)
                    index = playlists->create_playlist(
                        playlistName.c_str(), playlistName.length(), pfc_infinite);
                if (index == pfc_infinite)
                    throw std::runtime_error("the playlist could not be created");

                playlists->set_active_playlist(index);
                playlists->playlist_add_locations(index, locations, true, owner);
                m_logEntries.emplace_back("Added " + std::to_string(m_createdPaths.size()) +
                    " created file(s) to the \"" + playlistName + "\" playlist and made it active.");
            }
            catch (const std::exception& e)
            {
                m_logEntries.emplace_back(std::string("Could not add created files to the \"") +
                    playlistName + "\" playlist: " + e.what());
            }
            catch (...)
            {
                m_logEntries.emplace_back("Could not add created files to the \"" +
                    playlistName + "\" playlist: unknown error.");
            }
        }

        if (m_cancelled)
            m_logEntries.emplace_back("Cancelled: the remaining tracks were not processed.");

        m_logEntries.emplace_back(std::string(64, '-'));
        std::ostringstream summary;
        summary << "Summary: Created " << m_created << " | Skipped " << m_skipped
            << " | Failed " << m_failed;
        m_logEntries.emplace_back(summary.str());

        std::ostringstream results;
        for (size_t i = 0; i < m_logEntries.size(); ++i)
        {
            if (i != 0) results << "\r\n\r\n";
            results << m_logEntries[i];
        }
        // Show the per-entry log in foobar2000's existing popup message,
        // rather than opening a separate custom results window.
        report_result("Create utag files", results.str(), summary.str());
    }

private:
    // Worker thread: no UI, playlist or config access in here.
    void process_item(create_item& item, threaded_process_status& status,
        abort_callback& abort)
    {
        m_outputPath.clear();
        json station = item.station;
        bool fromRadioBrowser = false;

        // Same lookup as "Add internet radio url and create utag": ask Radio
        // Browser about this URL. If it knows the station, its details become
        // the .utag (tags from the playlist entry that Radio Browser does not
        // provide are kept); otherwise, or if the lookup fails, the entry's own
        // tags are used as before.
        if (m_rbLookup)
        {
            const std::string shown = item.filenameLabel.empty() ? item.url : item.filenameLabel;
            status.set_item(("Searching Radio Browser: " + shown).c_str());

            json found;
            std::string lookupError;
            if (radio_lookup_station_by_url(item.url, abort, found, lookupError))
            {
                for (auto it = station.begin(); it != station.end(); ++it)
                {
                    if (!found.contains(it.key()))
                        found[it.key()] = it.value();
                }
                station = std::move(found);
                fromRadioBrowser = true;

                if (station.contains("name") && station["name"].is_string())
                {
                    const std::string rbName = station["name"].get<std::string>();
                    if (!rbName.empty())
                        item.filenameLabel = rbName;
                }
            }
            else if (!lookupError.empty())
            {
                m_logEntries.emplace_back("[NOTE] Radio Browser lookup failed for " + item.url +
                    " (" + lookupError + "); using the track's own details instead.");
            }
            else
            {
                m_logEntries.emplace_back("[NOTE] Radio Browser has no station with the URL " +
                    item.url + "; using the track's own details instead.");
            }
        }

        const std::wstring safeName = make_safe_filename_base(item.filenameLabel);
        station["@"] = item.url;
        const std::string contents = json::array({ station }).dump(4, ' ', false,
            json::error_handler_t::replace) + "\n";

        if (write_unique_utag(m_folder, safeName, contents, m_outputPath))
        {
            m_createdPaths.emplace_back(m_outputPath);
            ++m_created;
            m_logEntries.emplace_back("[CREATED] " + item.filenameLabel +
                (fromRadioBrowser ? " (details from Radio Browser)" : "") +
                "\n          @  : " + item.url +
                "\n          -> " + m_outputPath);
        }
        else
        {
            ++m_skipped;
            m_logEntries.emplace_back("[SKIPPED] " + item.filenameLabel +
                "\n          @  : " + item.url +
                "\n          -> Tried 9999 filename variants; no available filename.");
        }
    }

    std::vector<create_item> m_items;
    std::wstring m_folder;
    bool m_rbLookup = false;
    bool m_cancelled = false;

    std::string m_outputPath;
    std::vector<std::string> m_logEntries;
    std::vector<std::string> m_createdPaths;
    unsigned m_created = 0;
    unsigned m_skipped = 0;
    unsigned m_failed = 0;
};

} // namespace

// Collects the selected tracks on the main thread, then hands them to a
// background task that does the Radio Browser lookups and writes the files.
static void create_utag_files_in_folder(
    metadb_handle_list_cref data,
    const std::wstring& folder)
{
    unsigned skipped = 0;
    unsigned failed = 0;
    std::vector<std::string> logEntries;
    std::vector<create_item> items;
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

        create_item item;
        item.url = url;
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

            item.filenameLabel = stationName;
            if (item.filenameLabel.empty()) item.filenameLabel = trackTitle;
            if (item.filenameLabel.empty()) item.filenameLabel = basename_from_path(url);
            if (item.filenameLabel.empty()) item.filenameLabel = "Station";

            json& station = item.station;
            station["name"] = stationName.empty() ? item.filenameLabel : stationName;
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

            items.emplace_back(std::move(item));
        }
        catch (const std::exception& e)
        {
            ++failed;
            logEntries.emplace_back("[ERROR] " + url + "\n        Reason: " + e.what());
        }
        catch (...)
        {
            ++failed;
            logEntries.emplace_back("[ERROR] " + url + "\n        Reason: Unknown error.");
        }
    }

    const bool lookup = g_cfg_ctx_rb_lookup;
    service_ptr_t<threaded_process_callback> task =
        new service_impl_t<create_utag_task>(std::move(items), folder, lookup,
            std::move(logEntries), skipped, failed);

    // Modeless: foobar2000 stays usable; the progress window only shows up
    // when the lookups take a moment.
    threaded_process::g_run_modeless(
        task,
        threaded_process::flag_show_abort | threaded_process::flag_show_progress |
        threaded_process::flag_show_delayed,
        core_api::get_main_window(),
        lookup ? "Searching Radio Browser and creating utag files..." : "Creating utag files...");
}

// -----------------------------------------------------------------------------
// "Search Radio Browser and tag": looks up the "@" stream URL of the selected
// .utag items in Radio Browser and writes the station details into their tags.
// The tags are written through foobar2000's normal tagging (metadb_io), so the
// utag input's retag() rewrites the .utag file exactly as a Properties edit would.
// -----------------------------------------------------------------------------

// Menu item: look up the selected .utag items in Radio Browser and tag them.
// {9A3C5E71-4B28-4D06-8F1D-62E7B0A9C34F}
static const GUID g_rb_tag_guid =
{ 0x9a3c5e71, 0x4b28, 0x4d06, { 0x8f, 0x1d, 0x62, 0xe7, 0xb0, 0xa9, 0xc3, 0x4f } };

namespace
{
    struct rb_tag_item
    {
        metadb_handle_ptr handle;
        std::string url;    // the "@" value
        std::string label;  // shown in the log
        json found;         // Radio Browser's station object (when have == true)
        bool have = false;
    };

    // Replaces the tags Radio Browser knows about; everything else is kept.
    void apply_station_to_info(file_info& info, const json& station)
    {
        for (auto it = station.begin(); it != station.end(); ++it)
        {
            const std::string& key = it.key();
            if (key.empty() || is_reserved_key(key)) continue;

            std::vector<std::string> values;
            if (it.value().is_array())
            {
                for (const auto& v : it.value())
                {
                    if (v.is_object() || v.is_array() || v.is_null()) continue;
                    const std::string text = json_value_to_string(v);
                    if (!text.empty()) values.push_back(text);
                }
            }
            else if (!it.value().is_object() && !it.value().is_null())
            {
                const std::string text = json_value_to_string(it.value());
                if (!text.empty()) values.push_back(text);
            }

            if (values.empty()) continue;

            info.meta_remove_field(key.c_str());
            for (const auto& value : values)
                info.meta_add(key.c_str(), value.c_str());
        }
    }

    class rb_tag_task : public threaded_process_callback
    {
    public:
        explicit rb_tag_task(std::vector<rb_tag_item> items, std::vector<std::string> preLog,
            unsigned preSkipped)
            : m_items(std::move(items)), m_log(std::move(preLog)), m_skipped(preSkipped) {
        }

        // Worker thread: network only, no metadb writes.
        void run(threaded_process_status& status, abort_callback& abort) override
        {
            const size_t total = m_items.size();
            for (size_t i = 0; i < total; ++i)
            {
                rb_tag_item& item = m_items[i];
                try
                {
                    abort.check();
                    status.set_progress(i, total);
                    status.set_item(("Searching Radio Browser: " + item.label).c_str());

                    std::string error;
                    if (radio_lookup_station_by_url(item.url, abort, item.found, error))
                    {
                        item.have = true;
                    }
                    else if (!error.empty())
                    {
                        ++m_failed;
                        m_log.emplace_back("[FAILED] " + item.label + "\n          @  : " + item.url +
                            "\n          Radio Browser lookup failed: " + error);
                    }
                    else
                    {
                        ++m_notFound;
                        m_log.emplace_back("[NOT FOUND] " + item.label + "\n          @  : " + item.url +
                            "\n          Radio Browser has no station with this URL; tags unchanged.");
                    }
                }
                catch (const exception_aborted&)
                {
                    m_cancelled = true;
                    break;
                }
                catch (const std::exception& e)
                {
                    ++m_failed;
                    m_log.emplace_back("[FAILED] " + item.label + "\n          @  : " + item.url +
                        "\n          Reason: " + e.what());
                }
                catch (...)
                {
                    ++m_failed;
                    m_log.emplace_back("[FAILED] " + item.label + "\n          @  : " + item.url +
                        "\n          Reason: Unknown error.");
                }
            }
        }

        // Main thread: build the new tags and hand them to foobar2000.
        void on_done(HWND, bool) override
        {
            metadb_handle_list handles;
            pfc::list_t<file_info_impl> infos;
            std::vector<size_t> tagged; // indices into m_items, in `handles` order

            for (size_t i = 0; i < m_items.size(); ++i)
            {
                rb_tag_item& item = m_items[i];
                if (!item.have) continue;

                file_info_impl info;
                if (!item.handle->get_info(info))
                {
                    ++m_failed;
                    m_log.emplace_back("[FAILED] " + item.label + "\n          @  : " + item.url +
                        "\n          The item's current tags are not loaded yet; try again in a moment.");
                    continue;
                }

                apply_station_to_info(info, item.found);
                handles.add_item(item.handle);
                infos.add_item(info);
                tagged.push_back(i);
            }

            if (handles.get_count() > 0)
            {
                try
                {
                    pfc::list_t<const file_info*> infoPtrs;
                    for (t_size i = 0; i < infos.get_count(); ++i)
                        infoPtrs.add_item(&infos[i]);

                    static_api_ptr_t<metadb_io_v2> io;
                    io->update_info_async_simple(handles, infoPtrs,
                        core_api::get_main_window(), metadb_io_v2::op_flag_delay_ui, nullptr);

                    for (size_t i : tagged)
                    {
                        const rb_tag_item& item = m_items[i];
                        std::string name;
                        if (item.found.contains("name") && item.found["name"].is_string())
                            name = item.found["name"].get<std::string>();
                        m_log.emplace_back("[TAGGED] " + item.label +
                            (name.empty() || name == item.label ? std::string() : " -> " + name) +
                            "\n          @  : " + item.url);
                        ++m_tagged;
                    }
                }
                catch (const std::exception& e)
                {
                    m_failed += static_cast<unsigned>(tagged.size());
                    m_log.emplace_back(std::string("[FAILED] Could not write the tags: ") + e.what());
                }
                catch (...)
                {
                    m_failed += static_cast<unsigned>(tagged.size());
                    m_log.emplace_back("[FAILED] Could not write the tags: unknown error.");
                }
            }

            if (m_cancelled)
                m_log.emplace_back("Cancelled: the remaining items were not looked up.");

            m_log.emplace_back(std::string(64, '-'));
            std::ostringstream summary;
            summary << "Summary: Tagged " << m_tagged << " | Not found " << m_notFound
                << " | Skipped " << m_skipped << " | Failed " << m_failed;
            m_log.emplace_back(summary.str());

            std::ostringstream results;
            for (size_t i = 0; i < m_log.size(); ++i)
            {
                if (i != 0) results << "\r\n\r\n";
                results << m_log[i];
            }
            report_result("Radio Browser tagging", results.str(), summary.str());
        }

    private:
        std::vector<rb_tag_item> m_items;
        std::vector<std::string> m_log;
        bool m_cancelled = false;
        unsigned m_tagged = 0;
        unsigned m_notFound = 0;
        unsigned m_skipped = 0;
        unsigned m_failed = 0;
    };
}

// Main thread: reads each selected .utag's "@" URL, then starts the lookups.
static void start_radio_browser_tagging(metadb_handle_list_cref data)
{
    std::vector<rb_tag_item> items;
    std::vector<std::string> log;
    unsigned skipped = 0;
    log.emplace_back("Radio Browser tagging - Results");
    log.emplace_back(std::string(64, '-'));

    for (t_size i = 0; i < data.get_count(); ++i)
    {
        const char* path = data[i]->get_path();
        if (path == nullptr || !is_utag_path(path))
            continue; // not a .utag: not part of this command

        rb_tag_item item;
        item.handle = data[i];

        file_info_impl info;
        if (data[i]->get_info(info))
        {
            const char* url = info.info_get("@");
            if (url != nullptr) item.url = url;
            item.label = find_track_metadata(info, { "name", "station", "title" });
        }
        if (item.label.empty()) item.label = basename_from_path(path);

        if (item.url.empty())
        {
            ++skipped;
            log.emplace_back("[SKIPPED] " + item.label + "\n          No \"@\" stream URL found for " + path);
            continue;
        }

        items.emplace_back(std::move(item));
    }

    if (items.empty())
    {
        popup_message::g_show("None of the selected items is a .utag file with a stream URL.",
            "Radio Browser tagging", popup_message::icon_information);
        return;
    }

    service_ptr_t<threaded_process_callback> task =
        new service_impl_t<rb_tag_task>(std::move(items), std::move(log), skipped);

    // Modeless: foobar2000 stays usable while Radio Browser is asked.
    threaded_process::g_run_modeless(
        task,
        threaded_process::flag_show_abort | threaded_process::flag_show_progress |
        threaded_process::flag_show_delayed,
        core_api::get_main_window(),
        "Searching Radio Browser...");
}

class create_utag_context_menu : public contextmenu_item_simple
{
public:
    enum { cmd_create = 0, cmd_create_auto, cmd_set_folder, cmd_rb_tag, cmd_count };

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
        case cmd_rb_tag:     out = "Search Radio Browser and tag"; break;
        }
    }

    GUID get_item_guid(unsigned index) override
    {
        switch (index)
        {
        case cmd_create_auto: return g_create_utag_auto_guid;
        case cmd_set_folder:  return g_set_utag_folder_guid;
        case cmd_rb_tag:      return g_rb_tag_guid;
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
        case cmd_rb_tag:
            out = "Look up the \"@\" URL of each selected .utag in Radio Browser and write the station details into its tags.";
            return true;
        }
        return false;
    }

    // The Radio Browser command only appears when a .utag item is selected.
    bool context_get_display(
        unsigned index,
        metadb_handle_list_cref data,
        pfc::string_base& out,
        unsigned& flags,
        const GUID& caller) override
    {
        if (index == cmd_rb_tag)
        {
            bool anyUtag = false;
            for (t_size i = 0; i < data.get_count() && !anyUtag; ++i)
            {
                const char* path = data[i]->get_path();
                anyUtag = path != nullptr && is_utag_path(path);
            }
            if (!anyUtag) return false;
        }
        return contextmenu_item_simple::context_get_display(index, data, out, flags, caller);
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

        if (index == cmd_rb_tag)
        {
            start_radio_browser_tagging(data);
            return;
        }

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

static contextmenu_item_factory_t<create_utag_context_menu>
g_create_utag_context_menu_factory;
