#include "utag_common.h"

// Extracts the saved favicon as front-cover artwork when available. Favicon
// downloading is asynchronous, so an initial artwork query may happen before the
// sidecar exists; later queries can use it. If it is not present, preserve the
// referenced-file fallback. The built-in viewer may cache artwork and may need
// its own refresh/track-change before it asks again.
class utag_album_art_extractor_instance : public album_art_extractor_instance
{
public:
    utag_album_art_extractor_instance(std::string imagePath, std::string reference)
        : m_imagePath(std::move(imagePath)), m_reference(std::move(reference)) {
    }

    album_art_data_ptr query(const GUID& what, abort_callback& abort) override
    {
        abort.check();
        if (what == album_art_ids::cover_front && !m_imagePath.empty())
        {
            try
            {
                service_ptr_t<file> imageFile;
                filesystem::g_open_read(imageFile, m_imagePath.c_str(), abort);
                const t_filesize size = imageFile->get_size_ex(abort);
                if (size > 0 && size <= 64 * 1024 * 1024)
                    return album_art_data_impl::g_create(
                        imageFile.get_ptr(), pfc::downcast_guarded<t_size>(size), abort);
            }
            catch (const exception_io&)
            {
                // Fall back to the referenced media file if the sidecar is absent/unreadable.
            }
        }

        // Only local, non-.utag references can hold embedded artwork. Remote
        // streams have no length ("Length of object is unknown").
        if (!is_local_file_reference(m_reference) || reference_is_utag(m_reference))
            throw exception_album_art_not_found();

        try
        {
            auto fallback = album_art_extractor::g_open(nullptr, m_reference.c_str(), abort);
            return fallback->query(what, abort);
        }
        catch (const exception_album_art_not_found&)
        {
            throw;
        }
        catch (const exception_io&)
        {
            // Unreadable/unsized referenced item: report "no artwork" rather
            // than an unexpected error in the artwork viewer.
            throw exception_album_art_not_found();
        }
    }

private:
    std::string m_imagePath;
    std::string m_reference;
};

class utag_album_art_extractor : public album_art_extractor_v2
{
public:
    bool is_our_path(const char* path, const char* extension) override
    {
        (void)path;
        return extension != nullptr && pfc::stricmp_ascii(extension, "utag") == 0;
    }

    album_art_extractor_instance_ptr open(
        file_ptr fileHint, const char* path, abort_callback& abort) override
    {
        (void)fileHint;
        if (path == nullptr || *path == '\0') throw exception_io_data();

        service_ptr_t<file> file;
        filesystem::g_open_read(file, path, abort);
        const t_filesize size = file->get_size_ex(abort);
        if (size == 0 || size > kMaxStubSize) throw exception_io_data();

        std::string buffer(static_cast<size_t>(size), '\0');
        file->seek(0, abort);
        file->read_object(buffer.data(), static_cast<t_size>(buffer.size()), abort);
        file.release();

        if (buffer.size() >= 3 &&
            static_cast<unsigned char>(buffer[0]) == 0xEF &&
            static_cast<unsigned char>(buffer[1]) == 0xBB &&
            static_cast<unsigned char>(buffer[2]) == 0xBF)
            buffer.erase(0, 3);

        json parsed = json::parse(buffer, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array() ||
            parsed.size() != 1 || !parsed[0].is_object())
            throw exception_io_data();

        const auto refIt = parsed[0].find("@");
        if (refIt == parsed[0].end() || !refIt->is_string() ||
            refIt->get<std::string>().empty())
            throw exception_album_art_not_found();

        const std::string reference = to_native_windows_path(refIt->get<std::string>());
        std::string imagePath;
        const auto faviconIt = parsed[0].find("favicon");
        if (faviconIt != parsed[0].end() && faviconIt->is_string())
        {
            const std::string url = faviconIt->get<std::string>();
            if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
            {
                const std::string extension = favicon_extension_for_url(url);

                std::string nativePath = to_native_windows_path(path);
                const size_t pathSlash = nativePath.find_last_of("\\/");
                const size_t pathDot = nativePath.find_last_of('.');
                if (pathDot != std::string::npos &&
                    (pathSlash == std::string::npos || pathDot > pathSlash))
                {
                    imagePath = nativePath.substr(0, pathDot) + extension;
                    const std::wstring wideImage = utf8_to_wide(imagePath);
                    const DWORD attrs = GetFileAttributesW(wideImage.c_str());
                    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
                        imagePath.clear();
                }
            }
        }

        abort.check();
        return new service_impl_t<utag_album_art_extractor_instance>(
            std::move(imagePath), reference);
    }

    GUID get_guid() override { return g_utag_guid; }
};

static service_factory_single_t<utag_album_art_extractor>
g_utag_album_art_extractor_factory;

// Exposes the artwork editor for the audio file referenced by a .utag file.
// The artwork is edited in the referenced audio file, not in the .utag JSON.
class utag_album_art_editor : public album_art_editor_v2
{
public:
    bool is_our_path(
        const char* path,
        const char* extension) override
    {
        (void)path;

        return extension != nullptr
            && pfc::stricmp_ascii(extension, "utag") == 0;
    }

    album_art_editor_instance_ptr open(
        file_ptr fileHint,
        const char* path,
        abort_callback& abort) override
    {
        (void)fileHint;

        if (path == nullptr || *path == '\0') {
            throw exception_io_data();
        }

        // Read the .utag JSON file.
        service_ptr_t<file> input;
        filesystem::g_open_read(input, path, abort);

        const t_filesize fileSize = input->get_size_ex(abort);

        if (fileSize == 0 || fileSize > kMaxStubSize) {
            throw exception_io_data();
        }

        std::string buffer(
            static_cast<size_t>(fileSize), '\0');

        input->seek(0, abort);
        input->read_object(
            buffer.data(),
            static_cast<t_size>(buffer.size()),
            abort);

        input.release();

        // Accept a UTF-8 BOM if present.
        if (buffer.size() >= 3
            && static_cast<unsigned char>(buffer[0]) == 0xEF
            && static_cast<unsigned char>(buffer[1]) == 0xBB
            && static_cast<unsigned char>(buffer[2]) == 0xBF)
        {
            buffer.erase(0, 3);
        }

        // Validate the existing .utag structure:
        // [ { "@": "file://..." } ]
        json parsed = json::parse(buffer, nullptr, false);

        if (parsed.is_discarded()
            || !parsed.is_array()
            || parsed.size() != 1
            || !parsed[0].is_object())
        {
            throw exception_io_data();
        }

        const auto it = parsed[0].find("@");

        if (it == parsed[0].end()
            || !it->is_string()
            || it->get_ref<const std::string&>().empty())
        {
            throw exception_io_data();
        }

        // Resolve the referenced audio file.
        const std::string reference =
            to_native_windows_path(
                it->get_ref<const std::string&>());

        abort.check();

        // Remote streams have no length and cannot hold editable artwork.
        if (!is_local_file_reference(reference) || reference_is_utag(reference)) {
            throw exception_io_data();
        }

        // Delegate to the registered editor for the actual audio format.
        // This may throw if that format has no artwork editor.
        try {
            return album_art_editor::g_open(
                nullptr, reference.c_str(), abort);
        }
        catch (const exception_io&) {
            throw exception_io_data();
        }
    }

    GUID get_guid() override
    {
        return g_utag_guid;
    }
};

static service_factory_single_t<utag_album_art_editor>
g_utag_album_art_editor_factory;
