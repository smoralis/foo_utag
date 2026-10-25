#include "utag_common.h"

static std::mutex g_faviconMutex;
static std::set<std::wstring> g_faviconInFlight;

// Helper: accepted URL file extensions (lowercase, include leading dot).
static bool is_known_image_ext(const std::string& ext)
{
    const std::string lower = lowercase_ascii(ext);
    static const char* const allowed[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg"
    };
    for (const char* a : allowed)
        if (lower == a) return true;
    return false;
}

// Extension (with leading dot) of the favicon image stored beside a .utag
// file. Used by BOTH the downloader and the artwork extractor so they always
// agree on the sidecar's filename.
std::string favicon_extension_for_url(const std::string& url)
{
    std::string extension = ".png";
    const size_t query = url.find_first_of("?#");
    const std::string urlPath = url.substr(0, query);
    const size_t dot = urlPath.find_last_of('.');
    const size_t slash = urlPath.find_last_of('/');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash))
    {
        const std::string ext = urlPath.substr(dot); // includes leading dot
        if (ext.size() >= 2 && ext.size() <= 12 && is_known_image_ext(ext))
            extension = ext;
    }
    return extension;
}

// Sniffs the leading bytes of a file and returns the extension (with leading
// dot, lowercase) that matches the image format, or an empty string when the
// file is not a recognized image. Used to validate downloads and manually
// assigned logos, and to name the sidecar after what the file really is.
std::wstring image_extension_by_magic(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    unsigned char buf[512] = { 0 };
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return {};

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (got >= 8 &&
        buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G' &&
        buf[4] == 0x0D && buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A)
        return L".png";

    // JPEG: FF D8 FF
    if (got >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)
        return L".jpg";

    // GIF: "GIF87a" or "GIF89a"
    if (got >= 6 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F' &&
        buf[3] == '8' && (buf[4] == '7' || buf[4] == '9') && buf[5] == 'a')
        return L".gif";

    // BMP: 'B' 'M'
    if (got >= 2 && buf[0] == 'B' && buf[1] == 'M')
        return L".bmp";

    // WebP: "RIFF"....  "WEBP"
    if (got >= 12 &&
        buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
        buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P')
        return L".webp";

    // SVG: skip an optional UTF-8 BOM, then require a real <svg element. A
    // bare "<?xml" is often an XML or HTML error page (e.g. a 404) and must
    // not be saved as artwork.
    size_t offset = 0;
    if (got >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        offset = 3;
    if (got > offset && buf[offset] == '<')
    {
        const std::string text(reinterpret_cast<char*>(buf) + offset,
            static_cast<size_t>(got) - offset);
        if (text.find("<svg") != std::string::npos) return L".svg";
    }

    // NOTE: ICO support intentionally not accepted.
    return {};
}

// The sidecar image sits beside the .utag file: same folder, same basename.
static const wchar_t* const kSidecarExtensions[] = {
    L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg"
};

std::wstring station_artwork_stem(const std::string& utagPath)
{
    try
    {
        const std::wstring wide = utf8_to_wide(to_native_windows_path(utagPath));
        const size_t slash = wide.find_last_of(L"\\/");
        const size_t dot = wide.find_last_of(L'.');
        if (dot == std::wstring::npos ||
            (slash != std::wstring::npos && dot < slash))
            return {};
        return wide.substr(0, dot);
    }
    catch (...)
    {
        return {};
    }
}

// Returns the sidecar image of a .utag file (a manually assigned logo or a
// downloaded favicon - both are the same kind of file), or an empty string.
// The sidecar's extension does not have to match the "favicon" URL, and the
// "favicon" key does not have to exist. Should several candidates exist, the
// most recently written one wins.
std::wstring find_station_artwork(const std::string& utagPath)
{
    const std::wstring stem = station_artwork_stem(utagPath);
    if (stem.empty()) return {};

    std::wstring best;
    FILETIME bestTime = {};
    for (const wchar_t* ext : kSidecarExtensions)
    {
        const std::wstring candidate = stem + ext;
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &data))
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (best.empty() || CompareFileTime(&data.ftLastWriteTime, &bestTime) > 0)
        {
            best = candidate;
            bestTime = data.ftLastWriteTime;
        }
    }
    return best;
}

// Deletes every sidecar image of a .utag file except `keep` (may be empty).
// Returns how many files were deleted.
unsigned delete_station_artwork_except(const std::string& utagPath, const std::wstring& keep)
{
    const std::wstring stem = station_artwork_stem(utagPath);
    if (stem.empty()) return 0;

    unsigned removed = 0;
    for (const wchar_t* ext : kSidecarExtensions)
    {
        const std::wstring candidate = stem + ext;
        if (!keep.empty() && _wcsicmp(candidate.c_str(), keep.c_str()) == 0)
            continue;
        if (DeleteFileW(candidate.c_str()))
            ++removed;
    }
    return removed;
}

