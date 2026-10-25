#include "utag_tunein.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <vector>

// -----------------------------------------------------------------------------
// TuneIn OPML API (the endpoints used by the footuner script):
//   Browse.ashx / Describe.ashx / Search.ashx / Recommend.ashx  lists
//   Tune.ashx                                                   station -> stream
// -----------------------------------------------------------------------------

namespace
{
    // Same host and partner id as the original script. Change the scheme here
    // if the service ever stops answering over plain http.
    constexpr const char* kTuneInHost = "http://opml.radiotime.com";

    constexpr size_t kTiMaxDetailedEntries = 60;
    constexpr size_t kTiMaxJsonBytes = 8 * 1024 * 1024;
    constexpr size_t kTiMaxPlaylistBytes = 256 * 1024;

    std::string ti_lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    bool ti_is_http(const std::string& url)
    {
        return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    }

    // TuneIn appends session parameters to stream URLs; like the script, keep
    // only the part before '?'.
    std::string ti_strip_query(const std::string& url)
    {
        return url.substr(0, url.find('?'));
    }

    // String value of a JSON field; numbers are converted, anything else is "".
    std::string ti_json_text(const json& object, const char* key)
    {
        const auto it = object.find(key);
        if (it == object.end())
            return {};

        if (it->is_string())
            return ti_trim(it->get<std::string>());

        if (it->is_number())
            return std::to_string(static_cast<long long>(it->get<double>()));

        return {};
    }

    std::string with_render(std::string url)
    {
        if (ti_lower(url).find("render=") == std::string::npos)
            url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("render=json");
        return url;
    }

    // ---- HTTP ----------------------------------------------------------------

    // Downloads `url`. Returns false with `error` set on failure; throws
    // exception_aborted when the user aborts. Safe to call from a worker thread.
    bool tunein_http_get(const std::string& url, abort_callback& abort,
        size_t maxBytes, std::string& body, std::string& error)
    {
        try
        {
            static_api_ptr_t<http_client> client;
            auto request = client->create_request("GET");
            auto stream = request->run_ex(url.c_str(), abort);

            std::string data;
            std::vector<char> buffer(16384);
            for (;;)
            {
                abort.check();
                const t_size got = stream->read(buffer.data(), buffer.size(), abort);
                if (got == 0)
                    break;

                data.append(buffer.data(), static_cast<size_t>(got));
                if (data.size() > maxBytes)
                {
                    error = "the response is too large";
                    return false;
                }
            }

            if (data.empty())
            {
                error = "empty response";
                return false;
            }

            body = std::move(data);
            return true;
        }
        catch (const exception_aborted&)
        {
            throw; // user pressed Abort
        }
        catch (const std::exception& e) // also covers pfc::exception
        {
            error = e.what();
            return false;
        }
        catch (...)
        {
            error = "no response from the server";
            return false;
        }
    }

    // ---- list parsing ----------------------------------------------------------

    void parse_level(const json& list, std::vector<tunein_entry>& out, int depth)
    {
        if (!list.is_array() || depth > 4)
            return;

        for (const auto& item : list)
        {
            if (!item.is_object())
                continue;

            const std::string text = ti_json_text(item, "text");

            // A section: heading + children.
            const auto children = item.find("children");
            if (children != item.end() && children->is_array())
            {
                if (!text.empty())
                {
                    tunein_entry header;
                    header.kind = tunein_entry::kind_t::header;
                    header.name = text;
                    header.label = "-- " + text + " --";
                    out.push_back(std::move(header));
                }
                parse_level(*children, out, depth + 1);
                continue;
            }

            if (text.empty())
                continue;

            tunein_entry e;
            e.name = text;
            e.url = ti_json_text(item, "URL");
            if (e.url.empty())
                e.url = ti_json_text(item, "url");
            e.image = ti_json_text(item, "image");
            e.guideId = ti_json_text(item, "guide_id");
            e.item = ti_lower(ti_json_text(item, "item"));

            const std::string lowerUrl = ti_lower(e.url);
            const bool tune = lowerUrl.find("tune.ashx") != std::string::npos;
            const bool programs = tune && lowerUrl.find("c=pbrowse") != std::string::npos;

            if (tune && !programs)
            {
                e.kind = tunein_entry::kind_t::station;
                e.label = text;

                if (!e.item.empty() && e.item != "station")
                    e.label += " [" + e.item + "]";

                const std::string reliability = ti_json_text(item, "reliability");
                if (!reliability.empty())
                    e.label += " [reliability: " + reliability + "%]";
            }
            else if (programs || lowerUrl.find("browse.ashx") != std::string::npos)
            {
                e.kind = tunein_entry::kind_t::link;
                e.url = with_render(e.url);
                e.label = "> " + text;
            }
            else
            {
                e.kind = tunein_entry::kind_t::note;
                e.label = text;
            }

            out.push_back(std::move(e));
        }
    }

