# utag Input for foobar2000

`utag Input` is a foobar2000 input component that lets you add internet radio streams and other remote URLs to the Media Library by representing each stream with a small `.utag` JSON file.

The `.utag` file stores the stream URL and metadata. foobar2000 can scan and tag the stub like a normal track, while audio is decoded from the original URL.

## Features

- **Library-friendly scanning:** scanning a `.utag` file does not connect to the stream.
- **Editable metadata:** tag changes are written back to the `.utag` JSON file.
- **Stream URL in Properties:** the URL is exposed in the information section as `@` and can be displayed with `%__@%`.
- **Create files from a playlist:** use **Tagging → Create utag files...** in the playlist context menu to create one `.utag` file for each selected track. Each URL is looked up in Radio Browser first (optional, on by default), so the file gets the station's details. The new file(s) are added to the "New utags" playlist, which becomes the active playlist.
- **Tag existing `.utag` files from Radio Browser:** **Tagging → Search Radio Browser and tag** looks up the `@` URL of the selected `.utag` items in Radio Browser and writes the station details into their tags.
- **Default folder:** **Tagging → Set default utag folder...** saves an output folder, and **Tagging → Create utag files in default folder** creates the files there without asking.
- **Add a station by URL:** **File → Utag Input → Add internet radio url and create utag** asks for a stream URL (and an optional name) and creates the `.utag` file in the default folder. If no name is entered, it is looked up from the stream. An optional checkbox searches Radio Browser for the URL and uses its station details. The new file is added to the "New utags" playlist, which becomes the active playlist.
- **Import from Radio Browser:** **File → Utag Input → Import stations from Radio Browser...** downloads stations from the [Radio Browser](https://www.radio-browser.info/) directory and creates one `.utag` file per station, in a new folder named after the search.
- **Browse Radio Browser:** **File → Utag Input → Browse Radio Browser...** lists the top stations, or the stations for a tag, country, language or codec you pick, or a name search. Choose the stations you want and import them.
- **Import from Icecast:** **File → Utag Input → Import stations from Icecast...** lets you filter the directory by station metadata and choose a maximum number of matching stations to import.
- **Browse Icecast Directory:** **File → Utag Input → Browse Icecast Directory...** downloads the public Icecast Yellow Pages XML export (once per session; **Refresh** reloads it) and creates `.utag` files for the stations you select. A search box at the top of the window filters the list as you type (station name, genre, codec, description or current song). The downloaded directory is not saved by the component.
- **Import from TuneIn:** **File → Utag Input → Import stations from TuneIn...** searches TuneIn by station name, artist or song and creates one `.utag` file per station found.
- **Browse TuneIn:** **File → Utag Input → Browse TuneIn...** walks TuneIn's categories, genres, locations and languages, your presets, searches and recommendations (for a list of artists, or for the playing station). Choose the stations you want and import them.
- **Playlists for the browse windows:** the three browse windows (Radio Browser, TuneIn, Icecast) append the stations you import to one playlist per service, `RadioBrowser`, `Tunein` and `Icecast` by default (names are set in the preferences). The playlist is created when missing and becomes the active one, and the first appended item gets the focus. The **Import stations from...** commands instead create a new playlist named like the search folder (search term and date).
- **Country flag:** whenever a `.utag` is created or tagged and it has a `countrycode` field (for example `GR`), a `countryflag` field with the matching flag emoji (for example 🇬🇷) is added. The flag is also refreshed whenever the tags are saved (for example after editing `countrycode` in Properties), so it always follows the code; an invalid or empty code leaves the flag as it is. When a file is created, an existing non-empty `countryflag` is kept. This applies to Radio Browser and Icecast imports, TuneIn imports, **Add internet radio url and create utag**, **Create utag files** and **Search Radio Browser and tag**.																																																																																																																																																													  
- **Results window:** every created or skipped entry lists the stream URL (`@`) and the output path; errors list the source URL and the reason. The window can be switched off in the preferences (errors are still shown).
- **Preferences page:** **Preferences → Tools → Utag Input** has the stream handoff, favicon download, default folder, console message, results window, Radio Browser lookup, TuneIn details and browse-window playlist options.
- **Optional stream handoff:** reuses the running decoder when the playing item is reopened after retagging, helping avoid a reconnect and audible pause.
- **Station logos:** assign a logo by hand (**Tagging → Set station logo...**), by dropping an image file or image URL on the logo window, or with the optional favicon download. All of them are the same sidecar image beside the `.utag` file, shown as artwork. See [Station logos](#station-logos).																																																																										  
- **Optional favicon download during playback:** downloads the favicon image stored in the `.utag` JSON, saved beside the `.utag` file and shown as artwork.

## `.utag` file format

A `.utag` file must contain a JSON array with exactly one object. The object must include a non-empty string property named `@`, containing the stream URL.

Example:

```json
[
    {
        "@": "https://example.com/radio-stream",
        "name": "Example Radio",
        "title": "Example Radio",
        "genre": "Electronic",
        "description": "An example internet radio station"
    }
]
```

The example URL is a placeholder; replace it with the actual URL of a stream.

### Fields

- `@` — required stream URL used for playback. This is reserved by the component and is not treated as an ordinary metadata tag.
- `name` — optional station display name. If present and non-empty, it is preferred for the component's display name.
- `display_name` — fallback display name when `name` is absent or empty.
- `title` — optional title metadata. If no title is supplied, the component uses the display name as the title.
- Other scalar fields — exposed as metadata values.
- Array fields — arrays of scalar values can represent multiple metadata values. Nulls, nested objects, and nested arrays are ignored when reading metadata.

The file is UTF-8 JSON. A UTF-8 BOM is accepted. Files must be non-empty and no larger than 4 MiB.

## Installation

1. Obtain or build the component DLL, named `foo_utag.dll`.
2. Install it using the method appropriate for your foobar2000 setup.
3. Restart foobar2000 if required by your installation method.

This repository source declares the component name as **utag Input** and validates the component filename as `foo_utag.dll`.
Releases contain both 32-bit and 64-bit dll.

## Adding a station to the Media Library

1. Create a `.utag` file containing a one-element JSON array as shown above.
2. Set `@` to the station's actual stream URL.
3. Add the `.utag` file to foobar2000's Media Library or a monitored library folder.
4. Play the entry. The component opens the URL and lets foobar2000's input decoder handle the stream.

Library scans use the metadata-only open path and do not intentionally connect to the stream. Technical information is obtained from the decoder during playback.

## Importing stations from Radio Browser

1. Choose **File → Utag Input → Import stations from Radio Browser...**. It sits in the same block of the File menu as the add-by-URL command.
2. Fill in any filters; all of them are optional:
   - **Name contains** and **Tag / genre**
   - **Country code**: two letters, such as `DE`
   - **Codec**, such as `MP3` or `AAC+`
   - **Min. bitrate (kbps)**
   - **Maximum stations**: 1 to 5000, default 100

   The **Show all...** buttons next to the tag, country and codec fields open a list with a filter box and fill in the field for you.
3. Press **Import**. If no default folder is saved yet, you are asked to choose one first.

The window does not block foobar2000, and the import itself runs in the background with a progress window and an **Abort** button.

Every import goes into its own new sub folder of the default folder, named after the search and the date and time, for example `Radio Browser - jazz - DE - 128+ kbps - top 100 - 2026-10-18 14-32-05` (tag, name, country code and minimum bitrate appear only when you filled them in). Earlier imports are therefore never mixed with new ones. No folder is created when nothing is found or the download fails.

The most-clicked working stations that match are downloaded (broken stations are left out), with an **Abort** button while the list is fetched. The component tries `de1.api.radio-browser.info` first and falls back to the `de2`, `nl1` and `at1` mirrors.

Each station becomes a `.utag` file with its stream URL (`url_resolved`, or `url` if that is empty) as `@`, plus `name`, `title`, `genre` (one value per tag), `country`, `countrycode`, `language`, `codec`, `bitrate`, `homepage`, `favicon` and `stationuuid` when the directory has them. Stations without a name or an http(s) URL are skipped. A station that repeats a stream URL already used earlier in the same search is skipped too. Files are never overwritten; duplicate names receive a numeric suffix.

When the import finishes, the new files are also added to a new playlist named like the search folder, and that playlist becomes the active one. The results popup shows its name.

The results popup lists the first 60 stations in detail (stream URL `@` and output path) and the summary counts all of them. If Radio Browser cannot be reached, nothing is created and an error is shown.

## Browsing Radio Browser

1. Choose **File → Utag Input → Browse Radio Browser...**. The window stays open while you keep using foobar2000.
2. Open an entry from the home list (double-click, or press **Open**):
   - **Most clicked**, **Most voted**, **Trending now** (recently clicked) and **Recently added or changed**
   - **Tag...**, **Country...**, **Language...** and **Codec...**: each opens a list with a filter box, then shows the stations that match
   - **Search by name...**: moves to the search box at the top of the window (you can also click the box directly). Type part of a station name and press **Enter**; **Esc** clears the box. A new search while search results are showing replaces them, so **Back** still returns to the list you started from.
3. Each station list shows up to 200 working stations, with codec, bitrate and country code. Lists are always sorted ascending by name, including the top lists.
4. Select one or more stations and press **Import selected**, or double-click a station to import just that one. **Back** and **Home** move between lists.

Imports create a new sub folder of the default folder named after the list and the date and time, with the same station fields as the search import above. The created files are appended to the `RadioBrowser` playlist (see **Preferences**), which becomes the active playlist with the first appended item focused.

## Importing stations from TuneIn

1. Choose **File → Utag Input → Import stations from TuneIn...**.
2. Enter what to search for and choose **Station name**, **Artist** or **Song title**.
3. Set **Max stations** (1 to 200, default 25) and press **Import**. If no default folder is saved yet, you are asked to choose one first.

Each station needs a further request to find its stream: a direct URL is used if TuneIn gives one, otherwise the first stream in the station's `.pls` or `.m3u` playlist. Anything after a `?` is removed from the stream URL. Stations whose stream cannot be found are skipped and listed in the results.

Each station becomes a `.utag` file with `@`, `name`, `title`, `stream_tunein_id` (the TuneIn id), `favicon` (the station logo), and `codec` and `bitrate` when TuneIn provides them. Files go into a new sub folder of the default folder and are added to a new playlist, as with the Radio Browser import.

**Station details (optional):** when **Look up station details when importing from TuneIn** is enabled in the preferences, one more request per station is made to TuneIn's station description, and these fields are added when TuneIn provides them:

- `location`: city and state or country, as text
- `description`: the station description
- `genre`: TuneIn's genre name
- `tunein_url`: TuneIn's own address for the station
- `country_region_id`: TuneIn's country or region identifier

If the details cannot be fetched for a station, the file is still created and the results note it. The lookup makes imports noticeably slower, so it is off by default.

## Browsing TuneIn

1. Choose **File → Utag Input → Browse TuneIn...**. The window stays open while you keep using foobar2000.
2. The home list offers:
   - **Local**, **Music**, **Sports**, **Talk**, **By location**, **By language**, **Best** and **Popular**
   - **Genres**
   - **My presets**: uses your TuneIn username, asked for the first time and saved. **Set TuneIn username...** changes it.
   - **Search stations...**, **Search by artist...**, **Search by song title...** and **Recommended stations for a list of artists...**: these move to the search box at the top of the window and select the matching type in the drop-down next to it. You can also pick the type (**Station**, **Artist**, **Song title** or **Recommend**) yourself, type in the box and press **Enter**. For **Recommend**, enter up to 10 artist names separated by commas. **Esc** clears the box. A new search while search results are showing replaces them, so **Back** still returns to the list you started from.
   - **Recommendations for the playing station**: uses the `%stream_tunein_id%` field of the playing track, so it works for stations imported from TuneIn
3. Entries starting with `>` open another list. Select one or more stations and press **Import selected**, or double-click a station to import just that one. **Back** and **Home** move between lists.

Lists are always sorted ascending by name (inside each section, for lists that have sections).

Imported files are appended to the `Tunein` playlist (see **Preferences**), which becomes the active playlist with the first appended item focused.

TuneIn is read through its unofficial OPML web service, which may change or stop working.

## Importing from and browsing the Icecast Directory

- **File → Utag Input → Import stations from Icecast...** asks for search terms (matched against station name, genre, codec, description and current song) and a maximum number of stations (1 to 5000, default 100), then imports the matches into a new sub folder of the default folder. The files go into a new playlist named like that folder.
- **File → Utag Input → Browse Icecast Directory...** loads the directory once per session and keeps it in memory. Type in the search box at the top to filter the list live; **Enter** moves to the list and **Esc** clears the box. **Refresh** downloads the directory again and keeps the text in the box. Select one or more stations and press **Import selected** (or double-click one). The created files are appended to the `Icecast` playlist (see **Preferences**), which becomes the active playlist with the first appended item focused.

## Creating `.utag` files from a playlist

1. Select one or more playlist tracks.
2. Open the playlist context menu.
3. Choose one of the commands under **Tagging**:
   - **Create utag files...** asks for the destination folder each time and remembers the last one used.
   - **Create utag files in default folder** creates the files in the default folder without asking. If no default folder is saved yet, or the saved folder no longer exists, you are asked to choose one first.
   - **Set default utag folder...** only chooses and saves the default folder.
4. Read the results popup, which lists each entry's stream URL (`@`) and the file it was written to.
5. The new file(s) are added to the "New utags" playlist, which becomes the active playlist.

**Radio Browser lookup:** when **Look each URL up in Radio Browser when creating utag files from playlist tracks** is enabled in the preferences (it is by default), every track's URL is looked up in Radio Browser first, as in **Add internet radio url and create utag**. If Radio Browser knows the station, its details (name, genre, country, codec, bitrate, homepage, favicon and so on) become the `.utag`, and tags from the playlist entry that Radio Browser does not supply are kept. If it has no match, or the lookup fails, the track's own tags are used and the results note which happened. The lookups run in the background with a progress window and an **Abort** button; cancelling keeps the files already created.

The component attempts to derive the station name and title from the selected tracks' metadata. It copies other metadata fields and preserves multi-value tags. Files are created without overwriting existing files; duplicate names receive a numeric suffix such as `Station (2).utag`.

Existing `.utag` entries are skipped by these commands. Tracks that do not expose usable metadata may fall back to a title or URL-derived filename.

## Tagging existing `.utag` files from Radio Browser

1. Select one or more `.utag` items in a playlist or the Media Library. Other selected items are ignored.
2. Open the context menu and choose **Tagging → Search Radio Browser and tag**. The command only appears when a `.utag` item is selected.
3. Each item's `@` URL is looked up in Radio Browser in the background (progress window with **Abort**).
4. For every station found, the Radio Browser details are written into the item's tags through foobar2000's normal tagging, so the `.utag` file is rewritten exactly as after a Properties edit. `@` is never changed.

Every field Radio Browser supplies replaces the current value, including `name` and `title`. Fields it does not supply (your own tags, for example) are kept. Items that are not found or whose lookup fails are left unchanged and listed in the results popup, which ends with a summary of tagged, not found, skipped and failed items.

## Adding a station by URL

1. Choose **File → Utag Input → Add internet radio url and create utag**. The command is in the same block of the File menu as Add files, Add folder and Add location.
2. Enter the stream URL. It must be a full URL such as `http://host:8000/stream`.
3. Optionally enter a station name.
4. Optionally tick **Search Radio Browser for this URL (use its station details)**. The choice is remembered. If Radio Browser knows the stream, its details become the `.utag`; otherwise the stream's own details are used and the results note it.
5. Press **Create**. The `.utag` file is written to the default folder (see above); if none is set, you are asked to choose one first.

If the name field is empty, the component connects to the stream briefly and reads the station name from the `icy-name` (or `ice-name`) response header. No audio is downloaded, and a progress box with an **Abort** button appears if the lookup takes a moment. The result depends on the lookup:

- **Name found:** it is used as the station name and file name, and the results popup notes "(name from stream)".
- **Server reachable but no name sent:** the host (and port) from the URL is used as the name.
- **Server unreachable:** no file is created. The results popup shows an error with the URL and the reason, and the summary counts it as failed.
- **Aborted:** no file is created, and the results popup says the lookup was cancelled.
- **Name typed in:** no lookup is made, so an unreachable server does not block creating the file.

Files are never overwritten; duplicate names receive a numeric suffix such as `Station (2).utag`. 

The new file is added to the "New utags" playlist, which becomes the active playlist.

## Editing tags

The component supports writing updated metadata back to the `.utag` file. The stream URL field `@` is reserved and is not written as a normal tag.

When tags are saved, standard tag fields handled by the component are replaced using the metadata values supplied by foobar2000. Other JSON fields are retained unless they conflict with the fields being rewritten.

## Preferences

All options are on one page: **Preferences → Tools → Utag Input**.

- **Playback**
  - **Reuse the running stream when the playing item is retagged**: enabled by default
  - **Download the favicon image during playback**: disabled by default
- **Default folder**: used by **Create utag files in default folder**, by **Add internet radio url and create utag** and by the importers. If empty, you are asked when a folder is needed.
- **Messages**
  - **Write messages to the foobar2000 console**: enabled by default
  - **Show a results window after import and create commands**: enabled by default. Errors are always shown. When it is off, a short summary is written to the console instead, and the full per-station details too if console messages are on.
- **Radio Browser and TuneIn**
  - **Look each URL up in Radio Browser when creating utag files from playlist tracks**: enabled by default
  - **Look up station details when importing from TuneIn**: disabled by default. See below.
- **Playlists for the browse windows**: the playlist names used by the Radio Browser, TuneIn and Icecast browse windows (defaults `RadioBrowser`, `Tunein`, `Icecast`). Leave a box empty to use its default name. The playlist is created if it does not exist, and new stations are appended to it.

The page scrolls when the preferences window is too short to show everything, and it stretches to the available width.

## Stream handoff preference

The component provides this preference:

**Reuse the running stream when the playing item is retagged (avoids a reconnect pause)**

It is located under **Preferences → Tools → Utag Input** and is enabled by default.

When enabled, the component temporarily retains the current decoder when an input instance closes during playback. If a new instance for the same stream URL opens within approximately two seconds, it can take over the existing decoder rather than opening a new stream connection. If the decoder is not reused within the handoff window, it is released shortly afterward.

This behavior is intended to reduce reconnects and audible pauses when retagging the currently playing item. Disable the preference if you prefer every reopened input to use a fresh decoder connection.

## favicon download during playback

The component provides this preference:

**Download the favicon image during playback (saved next to the .utag file)**

It is located under **Preferences → Tools → Utag Input** and is disabled by default.

The favicon URL stored in the `.utag` JSON is used.

Downloads run asynchronously and should not block playback initialization.

The image is saved beside the .utag file using the same base filename and the extension inferred from the favicon URL.

Any existing sidecar image (including a manually assigned logo) is kept and no download is made, and downloads use a temporary file before publishing the completed image.

The built-in Album Art Viewer may cache artwork and might not refresh automatically when the download completes.

## Station logos

A station's artwork is a **sidecar image** beside its `.utag` file: same folder, same base name, with an image extension (`.png`, `.jpg`, `.jpeg`, `.gif`, `.bmp`, `.webp` or `.svg`). The artwork extractor reads it whether it was downloaded or assigned by hand, and the `.utag` JSON is not changed. A station no longer needs a `favicon` key to have artwork.

**Set station logo...** (playlist context menu, **Tagging**; appears when a `.utag` item is selected) opens a small window for the selected stations:

- **Drop an image file** (for example from Explorer) on the window.
- **Drop an image URL or an image dragged from a web page**; it is downloaded in the background (http and https).
- **Browse...** to pick a file.
- **Get favicon** downloads each station's own `favicon` URL right now, whatever the preference says.
- **Remove** deletes the logo image (also available as **Remove station logo** in the context menu, which asks first).

With several stations selected, a dropped image is used for all of them. The file is checked by its contents (not its name), copied beside the `.utag` under the station's base name with the correct extension, and replaces any older sidecar of that station. Images up to 32 MB are accepted. The preview needs Windows to be able to decode the format; SVG, and WebP on systems without the WebP extension, are saved and used as artwork but show no preview.

### Moving, copying and deleting stations

When foobar2000 moves, copies or deletes a `.utag` file, the logo image beside it is moved, copied or deleted too. Select the `.utag` items in a playlist or the Media Library and use foobar2000's own commands (**Move to...**, **Copy to...**, **Delete** / **Remove from disk**) or the File Operations component; foo_utag listens for the file operation notifications and follows along.

- A logo is moved only after its `.utag` file has arrived at the new location, and deleted only after the `.utag` file is gone.
- Existing artwork at the destination is never overwritten; the logo then stays where it was and a console message says so.
- Renaming a `.utag` file with a tool that reports it as a move renames the logo with it.
- Switch it off with **Move, copy and delete the logo image together with its .utag file...** in **Preferences → Tools → Utag Input** (on by default). When off, logo images stay where they are.
- This only works for tools that report their file operations to foobar2000. Moving files in Explorer, or a tool that does not notify, leaves the logo behind.

A logo that exists is never replaced by a downloaded favicon. Dropping onto the playlist itself is not possible for a component; use the window above.

The built-in Album Art Viewer may cache artwork and might need a track change or refresh to show a new logo.				
## Technical notes and limitations

- Stream decoding is delegated to foobar2000's URL/file and input-decoder services.
- Whether a URL can play depends on the URL, server behavior, and the decoders available in the foobar2000 installation.
- Live-stream metadata and technical information may depend on the source and decoder.
- The component does not connect to a stream during a library metadata scan.
- The source limits `.utag` files to 4 MiB.
- The playlist creation commands skip selected entries whose paths already end in `.utag`.
- Radio Browser import needs an internet connection and uses the SDK's `http_client`; it fetches at most 5000 stations per run. The Radio Browser lists fetch at most 200 stations.
- The Radio Browser lookup by URL uses the `/json/stations/byurl` endpoint, trying the same mirrors as the import.
- The browse windows add their files with an asynchronous location lookup, so the playlist focus is set once foobar2000 has resolved the new `.utag` files.
- TuneIn import and browsing use TuneIn's OPML web service (`opml.radiotime.com`, over http). It is unofficial and undocumented, so it may change or stop working.
- The station name lookup only reads HTTP response headers; it cannot find names for servers that do not send `icy-name` or `ice-name`.
- Dark mode in the input box needs foobar2000 v2.0 or later; the folder chooser is a Windows dialog and follows the Windows theme.

## Local File References and Metadata Separation

The component supports **local file references**, allowing a `.utag` file to reference an audio file stored on the local filesystem. For example:

```json
[
  {
    "@": "file://C:\\Users\\Username\\Music\\Example.m4a",
    "TITLE": "Example Track",
    "ARTIST": "Example Artist",
    "ALBUM": "Example Album"
  }
]
```

The `@` field identifies the referenced audio file. The component uses this reference to access the original audio and retrieve its media information.

### Non-destructive metadata storage

The component is designed to keep metadata separate from the original audio files:

- **External metadata:** Tags are stored in `.utag` JSON files rather than written into the original audio files.
- **Original files remain unchanged:** Updating or removing metadata through the component modifies the `.utag` file, not the referenced audio file's embedded tags.
- **Local file support:** References can point to audio files on local Windows drives.
- **Artwork integration:** Album artwork can be read through the referenced audio file. Artwork editing is delegated to the registered artwork editor for that file, rather than being stored in the `.utag` JSON.

This separation helps preserve original media files while allowing metadata to be managed independently.

**Note:** The component's metadata operations do not modify the referenced audio file. Artwork changes are a separate operation delegated to the referenced file's artwork editor and may modify the original audio file, depending on the editor and format.

## Example

For an audio file such as:

`C:\Users\Username\Music\Example.m4a`

The corresponding `.utag` file can store its reference and metadata separately:

`C:\Users\Username\Music\utags\Example.utag`

Editing the title, artist, album, or other supported metadata updates the `.utag` file, leaving the original audio file untouched by those metadata operations.

## Building

The provided source is a C++ foobar2000 component and includes:

- the foobar2000 SDK header `SDK/foobar2000.h`, plus `SDK/coreDarkMode.h` for dark mode support;
- the `nlohmann/json.hpp` JSON header;
- Windows SDK headers and libraries, including `Ole32.lib`, `Shell32.lib` and `Urlmon.lib`. The preferences page also uses `UxTheme.lib` and `Gdi32.lib` (linked through `#pragma comment`).

The station name lookup and the Radio Browser and TuneIn features use the SDK's `http_client` service, so the SDK version must provide it.

The source is split into several files, all of which belong to the component:

- `utag_component.cpp`: component name, version and description, filename check
- `utag_input.cpp`: the input (reading `.utag` files, tagging, playback)
- `utag_album_art.cpp`: artwork extractor and editor
- `utag_favicon.cpp`, `utag_handoff.cpp`: favicon download and sidecar image helpers; stream handoff and technical-info cache
- `utag_artwork.cpp`: manual station logos and the Station logo window
- `utag_fileops.cpp`: moves, copies and deletes logo images along with their `.utag` files
- `utag_context_menu.cpp`, `utag_mainmenu.cpp`: playlist context menu (create utag files, Radio Browser tagging) and File menu commands
- `utag_add_station.cpp`: add a station by URL
- `utag_radio_browser.cpp`, `utag_radio_browse.cpp`: Radio Browser import, browse and lookup by URL
- `utag_icecast_browse.cpp`: Icecast Directory import and browse
- `utag_tunein.cpp`, `utag_tunein_api.cpp`, `utag_tunein_browser.cpp`: TuneIn import, API layer and browse
- `utag_prefs.cpp`, `utag_config.cpp`: preferences page and saved settings
- `utag_ui_util.cpp`, `utag_util.cpp`: shared dialog, folder, playlist and helper functions
- `utag_common.h`, `utag_ui.h`, `utag_radio.h`, `utag_tunein.h`: shared headers

Build it with a compatible Windows C++ toolchain and foobar2000 SDK setup. The source uses the component filename `foo_utag.dll`. Exact project-generation and build commands depend on your local SDK and build environment.

## Other Links

A python script to create foo_utag .utag files from the Radio Browser station directory.

https://gist.github.com/smoralis/562aba0d5d9740310d5f0a6044d6d3f8

## License

MIT License
