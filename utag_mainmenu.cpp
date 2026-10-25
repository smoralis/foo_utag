#include "utag_ui.h"

// Explicit declaration keeps the menu command visible even if a project file
// is built against an older copy of utag_ui.h.
void utag_open_icecast_browse(HWND owner);
void utag_import_from_icecast(HWND owner);

// Put all foo_utag commands under a dedicated File > Utag submenu.
// {B7A9E1C2-6D34-4F80-9A21-5C8D3E7F1042}
static const GUID g_utag_mainmenu_group_guid =
{ 0xb7a9e1c2, 0x6d34, 0x4f80, { 0x9a, 0x21, 0x5c, 0x8d, 0x3e, 0x7f, 0x10, 0x42 } };

static mainmenu_group_popup_factory g_utag_mainmenu_group(
    g_utag_mainmenu_group_guid,
    mainmenu_groups::file,
    mainmenu_commands::sort_priority_dontcare,
    "Utag Input");
// {A3C5E7F1-4B28-4D9A-8E36-1F7B92D0C456}
static const GUID g_add_url_utag_guid =
{ 0xa3c5e7f1, 0x4b28, 0x4d9a, { 0x8e, 0x36, 0x1f, 0x7b, 0x92, 0xd0, 0xc4, 0x56 } };

// {53CE13D0-118F-4D5B-B106-E4845184780C}
static const GUID g_import_radio_browser_guid =
{ 0x53ce13d0, 0x118f, 0x4d5b, { 0xb1, 0x06, 0xe4, 0x84, 0x51, 0x84, 0x78, 0x0c } };

// {AF9D94CA-0F55-49A6-82A4-080D080CD809}
static const GUID g_import_tunein_guid =
{ 0xaf9d94ca, 0x0f55, 0x49a6, { 0x82, 0xa4, 0x08, 0x0d, 0x08, 0x0c, 0xd8, 0x09 } };

// {39CFB64F-FDD3-4787-8F60-7DA287EE43A5}
static const GUID g_browse_tunein_guid =
{ 0x39cfb64f, 0xfdd3, 0x4787, { 0x8f, 0x60, 0x7d, 0xa2, 0x87, 0xee, 0x43, 0xa5 } };

// {6EA12FAF-6764-4D45-8777-9E63E2664CB3}
static const GUID g_browse_radio_browser_guid =
{ 0x6ea12faf, 0x6764, 0x4d45, { 0x87, 0x77, 0x9e, 0x63, 0xe2, 0x66, 0x4c, 0xb3 } };

// {7D8E3A10-6B91-4D21-9F40-28D1B0C6A517}
static const GUID g_browse_icecast_guid =
{ 0x7d8e3a10, 0x6b91, 0x4d21, { 0x9f, 0x40, 0x28, 0xd1, 0xb0, 0xc6, 0xa5, 0x17 } };

// {E9A5D27B-4F31-4D6E-8A1C-27A6B9D53140}
static const GUID g_import_icecast_guid =
{ 0xe9a5d27b, 0x4f31, 0x4d6e, { 0x8a, 0x1c, 0x27, 0xa6, 0xb9, 0xd5, 0x31, 0x40 } };

class add_url_utag_mainmenu : public mainmenu_commands
{
public:
    enum { cmd_add_url = 0, cmd_import_radio_browser, cmd_browse_radio_browser, cmd_import_tunein, cmd_browse_tunein, cmd_import_icecast, cmd_browse_icecast, cmd_count };

    t_uint32 get_command_count() override
    {
        return cmd_count;
    }

    GUID get_command(t_uint32 index) override
    {
        switch (index)
        {
        case cmd_import_radio_browser: return g_import_radio_browser_guid;
        case cmd_import_tunein:        return g_import_tunein_guid;
        case cmd_browse_tunein:        return g_browse_tunein_guid;
        case cmd_browse_radio_browser: return g_browse_radio_browser_guid;
        case cmd_browse_icecast:       return g_browse_icecast_guid;
        case cmd_import_icecast:       return g_import_icecast_guid;
        default:                       return g_add_url_utag_guid;
        }
    }

    void get_name(t_uint32 index, pfc::string_base& out) override
    {
        if (index == cmd_import_radio_browser)
            out = "Import stations from Radio Browser...";
        else if (index == cmd_import_tunein)
            out = "Import stations from TuneIn...";
        else if (index == cmd_browse_tunein)
            out = "Browse TuneIn...";
        else if (index == cmd_browse_radio_browser)
            out = "Browse Radio Browser...";
        else if (index == cmd_browse_icecast)
            out = "Browse Icecast Directory...";
        else if (index == cmd_import_icecast)
            out = "Import stations from Icecast...";
        else
            out = "Add internet radio url and create utag";
    }

    bool get_description(t_uint32 index, pfc::string_base& out) override
    {
        if (index == cmd_import_radio_browser)
            out = "Download stations from the Radio Browser directory and create .utag files in the default utag folder.";
        else if (index == cmd_import_tunein)
            out = "Search TuneIn for stations and create .utag files in the default utag folder.";
        else if (index == cmd_browse_tunein)
            out = "Browse TuneIn categories, presets and recommendations and create .utag files for chosen stations.";
        else if (index == cmd_browse_radio_browser)
            out = "Browse Radio Browser top lists, tags, countries, languages and codecs and create .utag files for chosen stations.";
        else if (index == cmd_browse_icecast)
            out = "Browse the public Icecast stream directory, search station metadata and create .utag files for selected streams.";
        else if (index == cmd_import_icecast)
            out = "Filter the Icecast directory and import up to a chosen number of matching stations as .utag files.";
        else
            out = "Enter a stream URL and create a .utag file for it in the default utag folder.";
        return true;
    }

    GUID get_parent() override
    {
        return g_utag_mainmenu_group_guid;
    }

    t_uint32 get_sort_priority() override
    {
        return sort_priority_dontcare;
    }

    void execute(t_uint32 index, service_ptr_t<service_base>) override
    {
        HWND owner = core_api::get_main_window();

        if (index == cmd_import_radio_browser)
        {
            utag_import_from_radio_browser(owner);
            return;
        }

        if (index == cmd_import_tunein)
        {
            utag_open_tunein_dialog(owner);
            return;
        }

        if (index == cmd_browse_tunein)
        {
            utag_open_tunein_browser(owner);
            return;
        }

        if (index == cmd_browse_radio_browser)
        {
            utag_open_radio_browse(owner);
            return;
        }

        if (index == cmd_browse_icecast)
        {
            utag_open_icecast_browse(owner);
            return;
        }

        if (index == cmd_import_icecast)
        {
            utag_import_from_icecast(owner);
            return;
        }

        utag_open_station_dialog(owner);
    }
};

static mainmenu_commands_factory_t<add_url_utag_mainmenu>
g_add_url_utag_mainmenu_factory;