    // ---- stream resolving ------------------------------------------------------

    // First http(s) stream URL of a .pls ("File1=...") or .m3u (plain lines).
    std::string first_stream_in_playlist(const std::string& text)
    {
        std::istringstream in(text);

        for (std::string line; std::getline(in, line);)
        {
            line = ti_trim(line);
            if (line.empty() || line[0] == '#')
                continue;

            if (ti_lower(line).rfind("file", 0) == 0)
            {
                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                line = ti_trim(line.substr(eq + 1));
            }

            if (ti_is_http(line))
                return line;
        }

        return {};
    }

    struct tunein_stream
    {
        std::string url;
        std::string codec;
        std::string bitrate;
    };

    void fill_tunein_stream(const json& entry, const std::string& url, tunein_stream& out)
    {
        out.url = ti_strip_query(url);
        out.codec = ti_json_text(entry, "media_type");

        const auto bitrate = entry.find("bitrate");
        if (bitrate != entry.end())
        {
            if (bitrate->is_number() && bitrate->get<double>() > 0)
                out.bitrate = std::to_string(static_cast<long long>(bitrate->get<double>()));
            else if (bitrate->is_string())
                out.bitrate = ti_trim(bitrate->get<std::string>());
        }
    }

    // Tune.ashx -> stream. A direct URL wins; otherwise the first pls/m3u
    // playlist that yields a stream is used.
    bool resolve_tunein_stream(const std::string& tuneUrl, abort_callback& abort,
        tunein_stream& out, std::string& error)
    {
        json response;
        if (!tunein_fetch_json(with_render(tuneUrl), abort, response, error))
            return false;

        const auto list = response.find("body");
        if (list == response.end() || !list->is_array())
        {
            error = "no stream list in the response";
            return false;
        }

        std::vector<const json*> playlists;

        for (const auto& entry : *list)
        {
            if (!entry.is_object())
                continue;

            const std::string streamUrl = ti_json_text(entry, "url");
            if (streamUrl.empty())
                continue;

            bool direct = false;
            const auto flag = entry.find("is_direct");
            if (flag != entry.end())
            {
                direct = (flag->is_boolean() && flag->get<bool>()) ||
                    (flag->is_string() && ti_lower(flag->get<std::string>()) == "true");
            }

            if (direct && ti_is_http(streamUrl))
            {
                fill_tunein_stream(entry, streamUrl, out);
                return true;
            }

            const std::string type = ti_lower(ti_json_text(entry, "playlist_type"));
            if (type == "pls" || type == "m3u")
                playlists.push_back(&entry);
        }

        for (const json* entry : playlists)
        {
            abort.check();

            std::string text;
            std::string playlistError;
            if (!tunein_http_get(ti_json_text(*entry, "url"), abort,
                kTiMaxPlaylistBytes, text, playlistError))
            {
                error = playlistError;
                continue;
            }

            const std::string stream = first_stream_in_playlist(text);
            if (!stream.empty())
            {
                fill_tunein_stream(*entry, stream, out);
                return true;
            }

            error = "the playlist contained no stream URL";
        }

        if (error.empty())
            error = "no playable stream returned";
        return false;
    }

    // ---- background import ---------------------------------------------------------

    std::string describe_search(const tunein_search_request& q)
    {
        const char* kind = q.kind == tunein_kind::artist ? "artist"
            : q.kind == tunein_kind::song ? "song"
            : "station";

        return std::string("TuneIn - ") + kind + " - " + q.text +
            " - top " + std::to_string(q.limit);
    }

