#include "utag_ui.h"

// Main menu group that hosts the command. If it lands in the wrong block of the
// File menu, point this at another group from mainmenu_groups (mainmenu.h).
#define UTAG_MAINMENU_GROUP mainmenu_groups::file_add

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

class add_url_utag_mainmenu : public mainmenu_commands
{
public:
    enum { cmd_add_url = 0, cmd_import_radio_browser, cmd_import_tunein, cmd_browse_tunein, cmd_browse_radio_browser, cmd_count };

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
        else
            out = "Enter a stream URL and create a .utag file for it in the default utag folder.";
        return true;
    }

    GUID get_parent() override
    {
        // "file_add" is the File menu block with Add files / Add folder / Add location.
        return UTAG_MAINMENU_GROUP;
    }

    // Sort after the built-in items of the group (i.e. after "Add location...").
    t_uint32 get_sort_priority() override
    {
        return sort_priority_last;
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

        utag_open_station_dialog(owner);
    }
};

static mainmenu_commands_factory_t<add_url_utag_mainmenu>
g_add_url_utag_mainmenu_factory;