// Ask foobar2000 to refresh the .utag metadata after its favicon sidecar
// has been successfully published. This gives artwork viewers a chance to
// query the extractor again; it is intentionally never called on download failure.
void notify_utag_artwork_changed(const std::string& utagPath)
{
    try
    {
        metadb_handle_ptr handle = metadb::get()->handle_create(utagPath.c_str(), 0);
        file_info_impl info;
        if (!handle->get_info(info))
            return;

        const t_filestats stats = handle->get_filestats();
        fb2k::inMainThread([handle, info, stats]() {
            try
            {
                auto hints = metadb_hint_list_v3::create();
                hints->add_hint_forced(handle, info, stats, true);
                hints->on_done();
            }
            catch (...)
            {
            }
            });
    }
    catch (...)
    {
    }
}
void schedule_favicon_download_for_playback(
    const std::string& utagPath,
    const json& station)
{
    if (!favicon_download_enabled())
        return;

    const auto it = station.find("favicon");
    if (it == station.end() || !it->is_string())
        return;

    const std::string url = it->get<std::string>();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return;

    // Any existing sidecar - including a manually assigned logo - wins over a
    // downloaded favicon.
    if (!find_station_artwork(utagPath).empty())
        return;

    try
    {
        // Use a native Windows path (strip "file://") before converting to wide.
        const std::string nativeUtagPath = to_native_windows_path(utagPath);
        const std::wstring widePath = utf8_to_wide(nativeUtagPath);

        const size_t slash = widePath.find_last_of(L"\\/");
        const size_t dot = widePath.find_last_of(L'.');
        if (dot == std::wstring::npos ||
            (slash != std::wstring::npos && dot < slash))
            return;

        // Same extension rule as the artwork extractor (see
        // favicon_extension_for_url).
        const std::wstring extension =
            utf8_to_wide(favicon_extension_for_url(url));

        const std::wstring imagePath = widePath.substr(0, dot) + extension;

        // only skip when a file already exists at imagePath and it is not a directory.
        const DWORD attrs = GetFileAttributesW(imagePath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            UTAG_LOG("[foo_utag] favicon already exists, skipping download: %s",
                pfc::stringcvt::string_utf8_from_wide(imagePath.c_str()).get_ptr());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_faviconMutex);
            if (!g_faviconInFlight.insert(imagePath).second)
                return;
        }

        UTAG_LOG("[foo_utag] starting favicon download: %s -> %s",
            url.c_str(), pfc::stringcvt::string_utf8_from_wide(imagePath.c_str()).get_ptr());

        std::thread([url, imagePath, utagPath]() {
            HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const std::wstring tempPath = imagePath + L".download.tmp";
            try
            {
                // Another playback may have completed the same download meanwhile.
                const DWORD existing = GetFileAttributesW(imagePath.c_str());
                if (existing != INVALID_FILE_ATTRIBUTES &&
                    (existing & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    std::lock_guard<std::mutex> lock(g_faviconMutex);
                    g_faviconInFlight.erase(imagePath);
                    if (SUCCEEDED(comResult)) CoUninitialize();
                    return;
                }

                // Download to a temporary file so artwork readers never see a
                // partially-written image.
                DeleteFileW(tempPath.c_str());
                const std::wstring wideUrl = utf8_to_wide(url);
                const HRESULT hr = URLDownloadToFileW(
                    nullptr, wideUrl.c_str(), tempPath.c_str(), 0, nullptr);

                if (SUCCEEDED(hr))
                {
                    // A logo may have been assigned while the download ran.
                    if (!find_station_artwork(utagPath).empty())
                    {
                        UTAG_LOG("[foo_utag] a logo was assigned meanwhile, discarding favicon download");
                        DeleteFileW(tempPath.c_str());
                    }
                    // Validate that the downloaded file actually looks like an image.
                    else if (image_extension_by_magic(tempPath).empty())
                    {
                        UTAG_LOG("[foo_utag] downloaded file is not a recognized image, discarding: %s",
                            pfc::stringcvt::string_utf8_from_wide(tempPath.c_str()).get_ptr());
                        DeleteFileW(tempPath.c_str());
                    }
                    else if (!MoveFileExW(tempPath.c_str(), imagePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    {
                        UTAG_LOG("[foo_utag] could not publish downloaded favicon (Win32 error %lu)",
                            static_cast<unsigned long>(GetLastError()));
                        DeleteFileW(tempPath.c_str());
                    }
                    else
                    {
                        UTAG_LOG("[foo_utag] saved favicon: %s",
                            pfc::stringcvt::string_utf8_from_wide(imagePath.c_str()).get_ptr());
                        notify_utag_artwork_changed(utagPath);

                    }
                }
                else
                {
                    UTAG_LOG("[foo_utag] favicon download failed (HRESULT 0x%08X)",
                        static_cast<unsigned>(hr));
                    DeleteFileW(tempPath.c_str());
                }
            }
            catch (...)
            {
                DeleteFileW(tempPath.c_str());
            }

            {
                std::lock_guard<std::mutex> lock(g_faviconMutex);
                g_faviconInFlight.erase(imagePath);
            }
            if (SUCCEEDED(comResult)) CoUninitialize();
            }).detach();
    }
    catch (...)
    {
        // Scheduling failure must never prevent station playback.
    }
}
