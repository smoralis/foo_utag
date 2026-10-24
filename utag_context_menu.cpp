#include "utag_ui.h"

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
    std::vector<std::string> createdPaths;
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
            const std::string contents = json::array({ station }).dump(4, ' ', false, json::error_handler_t::replace) + "\n";
            const bool saved = write_unique_utag(folder, safeName, contents, outputPath);
            if (saved)
            {
                createdPaths.emplace_back(outputPath);
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

    // Send successfully created files to the dedicated playlist and activate it.
    if (!createdPaths.empty())
    {
        const std::string playlistName = "New utags";
        try
        {
            static_api_ptr_t<playlist_manager> playlists;
            const HWND owner = core_api::get_main_window();
            pfc::list_t<const char*> locations;
            std::vector<pfc::string8> canonicalPaths;
            canonicalPaths.reserve(createdPaths.size());

            for (const auto& path : createdPaths)
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
            logEntries.emplace_back("Added " + std::to_string(createdPaths.size()) +
                " created file(s) to the \"" + playlistName + "\" playlist and made it active.");
        }
        catch (const std::exception& e)
        {
            logEntries.emplace_back(std::string("Could not add created files to the \"") +
                playlistName + "\" playlist: " + e.what());
        }
        catch (...)
        {
            logEntries.emplace_back("Could not add created files to the \"" +
                playlistName + "\" playlist: unknown error.");
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
    report_result("Create utag files", resultsText, summary.str());
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

static contextmenu_item_factory_t<create_utag_context_menu>
g_create_utag_context_menu_factory;
