#include "utag_common.h"

DECLARE_COMPONENT_VERSION(
    "utag Input",
    "1.0.11",
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
    "- Station favicon shown as artwork (downloaded during playback, optional)\n"
    "- Reuses the running stream when the playing item is retagged (optional)\n"
    "\n"
    "Creating .utag files:\n"
    "- Playlist context menu: Tagging > Create utag files...\n"
    "- Set default utag folder... / Create utag files in default folder: no prompt\n"
    "- File > Add internet radio url and create utag (the station name is read from\n"
    "  the stream when left empty; the result goes to the \"New utags\" playlist)\n"
    "- File > Import stations from Radio Browser... (search by name, tag, country,\n"
    "  codec and minimum bitrate; pickers list all tags, countries and codecs)\n"
    "- File > Browse Radio Browser... (top lists, tags, countries, languages, codecs,\n"
    "  search by name; pick stations to import)\n"
    "- File > Import stations from TuneIn... (search by station, artist or song)\n"
    "- File > Browse TuneIn... (categories, genres, location, language, your presets,\n"
    "  search, recommendations by artists and for the playing station)\n"
    "- Imports run in the background into a new sub folder of the default folder and\n"
    "  are added to a new playlist; errors are always reported\n"
    "\n"
    "Preferences (Preferences > Tools > Utag Input):\n"
    "- Reuse stream on retag, favicon download, default utag folder\n"
    "- Console messages and optional results windows after import and create commands\n"
    "- Optional TuneIn station details (location, description, genre, region) on import\n"
    "\n"
    "utag format: [ { \"@\": \"http://host:port/stream\", \"name\": \"Station\", ... } ]"
);

VALIDATE_COMPONENT_FILENAME("foo_utag.dll");

// {C46BA00F-5FB9-4E95-9A9C-990B65400232}
const GUID g_utag_guid =
{ 0xc46ba00f, 0x5fb9, 0x4e95, { 0x9a, 0x9c, 0x99, 0xb, 0x65, 0x40, 0x2, 0x32 } };
