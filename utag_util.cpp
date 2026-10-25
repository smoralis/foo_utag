#include "utag_common.h"

// -----------------------------------------------------------------------------
// Path helpers
// -----------------------------------------------------------------------------

std::wstring utf8_to_wide(const std::string& text);

std::string to_native_windows_path(const std::string& path)
{
    constexpr char prefix[] = "file://";
    constexpr char relativePrefix[] = "file-relative://";

    // file-relative:// is a foobar2000 path scheme, not a Win32 path.
    // Canonicalizing it with g_get_canonical_path() can leave the scheme
    // intact, which makes URLDownloadToFileW fail with E_ABORT. Strip the
    // scheme and ask Win32 to resolve the remaining relative path against the
    // process working directory (foobar2000's file-relative base).
    if (path.compare(0, sizeof(relativePrefix) - 1, relativePrefix) == 0)
    {
        std::wstring relative = utf8_to_wide(
            path.substr(sizeof(relativePrefix) - 1));
        std::replace(relative.begin(), relative.end(), L'/', L'\\');

        const DWORD needed = GetFullPathNameW(relative.c_str(), 0, nullptr, nullptr);
        if (needed == 0)
            throw exception_io_data();

        std::wstring absolute(static_cast<size_t>(needed), L'\0');
        const DWORD written = GetFullPathNameW(
            relative.c_str(), needed, absolute.data(), nullptr);
        if (written == 0 || written >= needed)
            throw exception_io_data();

        absolute.resize(written);
        return pfc::stringcvt::string_utf8_from_wide(absolute.c_str()).get_ptr();
    }

    if (path.compare(0, sizeof(prefix) - 1, prefix) == 0)
        return path.substr(sizeof(prefix) - 1);

    return path;
}

std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty())
        return {};

    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        text.c_str(), -1, nullptr, 0
    );

    if (count <= 0)
        throw exception_io_data();

    std::wstring result(static_cast<size_t>(count), L'\0');

    if (MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        text.c_str(), -1, result.data(), count
    ) <= 0)
    {
        throw exception_io_data();
    }

    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::string basename_from_path(const std::string& path)
{
    const std::string nativePath = to_native_windows_path(path);
    const auto pos = nativePath.find_last_of("/\\");

    return pos == std::string::npos
        ? nativePath
        : nativePath.substr(pos + 1);
}

// -----------------------------------------------------------------------------
// JSON helpers
// -----------------------------------------------------------------------------

std::string lowercase_ascii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    return value;
}

bool is_reserved_key(const std::string& key)
{
    const std::string lower = lowercase_ascii(key);

    return lower == "@";
}

bool is_standard_tag_key(const std::string& key)
{
    static const char* const keys[] = {
        "title",
        "artist",
        "album",
        "album artist",
        "album_artist",
        "date",
        "year",
        "genre",
        "tracknumber",
        "track number",
        "discnumber",
        "disc number",
        "composer",
        "performer",
        "comment",
        "copyright",
        "publisher",
        "label",
        "lyrics",
        "bpm",
        "rating",
        "replaygain_track_gain",
        "replaygain_track_peak",
        "replaygain_album_gain",
        "replaygain_album_peak",
        "totaltracks",
        "totaldiscs",
        "tracktotal",
        "disctotal"
    };

    const std::string lower = lowercase_ascii(key);

    for (const auto* candidate : keys)
    {
        if (lower == candidate)
            return true;
    }

    return false;
}

std::string json_value_to_string(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();

    if (value.is_null())
        return {};

    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";

    if (value.is_number())
        return value.dump();

    return value.dump();
}

double json_duration_to_seconds(const json& value)
{
    try
    {
        if (value.is_number())
        {
            const double result = value.get<double>();

            return std::isfinite(result) && result > 0.0
                ? result
                : 0.0;
        }

        if (value.is_string())
        {
            const std::string text = value.get<std::string>();
            size_t consumed = 0;
            const double result = std::stod(text, &consumed);

            if (consumed == text.size() &&
                std::isfinite(result) &&
                result > 0.0)
            {
                return result;
            }
        }
    }
    catch (...)
    {
    }

    return 0.0;
}