    class tunein_import_task : public threaded_process_callback
    {
    public:
        // Import of an explicit list (browser window).
        tunein_import_task(std::string description,
            std::vector<tunein_candidate> candidates, std::wstring parentFolder)
            : m_description(std::move(description)),
            m_candidates(std::move(candidates)),
            m_parent(std::move(parentFolder)),
            m_details(g_cfg_tunein_details) {
            m_browseWindow = true; // browser window: service playlist
        }

        // Search first, then import the stations found (search window).
        tunein_import_task(const tunein_search_request& search, std::wstring parentFolder)
            : m_hasSearch(true), m_search(search),
            m_description(describe_search(search)),
            m_parent(std::move(parentFolder)),
            m_details(g_cfg_tunein_details) {
        }

        void run(threaded_process_status& status, abort_callback& abort) override
        {
            try
            {
                do_import(status, abort);
            }
            catch (const exception_aborted&)
            {
                throw; // reported through on_done(aborted = true)
            }
            catch (const std::exception& e)
            {
                m_error = std::string("Import failed: ") + e.what();
            }
            catch (...)
            {
                m_error = "Import failed: unexpected error.";
            }
        }

        void on_done(HWND, bool aborted) override
        {
            static const char* const kTitle = "Import stations from TuneIn";
            const HWND owner = core_api::get_main_window();

            if (aborted)
            {
                std::string text = "Import cancelled; no files created.";
                if (m_created > 0)
                {
                    text = "Import cancelled; " + std::to_string(m_created) +
                        " file(s) were already created in:\r\n" + wide_to_utf8(m_folder);
                }
                report_result(kTitle, text, text);
                return;
            }

            if (!m_error.empty())
            {
                popup_message::g_show(m_error.c_str(), kTitle, popup_message::icon_error);
                return;
            }

            if (!m_info.empty())
            {
                report_result(kTitle, m_info, m_info);
                return;
            }

            // Browser window: append to the TuneIn playlist (set in preferences).
            // Search import: new playlist named like the search folder.
            const std::string playlistNote = m_browseWindow
                ? add_to_service_playlist(utag_service::tunein, m_createdPaths, owner)
                : add_to_new_folder_playlist(m_folder, m_createdPaths, owner);

            std::ostringstream results;
            results << "Import stations from TuneIn - Results"
                << "\r\n\r\n" << "Output folder: " << wide_to_utf8(m_folder)
                << "\r\n" << "Stations found: " << m_candidates.size();

            if (!playlistNote.empty())
                results << "\r\n" << playlistNote;

            results << "\r\n\r\n" << std::string(64, '-');

            for (const auto& entry : m_logEntries)
                results << "\r\n\r\n" << entry;

            if (m_loggedCount > kTiMaxDetailedEntries)
            {
                results << "\r\n\r\n... and " << (m_loggedCount - kTiMaxDetailedEntries)
                    << " more (details are shown for the first "
                    << kTiMaxDetailedEntries << " stations).";
            }

            results << "\r\n\r\n" << std::string(64, '-')
                << "\r\n\r\n" << "Summary: Created " << m_created << " | Skipped " << m_skipped;

            std::ostringstream shortSummary;
            shortSummary << "Created " << m_created << " | Skipped " << m_skipped << " - " << wide_to_utf8(m_folder);

            const std::string resultsText = results.str();
            report_result(kTitle, resultsText, shortSummary.str());
        }

