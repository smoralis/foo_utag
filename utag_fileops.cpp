#include "utag_common.h"

// -----------------------------------------------------------------------------
// Keeps a station's logo image with its .utag file.
//
// foobar2000's own file operations (Move to / Copy to / Delete in the playlist
// and library context menus, and the File Operations component) tell every
// registered file_operation_callback what they did. When the item is a .utag
// file, the sidecar image beside it is moved, copied or deleted to match.
// Switchable in Preferences > Tools > Utag Input.
//
// Paths in the callbacks are foobar2000 paths ("file://C:\..."). The lists are
// sorted by path; for moves and copies `from` and `to` correspond by index.
// -----------------------------------------------------------------------------

namespace
{
    static const wchar_t* const kExts[] = {
        L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg" };

    bool native_file_exists(const std::wstring& path)
    {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    // Full native path of the .utag file itself.
    std::wstring native_utag_path(const std::string& path)
    {
        try
        {
            return utf8_to_wide(to_native_windows_path(path));
        }
        catch (...)
        {
            return {};
        }
    }

    // Existing sidecar of `fromPath`, as (stem + extension) in the *source* folder.
    std::vector<std::wstring> sidecar_extensions_present(const std::wstring& stem)
    {
        std::vector<std::wstring> present;
        for (const wchar_t* ext : kExts)
            if (native_file_exists(stem + ext))
                present.emplace_back(ext);
        return present;
    }

    class utag_file_operations : public file_operation_callback
    {
    public:
        void on_files_deleted_sorted(const pfc::list_base_const_t<const char*>& items) override
        {
            if (!logo_fileops_enabled()) return;

            for (t_size i = 0; i < items.get_count(); ++i)
            {
                try
                {
                    const char* path = items[i];
                    if (path == nullptr || !is_utag_path(path)) continue;

                    // Only when the .utag really is gone.
                    const std::wstring utag = native_utag_path(path);
                    if (utag.empty() || native_file_exists(utag)) continue;

                    const unsigned removed = delete_station_artwork_except(path, std::wstring());
                    if (removed > 0)
                        UTAG_LOG("[foo_utag] deleted %u logo image(s) of %s", removed, path);
                }
                catch (...)
                {
                }
            }
        }

        void on_files_moved_sorted(
            const pfc::list_base_const_t<const char*>& from,
            const pfc::list_base_const_t<const char*>& to) override
        {
            transfer(from, to, true);
        }

        void on_files_copied_sorted(
            const pfc::list_base_const_t<const char*>& from,
            const pfc::list_base_const_t<const char*>& to) override
        {
            transfer(from, to, false);
        }

    private:
        void transfer(
            const pfc::list_base_const_t<const char*>& from,
            const pfc::list_base_const_t<const char*>& to,
            bool move)
        {
            if (!logo_fileops_enabled()) return;

            const t_size count = (std::min)(from.get_count(), to.get_count());
            for (t_size i = 0; i < count; ++i)
            {
                try
                {
                    const char* src = from[i];
                    const char* dst = to[i];
                    if (src == nullptr || dst == nullptr) continue;
                    if (!is_utag_path(src) || !is_utag_path(dst)) continue;

                    // The .utag must have arrived, and (for a move) left.
                    const std::wstring dstUtag = native_utag_path(dst);
                    const std::wstring srcUtag = native_utag_path(src);
                    if (dstUtag.empty() || srcUtag.empty()) continue;
                    if (!native_file_exists(dstUtag)) continue;
                    if (move && native_file_exists(srcUtag)) continue;

                    const std::wstring srcStem = station_artwork_stem(src);
                    const std::wstring dstStem = station_artwork_stem(dst);
                    if (srcStem.empty() || dstStem.empty()) continue;
                    if (_wcsicmp(srcStem.c_str(), dstStem.c_str()) == 0) continue;

                    const std::vector<std::wstring> present = sidecar_extensions_present(srcStem);
                    if (present.empty()) continue;

                    // Never overwrite artwork that is already at the destination.
                    if (!find_station_artwork(dst).empty())
                    {
                        UTAG_LOG("[foo_utag] logo not %s: the destination already has one: %s",
                            move ? "moved" : "copied", dst);
                        continue;
                    }

                    // One sidecar per station is the norm; if the source has
                    // several, carry only the newest one (what the artwork
                    // extractor would use).
                    std::wstring chosen = find_station_artwork(src);
                    if (chosen.empty()) continue;
                    const size_t dot = chosen.find_last_of(L'.');
                    const std::wstring ext = dot == std::wstring::npos
                        ? std::wstring() : chosen.substr(dot);
                    const std::wstring target = dstStem + ext;

                    bool ok;
                    if (move)
                        ok = MoveFileExW(chosen.c_str(), target.c_str(),
                            MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != FALSE;
                    else
                        ok = CopyFileW(chosen.c_str(), target.c_str(), TRUE) != FALSE;

                    if (ok)
                    {
                        UTAG_LOG("[foo_utag] logo %s with %s", move ? "moved" : "copied", dst);
                        notify_utag_artwork_changed(dst);
                        if (move)
                            notify_utag_artwork_changed(src);
                    }
                    else
                    {
                        UTAG_LOG("[foo_utag] could not %s the logo of %s (Win32 error %lu)",
                            move ? "move" : "copy", src,
                            static_cast<unsigned long>(GetLastError()));
                    }
                }
                catch (...)
                {
                }
            }
        }
    };

    static service_factory_single_t<utag_file_operations> g_utag_file_operations_factory;
}