bool is_local_file_reference(const std::string& url)
{
    // file:// references, including file://C:\Music\track.m4a
    if (url.size() >= 7) {
        const char prefix[] = "file://";
        bool matches = true;

        for (size_t i = 0; i < 7; ++i) {
            char c = url[i];

            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }

            if (c != prefix[i]) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return true;
        }
    }

    // Windows drive paths, e.g. C:\Music\track.m4a
    if (url.size() >= 3) {
        const char drive = url[0];

        const bool is_drive_letter =
            (drive >= 'A' && drive <= 'Z') ||
            (drive >= 'a' && drive <= 'z');

        if (is_drive_letter &&
            url[1] == ':' &&
            (url[2] == '\\' || url[2] == '/')) {
            return true;
        }
    }

    // UNC paths, e.g. \\server\share\track.m4a
    if (url.size() >= 2 &&
        url[0] == '\\' &&
        url[1] == '\\') {
        return true;
    }

    return false;
}

// True when a reference points at another .utag stub. Such a reference would
// make the input (or the artwork extractor/editor) open itself recursively.
bool reference_is_utag(const std::string& ref)
{
    return ref.size() >= 5 &&
        _stricmp(ref.c_str() + ref.size() - 5, ".utag") == 0;
}

// Convert a Windows wide string to UTF-8 for readable result logs.
std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), count, nullptr, nullptr) <= 0)
        return {};
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

// CON, PRN, AUX, NUL, COM1-9, LPT1-9 are device names on Windows, with or
// without an extension: "NUL.utag" would silently write nowhere.
static bool is_reserved_windows_name(const std::wstring& name)
{
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ')
        stem.pop_back();

    static const wchar_t* const reserved[] = { L"CON", L"PRN", L"AUX", L"NUL" };
    for (const wchar_t* r : reserved)
        if (_wcsicmp(stem.c_str(), r) == 0) return true;

    return stem.size() == 4 &&
        (_wcsnicmp(stem.c_str(), L"COM", 3) == 0 ||
            _wcsnicmp(stem.c_str(), L"LPT", 3) == 0) &&
        stem[3] >= L'1' && stem[3] <= L'9';
}

std::wstring make_safe_filename_base(const std::string& label)
{
    std::wstring result = utf8_to_wide(label);

    for (auto& ch : result)
    {
        if (ch < 32 || ch == L'<' || ch == L'>' ||
            ch == L':' || ch == L'"' || ch == L'/' ||
            ch == L'\\' || ch == L'|' || ch == L'?' ||
            ch == L'*')
        {
            ch = L'_';
        }
    }

    while (!result.empty() &&
        (result.back() == L'.' || result.back() == L' '))
    {
        result.pop_back();
    }

    if (result.empty())
        result = L"Station";

    if (result.size() >= 5 &&
        _wcsicmp(result.c_str() + result.size() - 5, L".utag") == 0)
    {
        result.resize(result.size() - 5);
    }

    if (result.empty())
        result = L"Station";

    if (is_reserved_windows_name(result))
        result.insert(0, L"_");

    return result;
}

bool is_utag_path(const std::string& path)
{
    return path.size() >= 5 &&
        _stricmp(path.c_str() + path.size() - 5, ".utag") == 0;
}

std::string percent_encode(const std::string& text)
{
    static const char* const hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

int compare_names(const std::string& a, const std::string& b)
{
    try
    {
        const std::wstring wa = utf8_to_wide(a);
        const std::wstring wb = utf8_to_wide(b);

        const int result = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
            wa.c_str(), static_cast<int>(wa.size()),
            wb.c_str(), static_cast<int>(wb.size()));

        if (result == CSTR_LESS_THAN)
            return -1;
        if (result == CSTR_GREATER_THAN)
            return 1;
        if (result == CSTR_EQUAL)
            return 0;
    }
    catch (...)
    {
    }

    return a < b ? -1 : (a > b ? 1 : 0); // fallback: byte order
}
