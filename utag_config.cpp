#include "utag_common.h"

// -----------------------------------------------------------------------------
// When the playing item is retagged, foobar2000 closes the playing input and
// opens a new one, which reconnects the live stream (audible pause). With this
// option on, the closing instance parks its running decoder here for a few
// seconds, and a new instance for the same URL picks it up instead of
// reconnecting.
//
// Preferences > Tools > Utag Input. On by default.
// -----------------------------------------------------------------------------

// {1DE83138-5922-43DB-9226-CB9B059F8C1A}
static const GUID g_guid_cfg_handoff =
{ 0x1de83138, 0x5922, 0x43db, { 0x92, 0x26, 0xcb, 0x9b, 0x05, 0x9f, 0x8c, 0x1a } };

cfg_bool g_cfg_handoff(g_guid_cfg_handoff, true);

bool handoff_enabled()
{
    return g_cfg_handoff;
}


// Optional playback-time favicon download. The image is stored beside the
// .utag file, using the same basename and the favicon URL's file extension.
// {30D0CE54-00A8-4EF0-98FB-02E9906B9422}
static const GUID g_guid_cfg_favicon =
{ 0x30d0ce54, 0x00a8, 0x4ef0, { 0x98, 0xfb, 0x02, 0xe9, 0x90, 0x6b, 0x94, 0x22 } };

cfg_bool g_cfg_favicon(g_guid_cfg_favicon, false);

bool favicon_download_enabled()
{
    return g_cfg_favicon;
}

// {D65D2ADE-1652-4447-BD71-21BF4C86EE8D}
static const GUID g_guid_cfg_logging =
{ 0xd65d2ade, 0x1652, 0x4447, { 0xbd, 0x71, 0x21, 0xbf, 0x4c, 0x86, 0xee, 0x8d } };

cfg_bool g_cfg_logging(g_guid_cfg_logging, true);

bool utag_console_logging_enabled()
{
    return g_cfg_logging;
}

// Default output folder for automatic utag creation (persisted).
// {23AB3FAC-61AE-4376-A99C-0F044D60697F}
static const GUID g_guid_cfg_default_dir =
{ 0x23ab3fac, 0x61ae, 0x4376, { 0xa9, 0x9c, 0x0f, 0x04, 0x4d, 0x60, 0x69, 0x7f } };

cfg_string g_cfg_default_dir(g_guid_cfg_default_dir, "");

// {A933B14B-A0A7-44CB-9A5B-856E915DCF43}
static const GUID g_guid_cfg_tunein_username =
{ 0xa933b14b, 0xa0a7, 0x44cb, { 0x9a, 0x5b, 0x85, 0x6e, 0x91, 0x5d, 0xcf, 0x43 } };

// TuneIn username, used for "My presets" in the TuneIn browser.
cfg_string g_cfg_tunein_username(g_guid_cfg_tunein_username, "");

// {3836CE30-82CA-4EB4-BD55-DF1652C47A0B}
static const GUID g_guid_cfg_result_popups =
{ 0x3836ce30, 0x82ca, 0x4eb4, { 0xbd, 0x55, 0xdf, 0x16, 0x52, 0xc4, 0x7a, 0x0b } };

// Show the results window after import / create commands (errors always show).
cfg_bool g_cfg_result_popups(g_guid_cfg_result_popups, true);

// {C383DE37-BE45-4BFD-9DF8-8D8394D6429D}
static const GUID g_guid_cfg_tunein_details =
{ 0xc383de37, 0xbe45, 0x4bfd, { 0x9d, 0xf8, 0x8d, 0x83, 0x94, 0xd6, 0x42, 0x9d } };

// TuneIn import: look up location, description, genre and region per station.
cfg_bool g_cfg_tunein_details(g_guid_cfg_tunein_details, false);

// {86F235C6-6F5A-4CB4-A1C4-E2ECC4F773AD}
static const GUID g_guid_cfg_add_rb_lookup =
{ 0x86f235c6, 0x6f5a, 0x4cb4, { 0xa1, 0xc4, 0xe2, 0xec, 0xc4, 0xf7, 0x73, 0xad } };

// "Add internet radio url" window: look the URL up in Radio Browser first.
cfg_bool g_cfg_add_rb_lookup(g_guid_cfg_add_rb_lookup, false);

// {B2E4A7D1-5C38-4F69-8A0E-93D61F7C25B4}
static const GUID g_guid_cfg_ctx_rb_lookup =
{ 0xb2e4a7d1, 0x5c38, 0x4f69, { 0x8a, 0x0e, 0x93, 0xd6, 0x1f, 0x7c, 0x25, 0xb4 } };

// "Create utag files" playlist commands: look each URL up in Radio Browser
// first (same lookup as the "Add internet radio url" window).
cfg_bool g_cfg_ctx_rb_lookup(g_guid_cfg_ctx_rb_lookup, true);

// Playlists that the Radio Browser, TuneIn and Icecast imports append their
// results to (created when missing). An empty value means the default name.
// {5E1B8C42-7A93-4D05-B6F8-0C2D49E7A316}
static const GUID g_guid_cfg_playlist_radiobrowser =
{ 0x5e1b8c42, 0x7a93, 0x4d05, { 0xb6, 0xf8, 0x0c, 0x2d, 0x49, 0xe7, 0xa3, 0x16 } };

// {C47D2A95-3E18-4B60-91F4-A8B05D6E7F23}
static const GUID g_guid_cfg_playlist_tunein =
{ 0xc47d2a95, 0x3e18, 0x4b60, { 0x91, 0xf4, 0xa8, 0xb0, 0x5d, 0x6e, 0x7f, 0x23 } };

// {1F96E0B3-D825-4C7A-8E31-57B4C2A90D68}
static const GUID g_guid_cfg_playlist_icecast =
{ 0x1f96e0b3, 0xd825, 0x4c7a, { 0x8e, 0x31, 0x57, 0xb4, 0xc2, 0xa9, 0x0d, 0x68 } };

cfg_string g_cfg_playlist_radiobrowser(g_guid_cfg_playlist_radiobrowser, "RadioBrowser");
cfg_string g_cfg_playlist_tunein(g_guid_cfg_playlist_tunein, "Tunein");
cfg_string g_cfg_playlist_icecast(g_guid_cfg_playlist_icecast, "Icecast");

// Move, copy and delete a station's logo image together with its .utag file
// when foobar2000 (File Operations, Remove from disk) does that to the .utag.
// {86953291-A2F5-4D4C-8603-AE4B4D121379}
static const GUID g_guid_cfg_logo_fileops =
{ 0x86953291, 0xa2f5, 0x4d4c, { 0x86, 0x03, 0xae, 0x4b, 0x4d, 0x12, 0x13, 0x79 } };

cfg_bool g_cfg_logo_fileops(g_guid_cfg_logo_fileops, true);

bool logo_fileops_enabled()
{
    return g_cfg_logo_fileops;
}
