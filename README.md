# utag Input for foobar2000

`utag Input` is a foobar2000 input component that lets you add internet radio streams and other remote URLs to the Media Library by representing each stream with a small `.utag` JSON file.

The `.utag` file stores the stream URL and metadata. foobar2000 can scan and tag the stub like a normal track, while audio is decoded from the original URL.

## Features

- **Library-friendly scanning:** scanning a `.utag` file does not connect to the stream.
- **Editable metadata:** tag changes are written back to the `.utag` JSON file.
- **Stream URL in Properties:** the URL is exposed in the information section as `@` and can be displayed with `%__@%`.
- **Create files from a playlist:** use **Tagging → Create utag files...** in the playlist context menu to create one `.utag` file for each selected track.
- **Default folder:** **Tagging → Set default utag folder...** saves an output folder, and **Tagging → Create utag files in default folder** creates the files there without asking.
- **Add a station by URL:** **File → Add internet radio url and create utag** asks for a stream URL (and an optional name) and creates the `.utag` file in the default folder. If no name is entered, it is looked up from the stream.
- **Import from Radio Browser:** **File → Import stations from Radio Browser...** downloads stations from the [Radio Browser](https://www.radio-browser.info/) directory and creates one `.utag` file per station, in a new folder named after the search.																																																														
- **Results popup:** every created or skipped entry lists the stream URL (`@`) and the output path; errors list the source URL and the reason.
- **Optional stream handoff:** an Advanced preference can reuse the running decoder when the playing item is reopened after retagging, helping avoid a reconnect and audible pause.

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

1. Choose **File → Import stations from Radio Browser...**. It sits in the same block of the File menu as the add-by-URL command.
2. Fill in any filters; all of them are optional:
   - **Name contains** and **Tag / genre**
   - **Country code**: two letters, such as `DE`
   - **Min. bitrate (kbps)**
   - **Maximum stations**: 1 to 5000, default 100
3. Press **Import**. If no default folder is saved yet, you are asked to choose one first.

Every import goes into its own new sub folder of the default folder, named after the search and the date and time, for example `Radio Browser - jazz - DE - 128+ kbps - top 100 - 2026-10-18 14-32-05` (tag, name, country code and minimum bitrate appear only when you filled them in). Earlier imports are therefore never mixed with new ones. No folder is created when nothing is found or the download fails.

The most-clicked working stations that match are downloaded (broken stations are left out), with an **Abort** button while the list is fetched. The component tries `de1.api.radio-browser.info` first and falls back to the `de2`, `nl1` and `at1` mirrors.

Each station becomes a `.utag` file with its stream URL (`url_resolved`, or `url` if that is empty) as `@`, plus `name`, `title`, `genre` (one value per tag), `country`, `countrycode`, `language`, `codec`, `bitrate`, `homepage`, `favicon` and `stationuuid` when the directory has them. Stations without a name or an http(s) URL are skipped. A station that repeats a stream URL already used earlier in the same search is skipped too. Files are never overwritten; duplicate names receive a numeric suffix.

When the import finishes, the new files are also added to a new playlist named like the search folder, and that playlist becomes the active one. The results popup shows its name.

The results popup lists the first 60 stations in detail (stream URL `@` and output path) and the summary counts all of them. If Radio Browser cannot be reached, nothing is created and an error is shown.
## Creating `.utag` files from a playlist

1. Select one or more playlist tracks.
2. Open the playlist context menu.
3. Choose one of the commands under **Tagging**:
   - **Create utag files...** asks for the destination folder each time and remembers the last one used.
   - **Create utag files in default folder** creates the files in the default folder without asking. If no default folder is saved yet, or the saved folder no longer exists, you are asked to choose one first.
   - **Set default utag folder...** only chooses and saves the default folder.
4. Read the results popup, which lists each entry's stream URL (`@`) and the file it was written to.

The component attempts to derive the station name and title from the selected tracks' metadata. It copies other metadata fields and preserves multi-value tags. Files are created without overwriting existing files; duplicate names receive a numeric suffix such as `Station (2).utag`.

Existing `.utag` entries are skipped by these commands. Tracks that do not expose usable metadata may fall back to a title or URL-derived filename.

## Adding a station by URL

1. Choose **File → Add internet radio url and create utag**. The command is in the same block of the File menu as Add files, Add folder and Add location.
2. Enter the stream URL. It must be a full URL such as `http://host:8000/stream`.
3. Optionally enter a station name.
4. Press **Create**. The `.utag` file is written to the default folder (see above); if none is set, you are asked to choose one first.

If the name field is empty, the component connects to the stream briefly and reads the station name from the `icy-name` (or `ice-name`) response header. No audio is downloaded, and a progress box with an **Abort** button appears if the lookup takes a moment. The result depends on the lookup:

- **Name found:** it is used as the station name and file name, and the results popup notes "(name from stream)".
- **Server reachable but no name sent:** the host (and port) from the URL is used as the name.
- **Server unreachable:** no file is created. The results popup shows an error with the URL and the reason, and the summary counts it as failed.
- **Aborted:** no file is created, and the results popup says the lookup was cancelled.
- **Name typed in:** no lookup is made, so an unreachable server does not block creating the file.

Files are never overwritten; duplicate names receive a numeric suffix such as `Station (2).utag`. 

## Editing tags

The component supports writing updated metadata back to the `.utag` file. The stream URL field `@` is reserved and is not written as a normal tag.

When tags are saved, standard tag fields handled by the component are replaced using the metadata values supplied by foobar2000. Other JSON fields are retained unless they conflict with the fields being rewritten.

## Stream handoff preference

The component provides this Advanced preference:

**utag: reuse stream when playing item is retagged**

It is located under **Preferences → Advanced → Tools** and is enabled by default.

When enabled, the component temporarily retains the current decoder when an input instance closes during playback. If a new instance for the same stream URL opens within approximately two seconds, it can take over the existing decoder rather than opening a new stream connection. If the decoder is not reused within the handoff window, it is released shortly afterward.

This behavior is intended to reduce reconnects and audible pauses when retagging the currently playing item. Disable the preference if you prefer every reopened input to use a fresh decoder connection.

## Technical notes and limitations

- Stream decoding is delegated to foobar2000's URL/file and input-decoder services.
- Whether a URL can play depends on the URL, server behavior, and the decoders available in the foobar2000 installation.
- Live-stream metadata and technical information may depend on the source and decoder.
- The component does not connect to a stream during a library metadata scan.
- The source limits `.utag` files to 4 MiB.
- The playlist creation commands skip selected entries whose paths already end in `.utag`.
- Radio Browser import needs an internet connection and uses the SDK's `http_client`; it fetches at most 5000 stations per run.
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
- Windows SDK headers and libraries, including `Ole32.lib` and `Shell32.lib`.

The station name lookup uses the SDK's `http_client` service, so the SDK version must provide it.

Build it with a compatible Windows C++ toolchain and foobar2000 SDK setup. The source uses the component filename `foo_utag.dll`. Exact project-generation and build commands depend on your local SDK and build environment.

## License

MIT License