    private:
        // Worker thread: no UI, playlist or config access in here.
        void do_import(threaded_process_status& status, abort_callback& abort)
        {
            if (m_hasSearch && !search_stations(status, abort))
                return;

            if (m_candidates.empty())
            {
                m_info = "No stations to import; no folder created.";
                return;
            }

            DWORD folderError = 0;
            if (!create_search_folder(m_parent, m_description, m_folder, folderError))
            {
                std::ostringstream message;
                message << "Could not create a folder in " << wide_to_utf8(m_parent)
                    << " (Windows error " << folderError << ").";
                m_error = message.str();
                return;
            }

            const auto addLog = [this](std::string text)
                {
                    if (m_loggedCount++ < kTiMaxDetailedEntries)
                        m_logEntries.emplace_back(std::move(text));
                };

            size_t done = 0;

            for (const auto& c : m_candidates)
            {
                abort.check();
                status.set_item(c.name.c_str());
                status.set_progress(done++, m_candidates.size());

                std::string outputPath;

                try
                {
                    tunein_stream stream;
                    std::string resolveError;
                    if (!resolve_tunein_stream(c.tuneUrl, abort, stream, resolveError))
                    {
                        ++m_skipped;
                        addLog("[SKIPPED] " + c.name +
                            "\n          -> Could not resolve a stream: " + resolveError);
                        continue;
                    }

                    json entry = json::object();
                    entry["@"] = stream.url;
                    entry["name"] = c.name;
                    entry["title"] = c.name;
                    if (!c.guideId.empty())
                        entry["stream_tunein_id"] = c.guideId;
                    if (ti_is_http(c.image))
                        entry["favicon"] = c.image;
                    if (!stream.codec.empty())
                        entry["codec"] = stream.codec;
                    if (!stream.bitrate.empty())
                        entry["bitrate"] = stream.bitrate;

                    std::string detailsNote;
                    if (m_details)
                        add_details(c, abort, entry, detailsNote);

                    const std::string contents =
                        json::array({ entry }).dump(4, ' ', false, json::error_handler_t::replace) + "\n";

                    if (write_unique_utag(m_folder, make_safe_filename_base(c.name),
                        contents, outputPath))
                    {
                        ++m_created;
                        m_createdPaths.push_back(outputPath);
                        addLog("[CREATED] " + c.name +
                            "\n          @  : " + stream.url +
                            "\n          -> " + outputPath + detailsNote);
                    }
                    else
                    {
                        ++m_skipped;
                        addLog("[SKIPPED] " + c.name +
                            "\n          -> Tried 9999 filename variants; no available filename.");
                    }
                }
                catch (const exception_aborted&)
                {
                    throw;
                }
                catch (const std::exception& e)
                {
                    ++m_skipped;
                    std::string text = "[ERROR] " + c.name;
                    if (!outputPath.empty()) text += "\n        Output: " + outputPath;
                    text += "\n        Reason: ";
                    text += e.what();
                    addLog(std::move(text));
                }
            }
        }

        // Runs the search and fills m_candidates. Returns false when the import
        // should stop (m_error or m_info is set).
        bool search_stations(threaded_process_status& status, abort_callback& abort)
        {
            status.set_item("Searching TuneIn...");

            json response;
            std::string error;
            if (!tunein_fetch_json(tunein_search_url(m_search.kind, m_search.text),
                abort, response, error))
            {
                m_error = "Could not search TuneIn:\r\n" + error;
                return false;
            }

            std::vector<tunein_entry> entries;
            const auto list = response.find("body");
            if (list != response.end())
                tunein_parse_entries(*list, entries);

            std::set<std::string> knownIds;

            for (const auto& e : entries)
            {
                if (m_candidates.size() >= static_cast<size_t>(m_search.limit))
                    break;

                if (e.kind != tunein_entry::kind_t::station)
                    continue;
                if (!e.item.empty() && e.item != "station")
                    continue;
                if (!e.guideId.empty() && !knownIds.insert(e.guideId).second)
                    continue;

                m_candidates.push_back({ e.name, e.url, e.image, e.guideId });
            }

            if (m_candidates.empty())
            {
                m_info = "No stations matched the search; no folder created.";
                return false;
            }

            return true;
        }

        // Station details from Describe.ashx: location, description, genre,
        // TuneIn url and region id. A failure is not fatal for the station: the
        // file is still created, and `note` says why details are missing.
        void add_details(const tunein_candidate& c, abort_callback& abort,
            json& entry, std::string& note)
        {
            if (c.guideId.empty())
            {
                note = "\n          (details unavailable: no TuneIn id)";
                return;
            }

            json response;
            std::string error;
            if (!tunein_fetch_json(
                tunein_url("Describe.ashx?id=" + percent_encode(c.guideId) +
                    "&partnerId=1&render=json"),
                abort, response, error))
            {
                note = "\n          (details unavailable: " + error + ")";
                return;
            }

            const auto list = response.find("body");
            if (list == response.end() || !list->is_array())
            {
                note = "\n          (details unavailable: unexpected response)";
                return;
            }

            for (const auto& item : *list)
            {
                if (!item.is_object())
                    continue;

                // The fields are normally on the item itself; accept a nested "station" too.
                const json* source = &item;
                const auto nested = item.find("station");
                if (nested != item.end() && nested->is_object())
                    source = &(*nested);

                bool any = false;
                const auto set = [&](const char* tag, const char* key)
                    {
                        const std::string value = ti_json_text(*source, key);
                        if (!value.empty())
                        {
                            entry[tag] = value;
                            any = true;
                        }
                    };

                set("location", "location");
                set("description", "description");
                set("genre", "genre_name");
                set("tunein_url", "url");
                set("country_region_id", "country_region_id");

                if (any)
                    return;
            }

            note = "\n          (details unavailable: no station details in the response)";
        }

