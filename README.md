# utag Input for foobar2000

`utag Input` is a foobar2000 input component that lets you add internet radio streams and other remote URLs to the Media Library by representing each stream with a small `.utag` JSON file.

The `.utag` file stores the stream URL and metadata. foobar2000 can scan and tag the stub like a normal track, while audio is decoded from the original URL.

## Features

- **Library-friendly scanning:** scanning a `.utag` file does not connect to the stream.
- **Editable metadata:** tag changes are written back to the `.utag` JSON file.
- **Stream URL in Properties:** the URL is exposed in the information section as `@` and can be displayed with `%__@%`.
- **Create files from a playlist:** use **Tagging → Create utag files...** in the playlist context menu to create one `.utag` file for each selected track.
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

## Adding a station to the Media Library

1. Create a `.utag` file containing a one-element JSON array as shown above.
2. Set `@` to the station's actual stream URL.
3. Add the `.utag` file to foobar2000's Media Library or a monitored library folder.
4. Play the entry. The component opens the URL and lets foobar2000's input decoder handle the stream.

Library scans use the metadata-only open path and do not intentionally connect to the stream. Technical information is obtained from the decoder during playback.

## Creating `.utag` files from a playlist

1. Select one or more playlist tracks.
2. Open the playlist context menu.
3. Choose **Tagging → Create utag files...**.
4. Choose the destination folder.

The component attempts to derive the station name and title from the selected tracks' metadata. It copies other metadata fields and preserves multi-value tags. Files are created without overwriting existing files; duplicate names receive a numeric suffix such as `Station (2).utag`.

Existing `.utag` entries are skipped by this command. Tracks that do not expose usable metadata may fall back to a title or URL-derived filename.

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
- The playlist creation command skips selected entries whose paths already end in `.utag`.

## Building

The provided source is a C++ foobar2000 component and includes:

- the foobar2000 SDK header `SDK/foobar2000.h`;
- the `nlohmann/json.hpp` JSON header;
- Windows SDK headers and libraries, including `Ole32.lib` and `Shell32.lib`.

Build it with a compatible Windows C++ toolchain and foobar2000 SDK setup. The source uses the component filename `foo_utag.dll`. Exact project-generation and build commands depend on your local SDK and build environment.

## License

MIT License