        bool m_hasSearch = false;
        tunein_search_request m_search;
        std::string m_description;
        std::vector<tunein_candidate> m_candidates;
        std::wstring m_parent;
        bool m_details = false; // look up Describe.ashx details (read on the main thread)

        std::wstring m_folder;
        std::string m_error;
        std::string m_info;
        std::vector<std::string> m_logEntries;
        std::vector<std::string> m_createdPaths; // for the playlist
        bool m_browseWindow = false;
        size_t m_loggedCount = 0;
        unsigned m_created = 0;
        unsigned m_skipped = 0;
    };

    void run_import_task(service_ptr_t<threaded_process_callback> task)
    {
        // Modeless: foobar2000 stays usable while stations are resolved.
        threaded_process::g_run_modeless(
            task,
            threaded_process::flag_show_abort | threaded_process::flag_show_progress,
            core_api::get_main_window(),
            "Importing stations from TuneIn...");
    }
}

// ---- public API -------------------------------------------------------------------

std::string ti_trim(const std::string& text)
{
    const char* ws = " \t\r\n";
    const size_t first = text.find_first_not_of(ws);
    if (first == std::string::npos)
        return {};
    const size_t last = text.find_last_not_of(ws);
    return text.substr(first, last - first + 1);
}

std::string tunein_url(const std::string& pathAndQuery)
{
    return std::string(kTuneInHost) + "/" + pathAndQuery;
}

std::string tunein_search_url(tunein_kind kind, const std::string& text)
{
    const std::string encoded = percent_encode(text);

    switch (kind)
    {
    case tunein_kind::artist:
        return tunein_url("Search.ashx?c=artist&query=" + encoded + "&partnerId=1&render=json");
    case tunein_kind::song:
        return tunein_url("Search.ashx?c=song&query=" + encoded + "&partnerId=1&render=json");
    default:
        return tunein_url("Search.ashx?query=" + encoded + "&types=station&partnerId=1&render=json");
    }
}

bool tunein_fetch_json(const std::string& url, abort_callback& abort,
    json& out, std::string& error)
{
    std::string body;
    if (!tunein_http_get(url, abort, kTiMaxJsonBytes, body, error))
        return false;

    json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
    {
        error = "unexpected response";
        return false;
    }

    out = std::move(parsed);
    return true;
}

std::string tunein_head_title(const json& response)
{
    const auto head = response.find("head");
    if (head == response.end() || !head->is_object())
        return {};
    return ti_json_text(*head, "title");
}

void tunein_parse_entries(const json& body, std::vector<tunein_entry>& out)
{
    parse_level(body, out, 0);
}

// Describe.ashx?c=genres: flat list of {text, guide_id}; each opens Browse.ashx?id=.
void tunein_parse_genres(const json& body, std::vector<tunein_entry>& out)
{
    if (!body.is_array())
        return;

    for (const auto& item : body)
    {
        if (!item.is_object())
            continue;

        const std::string text = ti_json_text(item, "text");
        const std::string guide = ti_json_text(item, "guide_id");
        if (text.empty() || guide.empty())
            continue;

        tunein_entry e;
        e.kind = tunein_entry::kind_t::link;
        e.name = text;
        e.guideId = guide;
        e.url = tunein_url("Browse.ashx?id=" + percent_encode(guide) + "&render=json");
        e.label = "> " + text;
        out.push_back(std::move(e));
    }
}

void tunein_start_import(const std::string& description,
    std::vector<tunein_candidate> candidates, const std::wstring& parentFolder)
{
    run_import_task(new service_impl_t<tunein_import_task>(
        description, std::move(candidates), parentFolder));
}

void tunein_start_search_import(const tunein_search_request& request,
    const std::wstring& parentFolder)
{
    run_import_task(new service_impl_t<tunein_import_task>(request, parentFolder));
}
