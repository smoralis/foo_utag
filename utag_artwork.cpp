#include "utag_ui.h"

#include <cstdio>
#include <wincodec.h>
#include <ole2.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "UxTheme.lib")

// -----------------------------------------------------------------------------
// Manually assigned station logos.
//
// A station's artwork is the "sidecar" image beside its .utag file (same folder,
// same basename). The favicon downloader writes that file; this file lets the
// user put one there by hand, so a station can have a logo even when it has no
// "favicon" URL, or when the URL is wrong. Both end up as the same kind of file
// and are read by the same artwork extractor, so nothing else needs to know
// where a logo came from.
//
//   set_station_logo_from_file()  validate, copy beside the .utag, replace any
//                                 older sidecar of the station
//   remove_station_logo()         delete the station's sidecar image(s)
//   utag_open_logo_dialog()       modeless window with a drop zone (image files
//                                 and image URLs), Browse, favicon and Remove
// -----------------------------------------------------------------------------

namespace
{
    constexpr ULONGLONG kMaxLogoSize = 32ull * 1024 * 1024;

    std::string win32_error_text(DWORD error)
    {
        wchar_t* buffer = nullptr;
        const DWORD len = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

        std::string text = "Win32 error " + std::to_string(error);
        if (len != 0 && buffer != nullptr)
        {
            std::wstring wide(buffer, len);
            while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' ||
                wide.back() == L' ' || wide.back() == L'.'))
                wide.pop_back();
            try { text = wide_to_utf8(wide); }
            catch (...) {}
        }
        if (buffer != nullptr) LocalFree(buffer);
        return text;
    }

    // Reads the station object of a .utag file (nothing is written back).
    bool read_station_json(const std::string& utagPath, json& station)
    {
        try
        {
            const std::wstring wide = utf8_to_wide(to_native_windows_path(utagPath));
            HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;

            LARGE_INTEGER size = {};
            if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
                static_cast<ULONGLONG>(size.QuadPart) > kMaxStubSize)
            {
                CloseHandle(h);
                return false;
            }

            std::string buffer(static_cast<size_t>(size.QuadPart), '\0');
            DWORD got = 0;
            const BOOL ok = ReadFile(h, buffer.data(),
                static_cast<DWORD>(buffer.size()), &got, nullptr);
            CloseHandle(h);
            if (!ok || got != buffer.size()) return false;

            if (buffer.size() >= 3 &&
                static_cast<unsigned char>(buffer[0]) == 0xEF &&
                static_cast<unsigned char>(buffer[1]) == 0xBB &&
                static_cast<unsigned char>(buffer[2]) == 0xBF)
                buffer.erase(0, 3);

            json parsed = json::parse(buffer, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array() ||
                parsed.size() != 1 || !parsed[0].is_object())
                return false;

            station = std::move(parsed[0]);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::string json_string_field(const json& station, const char* key)
    {
        const auto it = station.find(key);
        if (it != station.end() && it->is_string())
            return it->get<std::string>();
        return {};
    }

    // "Station name" for the window: the tag if present, else the file name.
    std::string station_label(const std::string& utagPath)
    {
        json station;
        if (read_station_json(utagPath, station))
        {
            std::string name = json_string_field(station, "name");
            if (name.empty()) name = json_string_field(station, "display_name");
            if (!name.empty()) return name;
        }

        std::string base = basename_from_path(utagPath);
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos && dot > 0) base.erase(dot);
        return base;
    }

    bool is_http_url(const std::string& url)
    {
        return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    }

    // Downloads an image URL into a temporary file and checks that it really is
    // an image. On success `tempFile` must be deleted by the caller.
    bool fetch_image_to_temp(const std::string& url, std::wstring& tempFile, std::string& error)
    {
        if (!is_http_url(url))
        {
            error = "Only http:// and https:// image addresses are supported.";
            return false;
        }

        wchar_t dir[MAX_PATH + 1] = {};
        wchar_t file[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, dir) == 0 || GetTempFileNameW(dir, L"utl", 0, file) == 0)
        {
            error = "Could not create a temporary file.";
            return false;
        }

        HRESULT hr = E_FAIL;
        try
        {
            hr = URLDownloadToFileW(nullptr, utf8_to_wide(url).c_str(), file, 0, nullptr);
        }
        catch (...)
        {
        }

        if (FAILED(hr))
        {
            DeleteFileW(file);
            char text[64];
            std::snprintf(text, sizeof(text), "Download failed (HRESULT 0x%08X).",
                static_cast<unsigned>(hr));
            error = text;
            return false;
        }

        if (image_extension_by_magic(file).empty())
        {
            DeleteFileW(file);
            error = "The address did not return a supported image.";
            return false;
        }

        tempFile = file;
        return true;
    }
}

// -----------------------------------------------------------------------------
// Public logic (no UI)
// -----------------------------------------------------------------------------

bool set_station_logo_from_file(const std::string& utagPath,
    const std::wstring& imageFile, std::string& error)
{
    const std::wstring stem = station_artwork_stem(utagPath);
    if (stem.empty())
    {
        error = "The station file has no usable path.";
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(imageFile.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        error = "The image file could not be found.";
        return false;
    }

    const ULONGLONG size =
        (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size == 0 || size > kMaxLogoSize)
    {
        error = "The image is empty or larger than 32 MB.";
        return false;
    }

    // The sidecar is named after what the file really is, not what it is called.
    const std::wstring ext = image_extension_by_magic(imageFile);
    if (ext.empty())
    {
        error = "Not a supported image (PNG, JPEG, GIF, BMP, WebP or SVG).";
        return false;
    }

    const std::wstring target = stem + ext;

    // Dropping the station's own sidecar onto itself is fine, just nothing to copy.
    if (_wcsicmp(imageFile.c_str(), target.c_str()) != 0)
    {
        // Copy to a temporary name first so artwork readers never see a
        // half-written image.
        const std::wstring temp = target + L".logo.tmp";
        DeleteFileW(temp.c_str());
        if (!CopyFileW(imageFile.c_str(), temp.c_str(), FALSE))
        {
            error = "Could not copy the image: " + win32_error_text(GetLastError());
            return false;
        }
        if (!MoveFileExW(temp.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD last = GetLastError();
            DeleteFileW(temp.c_str());
            error = "Could not save the logo: " + win32_error_text(last);
            return false;
        }
    }

    // One sidecar per station: remove older images with other extensions (but
    // never the file the user just chose, even if it is misnamed).
    const std::wstring wideSource = imageFile;
    {
        const std::wstring keepTarget = target;
        const std::wstring stemCopy = stem;
        static const wchar_t* const exts[] = {
            L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg" };
        for (const wchar_t* e : exts)
        {
            const std::wstring candidate = stemCopy + e;
            if (_wcsicmp(candidate.c_str(), keepTarget.c_str()) == 0) continue;
            if (_wcsicmp(candidate.c_str(), wideSource.c_str()) == 0) continue;
            DeleteFileW(candidate.c_str());
        }
    }

    UTAG_LOG("[foo_utag] station logo set: %s",
        pfc::stringcvt::string_utf8_from_wide(target.c_str()).get_ptr());
    notify_utag_artwork_changed(utagPath);
    return true;
}

bool remove_station_logo(const std::string& utagPath, std::string& error)
{
    if (find_station_artwork(utagPath).empty())
    {
        error = "The station has no logo image.";
        return false;
    }

    delete_station_artwork_except(utagPath, std::wstring());

    if (!find_station_artwork(utagPath).empty())
    {
        error = "The logo image could not be deleted (is it read-only or in use?).";
        return false;
    }

    UTAG_LOG("[foo_utag] station logo removed: %s", utagPath.c_str());
    notify_utag_artwork_changed(utagPath);
    return true;
}

// -----------------------------------------------------------------------------
// "Station logo" window
// -----------------------------------------------------------------------------

namespace
{
    constexpr int kIdHeader = 1001;
    constexpr int kIdPreview = 1002;
    constexpr int kIdBrowse = 1004;
    constexpr int kIdFavicon = 1005;
    constexpr int kIdStatus = 1003;  // placeholder in the template, replaced by the list
    constexpr int kIdResults = 1008; // results list view
    constexpr int kIdRemove = 1006;
    constexpr int kIdHomepage = 1007;

    constexpr UINT kMsgDrop = WM_APP + 1; // LPARAM: drop_payload* (heap)
    constexpr UINT kMsgDone = WM_APP + 2; // LPARAM: std::string* (heap), status text

    struct logo_target
    {
        std::string utagPath; // foobar2000 path of the .utag
        std::string label;
    };

    // What the drop target extracted from the dropped data object.
    struct drop_payload
    {
        std::vector<std::wstring> files;
        std::wstring url;
    };

    template <class T>
    struct com_ptr
    {
        T* p = nullptr;
        com_ptr() = default;
        com_ptr(const com_ptr&) = delete;
        com_ptr& operator=(const com_ptr&) = delete;
        ~com_ptr() { if (p) p->Release(); }
        T** operator&() { return &p; }
        T* operator->() const { return p; }
        explicit operator bool() const { return p != nullptr; }
    };

    // Decodes an image to a bitmap that fits in maxW x maxH (premultiplied
    // BGRA). Returns nullptr when Windows cannot decode the format (e.g. SVG,
    // or WebP without the Windows WebP extension); the logo itself is still
    // saved and works as artwork, only the preview is missing.
    HBITMAP load_preview_bitmap(const std::wstring& path, int maxW, int maxH, int& outW, int& outH)
    {
        outW = outH = 0;
        if (maxW < 8 || maxH < 8) return nullptr;

        com_ptr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return nullptr;

        com_ptr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
            return nullptr;

        com_ptr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame)))
            return nullptr;

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
            return nullptr;

        // Fit into the box; small images may be enlarged up to 4x.
        double scale = (std::min)(static_cast<double>(maxW) / w, static_cast<double>(maxH) / h);
        scale = (std::min)(scale, 4.0);
        const UINT tw = (std::max)(1u, static_cast<UINT>(w * scale));
        const UINT th = (std::max)(1u, static_cast<UINT>(h * scale));

        com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.p, tw, th, WICBitmapInterpolationModeFant)))
            return nullptr;

        com_ptr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(scaler.p, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            return nullptr;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(tw);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(th); // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr)
        {
            if (bitmap) DeleteObject(bitmap);
            return nullptr;
        }

        if (FAILED(converter->CopyPixels(nullptr, tw * 4, tw * th * 4,
            static_cast<BYTE*>(bits))))
        {
            DeleteObject(bitmap);
            return nullptr;
        }

        outW = static_cast<int>(tw);
        outH = static_cast<int>(th);
        return bitmap;
    }

    struct logo_state
    {
        // Follows foobar2000's own dark mode setting (no-op on pre-2.0 builds).
        fb2k::CCoreDarkModeHooks darkHooks;

        std::vector<logo_target> targets;

        HBITMAP preview = nullptr;
        int previewW = 0, previewH = 0;
        bool haveLogo = false;     // the first target has a sidecar image
        bool hover = false;        // something is being dragged over the window
        bool busy = false;         // a download is running
        bool oleInitialized = false;
        IDropTarget* dropTarget = nullptr;
    };

    HWND g_logoWnd = nullptr; // the one open window, if any

    // -------------------------------------------------------------------------
    // Drop target: image files (CF_HDROP) and image URLs (browser drags).
    // -------------------------------------------------------------------------
    class logo_drop_target : public IDropTarget
    {
    public:
        explicit logo_drop_target(HWND dlg) : m_dlg(dlg) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv == nullptr) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IDropTarget)
            {
                *ppv = static_cast<IDropTarget*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }

        STDMETHODIMP_(ULONG) Release() override
        {
            const LONG count = InterlockedDecrement(&m_ref);
            if (count == 0) delete this;
            return static_cast<ULONG>(count);
        }

        STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
        {
            m_accept = can_accept(data);
            *effect = m_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            set_hover(m_accept);
            return S_OK;
        }

        STDMETHODIMP DragOver(DWORD, POINTL, DWORD* effect) override
        {
            *effect = m_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

        STDMETHODIMP DragLeave() override
        {
            set_hover(false);
            return S_OK;
        }

        STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override
        {
            set_hover(false);
            *effect = DROPEFFECT_NONE;
            if (data == nullptr) return S_OK;

            auto* payload = new drop_payload;
            extract(data, *payload);
            if (payload->files.empty() && payload->url.empty())
            {
                delete payload;
                return S_OK;
            }

            // Handled after the drag loop has ended, so the source application
            // (Explorer, a browser) is not blocked while we copy or download.
            if (!PostMessageW(m_dlg, kMsgDrop, 0, reinterpret_cast<LPARAM>(payload)))
            {
                delete payload;
                return S_OK;
            }
            *effect = DROPEFFECT_COPY;
            return S_OK;
        }

    private:
        ~logo_drop_target() = default;

        static UINT url_format()
        {
            static const UINT fmt = RegisterClipboardFormatW(L"UniformResourceLocatorW");
            return fmt;
        }

        static bool has_format(IDataObject* data, UINT format)
        {
            FORMATETC fmt = { static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            return data->QueryGetData(&fmt) == S_OK;
        }

        static bool can_accept(IDataObject* data)
        {
            return data != nullptr &&
                (has_format(data, CF_HDROP) ||
                    has_format(data, url_format()) ||
                    has_format(data, CF_UNICODETEXT));
        }

        static bool get_hglobal(IDataObject* data, UINT format, STGMEDIUM& medium)
        {
            FORMATETC fmt = { static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            return SUCCEEDED(data->GetData(&fmt, &medium)) && medium.hGlobal != nullptr;
        }

        static std::wstring first_http_line(const wchar_t* text, size_t maxChars)
        {
            std::wstring value;
            for (size_t i = 0; i < maxChars && text[i] != L'\0' &&
                text[i] != L'\r' && text[i] != L'\n'; ++i)
                value.push_back(text[i]);

            while (!value.empty() && value.back() == L' ') value.pop_back();
            while (!value.empty() && value.front() == L' ') value.erase(value.begin());

            if (value.rfind(L"http://", 0) == 0 || value.rfind(L"https://", 0) == 0)
                return value;
            return {};
        }

        static void extract(IDataObject* data, drop_payload& payload)
        {
            STGMEDIUM medium = {};

            if (has_format(data, CF_HDROP) && get_hglobal(data, CF_HDROP, medium))
            {
                HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
                if (drop != nullptr)
                {
                    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < count; ++i)
                    {
                        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                        if (len == 0) continue;
                        std::wstring path(static_cast<size_t>(len) + 1, L'\0');
                        const UINT got = DragQueryFileW(drop, i, path.data(), len + 1);
                        path.resize(got);
                        if (!path.empty()) payload.files.push_back(std::move(path));
                    }
                    GlobalUnlock(medium.hGlobal);
                }
                ReleaseStgMedium(&medium);
            }

            if (!payload.files.empty()) return;

            for (const UINT format : { url_format(), static_cast<UINT>(CF_UNICODETEXT) })
            {
                medium = {};
                if (!has_format(data, format) || !get_hglobal(data, format, medium))
                    continue;

                const SIZE_T bytes = GlobalSize(medium.hGlobal);
                const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
                if (text != nullptr)
                {
                    payload.url = first_http_line(text, bytes / sizeof(wchar_t));
                    GlobalUnlock(medium.hGlobal);
                }
                ReleaseStgMedium(&medium);

                if (!payload.url.empty()) return;
            }
        }

        void set_hover(bool hover)
        {
            auto* state = reinterpret_cast<logo_state*>(
                GetWindowLongPtrW(m_dlg, DWLP_USER));
            if (state == nullptr || state->hover == hover) return;
            state->hover = hover;
            InvalidateRect(GetDlgItem(m_dlg, kIdPreview), nullptr, FALSE);
        }

        LONG m_ref = 1;
        HWND m_dlg;
        bool m_accept = false;
    };

    // -------------------------------------------------------------------------
    // Window logic
    // -------------------------------------------------------------------------

    // Report format used between the worker, the handlers and log_result: lines
    // separated by "\r\n"; a line is either "text" or "station<US>text" where
    // <US> is the unit separator character (0x1F), which cannot occur in names.
    constexpr char kSep = '\x1f';

    std::string station_line(const std::string& station, const std::string& text)
    {
        return station + kSep + text;
    }

    std::string join_lines(const std::string& report, const std::string& line)
    {
        return report.empty() ? line : report + "\r\n" + line;
    }

    // Adds the lines of `report` as rows of the results list and writes them to
    // the foobar2000 console. Every message goes to the console, whatever the
    // "Write messages to the console" option says.
    void log_result(HWND dlg, const std::string& report)
    {
        const HWND list = GetDlgItem(dlg, kIdResults);

        size_t start = 0;
        while (start <= report.size())
        {
            size_t end = report.find("\r\n", start);
            if (end == std::string::npos) end = report.size();

            std::string station;
            std::string text = report.substr(start, end - start);
            const size_t sep = text.find(kSep);
            if (sep != std::string::npos)
            {
                station = text.substr(0, sep);
                text = text.substr(sep + 1);
            }

            if (!text.empty())
            {
                console::printf("[foo_utag] station logo: %s%s%s",
                    station.c_str(), station.empty() ? "" : ": ", text.c_str());

                if (list != nullptr)
                {
                    try
                    {
                        const std::wstring wideStation = utf8_to_wide(station);
                        const std::wstring wideText = utf8_to_wide(text);

                        // Keep the list bounded.
                        while (ListView_GetItemCount(list) >= 500)
                            ListView_DeleteItem(list, 0);

                        LVITEMW item = {};
                        item.mask = LVIF_TEXT;
                        item.iItem = ListView_GetItemCount(list);
                        item.pszText = const_cast<wchar_t*>(wideStation.c_str());
                        const int index = static_cast<int>(
                            SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
                        if (index >= 0)
                        {
                            ListView_SetItemText(list, index, 1,
                                const_cast<wchar_t*>(wideText.c_str()));
                            ListView_EnsureVisible(list, index, FALSE);
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }

            if (end >= report.size()) break;
            start = end + 2;
        }
    }

    // True when foobar2000's dark mode hooks make the dialog dark: ask the
    // dialog what text color it hands out for static controls.
    bool dialog_is_dark(HWND dlg)
    {
        const HDC dc = GetDC(dlg);
        if (dc == nullptr) return false;

        SendMessageW(dlg, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
            reinterpret_cast<LPARAM>(GetDlgItem(dlg, kIdHeader)));
        const COLORREF text = GetTextColor(dc);
        ReleaseDC(dlg, dc);

        const int luminance = (GetRValue(text) * 299 + GetGValue(text) * 587 +
            GetBValue(text) * 114) / 1000;
        return luminance > 128;
    }

    // In dark mode the Windows header theme keeps drawing black column titles.
    // The header sends NM_CUSTOMDRAW to its parent, the list view, so the list
    // is subclassed and the header is painted here: dark background, light text.
    LRESULT CALLBACK dark_header_subclass(HWND list, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR)
    {
        if (msg == WM_NOTIFY)
        {
            const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lp);
            const HWND header = ListView_GetHeader(list);
            if (hdr != nullptr && header != nullptr &&
                hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == header)
            {
                const NMCUSTOMDRAW* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lp);

                if (cd->dwDrawStage == CDDS_PREPAINT)
                {
                    // Also covers the empty area right of the last column.
                    const HBRUSH background = CreateSolidBrush(RGB(45, 45, 45));
                    FillRect(cd->hdc, &cd->rc, background);
                    DeleteObject(background);
                    return CDRF_NOTIFYITEMDRAW;
                }

                if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    RECT rc = cd->rc;

                    const bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
                    const HBRUSH background = CreateSolidBrush(
                        pressed ? RGB(64, 64, 64) : RGB(45, 45, 45));
                    FillRect(cd->hdc, &rc, background);
                    DeleteObject(background);

                    // Column divider on the right, line under the header.
                    const HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
                    const HGDIOBJ oldPen = SelectObject(cd->hdc, pen);
                    MoveToEx(cd->hdc, rc.right - 1, rc.top + 2, nullptr);
                    LineTo(cd->hdc, rc.right - 1, rc.bottom - 2);
                    MoveToEx(cd->hdc, rc.left, rc.bottom - 1, nullptr);
                    LineTo(cd->hdc, rc.right, rc.bottom - 1);
                    SelectObject(cd->hdc, oldPen);
                    DeleteObject(pen);

                    wchar_t text[128] = {};
                    HDITEMW item = {};
                    item.mask = HDI_TEXT;
                    item.pszText = text;
                    item.cchTextMax = static_cast<int>(std::size(text));
                    SendMessageW(header, HDM_GETITEMW, cd->dwItemSpec,
                        reinterpret_cast<LPARAM>(&item));

                    HFONT font = reinterpret_cast<HFONT>(SendMessageW(header, WM_GETFONT, 0, 0));
                    if (font == nullptr)
                        font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
                    const HGDIOBJ oldFont = font ? SelectObject(cd->hdc, font) : nullptr;

                    SetBkMode(cd->hdc, TRANSPARENT);
                    SetTextColor(cd->hdc, RGB(228, 228, 228));
                    rc.left += 6;
                    rc.right -= 6;
                    DrawTextW(cd->hdc, text, -1, &rc,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

                    if (oldFont) SelectObject(cd->hdc, oldFont);
                    return CDRF_SKIPDEFAULT;
                }

                return CDRF_DODEFAULT;
            }
        }
        else if (msg == WM_NCDESTROY)
        {
            RemoveWindowSubclass(list, dark_header_subclass, id);
        }

        return DefSubclassProc(list, msg, wp, lp);
    }

    // The list is deliberately NOT given to foobar2000's dark mode hooks
    // (CCoreDarkModeHooks), which leave a SysListView32 unusable. It is themed
    // here instead: dark scrollbars and header through the Windows dark mode
    // visual styles, and explicit colors for the rows.
    void apply_list_theme(HWND list, bool dark)
    {
        const HWND header = ListView_GetHeader(list);

        if (dark)
        {
            SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
            if (header != nullptr)
            {
                SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
                SetWindowSubclass(list, dark_header_subclass, 1, 0);
            }

            const COLORREF background = RGB(32, 32, 32);
            ListView_SetBkColor(list, background);
            ListView_SetTextBkColor(list, background);
            ListView_SetTextColor(list, RGB(228, 228, 228));
        }
        else
        {
            SetWindowTheme(list, L"Explorer", nullptr);
            if (header != nullptr)
                SetWindowTheme(header, L"Explorer", nullptr);

            ListView_SetBkColor(list, GetSysColor(COLOR_WINDOW));
            ListView_SetTextBkColor(list, GetSysColor(COLOR_WINDOW));
            ListView_SetTextColor(list, GetSysColor(COLOR_WINDOWTEXT));
        }

        InvalidateRect(list, nullptr, TRUE);
        if (header != nullptr) InvalidateRect(header, nullptr, TRUE);
    }

    // Replaces the empty placeholder of the template by a report-style list
    // with a Station and a Result column.
    void create_results_list(HWND dlg)
    {
        const HWND placeholder = GetDlgItem(dlg, kIdStatus);
        if (placeholder == nullptr) return;

        RECT rc = {};
        GetWindowRect(placeholder, &rc);
        MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&rc), 2);
        DestroyWindow(placeholder);

        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);

        const HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
            dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdResults)),
            core_api::get_my_instance(), nullptr);
        if (list == nullptr) return;

        SendMessageW(list, WM_SETFONT, SendMessageW(dlg, WM_GETFONT, 0, 0), TRUE);
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

        const HDC dc = GetDC(dlg);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(dlg, dc);

        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(L"Station");
        column.cx = MulDiv(110, dpi, 96);
        SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));

        column.pszText = const_cast<wchar_t*>(L"Result");
        column.cx = MulDiv(250, dpi, 96);
        SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
        ListView_SetColumnWidth(list, 1, LVSCW_AUTOSIZE_USEHEADER);

        apply_list_theme(list, dialog_is_dark(dlg));
    }

    // The station's "homepage" tag when it is an http(s) address. Only one
    // station selected: with several there is no single page to open.
    std::string station_homepage(const logo_state* state)
    {
        if (state->targets.size() != 1)
            return {};

        json station;
        if (!read_station_json(state->targets[0].utagPath, station))
            return {};

        const std::string homepage = json_string_field(station, "homepage");
        return is_http_url(homepage) ? homepage : std::string();
    }

    void open_homepage(HWND dlg, const logo_state* state)
    {
        const std::string homepage = station_homepage(state);
        if (homepage.empty())
        {
            log_result(dlg, "This station has no homepage address.");
            return;
        }

        try
        {
            const HINSTANCE result = ShellExecuteW(dlg, L"open",
                utf8_to_wide(homepage).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) <= 32)
                log_result(dlg, "Could not open the homepage in your browser.");
        }
        catch (...)
        {
            log_result(dlg, "The homepage address could not be used.");
        }
    }

    void refresh_header(HWND dlg, const logo_state* state)
    {
        std::string text;
        if (state->targets.size() == 1)
            text = "Station: " + state->targets[0].label;
        else
            text = std::to_string(state->targets.size()) +
            " stations selected - a dropped image is used for all of them";
        try
        {
            SetDlgItemTextW(dlg, kIdHeader, utf8_to_wide(text).c_str());
        }
        catch (...)
        {
        }
    }

    void refresh_buttons(HWND dlg, const logo_state* state)
    {
        bool anyFavicon = false;
        for (const auto& target : state->targets)
        {
            json station;
            if (read_station_json(target.utagPath, station) &&
                is_http_url(json_string_field(station, "favicon")))
            {
                anyFavicon = true;
                break;
            }
        }

        EnableWindow(GetDlgItem(dlg, kIdBrowse), !state->busy);
        EnableWindow(GetDlgItem(dlg, kIdFavicon), !state->busy && anyFavicon);
        EnableWindow(GetDlgItem(dlg, kIdHomepage), !station_homepage(state).empty());

        bool anyLogo = false;
        for (const auto& target : state->targets)
        {
            if (!find_station_artwork(target.utagPath).empty())
            {
                anyLogo = true;
                break;
            }
        }
        EnableWindow(GetDlgItem(dlg, kIdRemove), !state->busy && anyLogo);
    }

    // Reloads the preview of the first selected station.
    void refresh_preview(HWND dlg, logo_state* state)
    {
        if (state->preview != nullptr)
        {
            DeleteObject(state->preview);
            state->preview = nullptr;
        }
        state->previewW = state->previewH = 0;
        state->haveLogo = false;

        if (!state->targets.empty())
        {
            const std::wstring path = find_station_artwork(state->targets[0].utagPath);
            if (!path.empty())
            {
                state->haveLogo = true;
                RECT rc = {};
                GetClientRect(GetDlgItem(dlg, kIdPreview), &rc);
                state->preview = load_preview_bitmap(path,
                    (rc.right - rc.left) - 12, (rc.bottom - rc.top) - 12,
                    state->previewW, state->previewH);
            }
        }

        InvalidateRect(GetDlgItem(dlg, kIdPreview), nullptr, TRUE);
        refresh_buttons(dlg, state);
    }

    void draw_preview(HWND dlg, const DRAWITEMSTRUCT* dis, const logo_state* state)
    {
        HDC dc = dis->hDC;
        RECT rc = dis->rcItem;

        // A neutral mid-gray works in light and dark mode and shows logos with
        // transparency properly.
        HBRUSH background = CreateSolidBrush(RGB(88, 88, 88));
        FillRect(dc, &rc, background);
        DeleteObject(background);

        HPEN pen = CreatePen(state->hover ? PS_SOLID : PS_DOT, 1,
            state->hover ? RGB(90, 170, 255) : RGB(160, 160, 160));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        if (state->preview != nullptr)
        {
            HDC memory = CreateCompatibleDC(dc);
            HGDIOBJ oldBitmap = SelectObject(memory, state->preview);
            const int x = rc.left + ((rc.right - rc.left) - state->previewW) / 2;
            const int y = rc.top + ((rc.bottom - rc.top) - state->previewH) / 2;
            BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, x, y, state->previewW, state->previewH,
                memory, 0, 0, state->previewW, state->previewH, blend);
            SelectObject(memory, oldBitmap);
            DeleteDC(memory);
            return;
        }

        HFONT font = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 235));

        const wchar_t* line1 = state->haveLogo
            ? L"Logo saved (no preview for this format)"
            : L"No logo yet";
        const wchar_t* line2 = L"Drop an image file or image URL here";

        RECT first = rc, second = rc;
        first.bottom = (rc.top + rc.bottom) / 2 - 1;
        second.top = (rc.top + rc.bottom) / 2 + 1;
        DrawTextW(dc, line1, -1, &first, DT_SINGLELINE | DT_CENTER | DT_BOTTOM);
        DrawTextW(dc, line2, -1, &second, DT_SINGLELINE | DT_CENTER | DT_TOP);
        if (oldFont) SelectObject(dc, oldFont);
    }

    // Copies one local image file in as the logo of every target.
    void apply_file(HWND dlg, logo_state* state, const std::wstring& file)
    {
        std::string report;
        for (const auto& target : state->targets)
        {
            std::string error;
            const bool ok = set_station_logo_from_file(target.utagPath, file, error);
            report = join_lines(report,
                station_line(target.label, ok ? std::string("Logo set") : error));
        }

        log_result(dlg, report);
        refresh_preview(dlg, state);
    }

    // Downloads an image (for a dropped URL: once for all targets; with an empty
    // url: each target's own "favicon" URL) in the background, then reports back
    // with kMsgDone. The window may be closed meanwhile; nothing here touches it
    // except through PostMessage.
    void start_download(HWND dlg, logo_state* state, const std::string& url)
    {
        state->busy = true;
        refresh_buttons(dlg, state);
        log_result(dlg, "Downloading...");

        std::vector<logo_target> targets = state->targets;
        std::thread([dlg, targets = std::move(targets), url]() {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            std::string report;
            try
            {
                std::wstring shared;
                std::string sharedError;
                bool sharedOk = false;
                if (!url.empty())
                    sharedOk = fetch_image_to_temp(url, shared, sharedError);

                for (const auto& target : targets)
                {
                    std::wstring temp;
                    std::string error;
                    bool have = false;
                    bool ownsTemp = false;

                    if (!url.empty())
                    {
                        have = sharedOk;
                        error = sharedError;
                        temp = shared;
                    }
                    else
                    {
                        json station;
                        const std::string favicon = read_station_json(target.utagPath, station)
                            ? json_string_field(station, "favicon") : std::string();
                        if (favicon.empty())
                            error = "no favicon URL in the station file";
                        else
                        {
                            have = fetch_image_to_temp(favicon, temp, error);
                            ownsTemp = have;
                        }
                    }

                    const bool done = have &&
                        set_station_logo_from_file(target.utagPath, temp, error);
                    report = join_lines(report, station_line(target.label,
                        done ? std::string("Logo set")
                        : (error.empty() ? std::string("Could not set the logo") : error)));

                    if (ownsTemp) DeleteFileW(temp.c_str());
                }

                if (!shared.empty()) DeleteFileW(shared.c_str());

            }
            catch (...)
            {
                report = "Unexpected error while downloading the image.";
            }
            if (SUCCEEDED(com)) CoUninitialize();

            auto* message = new std::string(std::move(report));
            if (!IsWindow(dlg) || !PostMessageW(dlg, kMsgDone, 0, reinterpret_cast<LPARAM>(message)))
                delete message;
            }).detach();
    }

    void handle_drop(HWND dlg, logo_state* state, const drop_payload& payload)
    {
        if (state->busy)
        {
            log_result(dlg, "Please wait for the current download to finish.");
            return;
        }

        for (const auto& file : payload.files)
        {
            if (!image_extension_by_magic(file).empty())
            {
                apply_file(dlg, state, file);
                return;
            }
        }

        if (!payload.files.empty())
        {
            log_result(dlg, "That is not a supported image (PNG, JPEG, GIF, BMP, WebP or SVG).");
            return;
        }

        if (!payload.url.empty())
        {
            try
            {
                start_download(dlg, state, wide_to_utf8(payload.url));
            }
            catch (...)
            {
                log_result(dlg, "That address could not be used.");
            }
        }
    }

    void browse_for_image(HWND dlg, logo_state* state)
    {
        wchar_t file[MAX_PATH * 2] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = dlg;
        ofn.lpstrFilter =
            L"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.svg)\0"
            L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.svg\0"
            L"All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(std::size(file));
        ofn.lpstrTitle = L"Choose a station logo";
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameW(&ofn))
            apply_file(dlg, state, file);
    }

    void remove_logos(HWND dlg, logo_state* state)
    {
        std::string report;
        for (const auto& target : state->targets)
        {
            if (find_station_artwork(target.utagPath).empty())
                continue;
            std::string error;
            const bool ok = remove_station_logo(target.utagPath, error);
            report = join_lines(report,
                station_line(target.label, ok ? std::string("Logo removed") : error));
        }

        if (report.empty())
            report = "No selected station has a logo.";
        log_result(dlg, report);
        refresh_preview(dlg, state);
    }

    INT_PTR CALLBACK logo_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<logo_state*>(GetWindowLongPtrW(dlg, DWLP_USER));

        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<logo_state*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            // Hook the dialog and its controls first; the results list is created
            // afterwards so the hooks never touch it (see apply_list_theme).
            state->darkHooks.AddDialogWithControls(dlg);
            modeless_dialog_manager::g_add(dlg); // Tab / Enter / Esc keys
            create_results_list(dlg);

            refresh_header(dlg, state);
            refresh_preview(dlg, state);

            // OLE is normally up already in foobar2000's UI thread; the call is
            // reference counted, so balance it in WM_NCDESTROY.
            const HRESULT ole = OleInitialize(nullptr);
            state->oleInitialized = SUCCEEDED(ole);
            auto* target = new logo_drop_target(dlg);
            if (SUCCEEDED(RegisterDragDrop(dlg, target)))
                state->dropTarget = target;
            else
                target->Release();

            // Dropping on a child control (the preview) must work too: the
            // children have no drop target of their own, so Windows hands the
            // drop to the dialog.
            if (state->dropTarget == nullptr)
                log_result(dlg, "Drag and drop is not available; use Browse...");

            return TRUE;
        }

        case WM_DRAWITEM:
        {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (dis != nullptr && dis->CtlID == kIdPreview && state != nullptr)
            {
                draw_preview(dlg, dis, state);
                return TRUE;
            }
            break;
        }

        case kMsgDrop:
        {
            std::unique_ptr<drop_payload> payload(reinterpret_cast<drop_payload*>(lp));
            if (state != nullptr && payload)
                handle_drop(dlg, state, *payload);
            return TRUE;
        }

        case kMsgDone:
        {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
            if (state != nullptr)
            {
                state->busy = false;
                if (text) log_result(dlg, *text);
                refresh_preview(dlg, state);
            }
            return TRUE;
        }

        case WM_COMMAND:
        {
            if (state == nullptr) break;
            switch (LOWORD(wp))
            {
            case kIdBrowse:
                if (!state->busy) browse_for_image(dlg, state);
                return TRUE;
            case kIdFavicon:
                if (!state->busy) start_download(dlg, state, std::string());
                return TRUE;
            case kIdRemove:
                if (!state->busy) remove_logos(dlg, state);
                return TRUE;
            case kIdHomepage:
                open_homepage(dlg, state);
                return TRUE;
            case IDCANCEL:
                DestroyWindow(dlg);
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(dlg);
            return TRUE;

        case WM_NCDESTROY:
            modeless_dialog_manager::g_remove(dlg);
            SetWindowLongPtrW(dlg, DWLP_USER, 0);
            if (g_logoWnd == dlg) g_logoWnd = nullptr;
            if (state != nullptr)
            {
                if (state->dropTarget != nullptr)
                {
                    RevokeDragDrop(dlg);
                    state->dropTarget->Release();
                    state->dropTarget = nullptr;
                }
                if (state->oleInitialized) OleUninitialize();
                if (state->preview != nullptr) DeleteObject(state->preview);
                delete state;
            }
            break;

        default:
            break;
        }

        return FALSE;
    }
}

void utag_open_logo_dialog(HWND owner, const std::vector<std::string>& utagPaths)
{
    if (utagPaths.empty())
        return;

    // Re-aimed at the new selection: close the old window first.
    if (g_logoWnd != nullptr && IsWindow(g_logoWnd))
        DestroyWindow(g_logoWnd);

    auto* state = new logo_state;
    for (const auto& path : utagPaths)
    {
        logo_target target;
        target.utagPath = path;
        target.label = station_label(path);
        state->targets.push_back(std::move(target));
    }

    constexpr WORD kButton = 0x0080, kStatic = 0x0082;

    dialog_template t;
    t.begin(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
        0, 0, 240, 236, L"Station logo", L"Segoe UI", 9);
    t.item(kStatic, SS_LEFT | SS_ENDELLIPSIS, 8, 8, 224, 10, kIdHeader, L"");
    t.item(kStatic, SS_OWNERDRAW, 8, 22, 224, 100, kIdPreview, L"");
    t.item(kStatic, SS_LEFT, 8, 128, 224, 66, kIdStatus, L"");
    t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 8, 198, 54, 14, kIdBrowse, L"Browse...");
    t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 66, 198, 62, 14, kIdFavicon, L"Get favicon");
    t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 132, 198, 46, 14, kIdRemove, L"Remove");
    t.item(kButton, BS_DEFPUSHBUTTON | WS_TABSTOP, 182, 198, 50, 14, IDCANCEL, L"Close");
    t.item(kButton, BS_PUSHBUTTON | WS_TABSTOP, 8, 216, 90, 14, kIdHomepage, L"Open homepage");

    const HWND dlg = CreateDialogIndirectParamW(
        core_api::get_my_instance(), t.data(), owner,
        logo_proc, reinterpret_cast<LPARAM>(state));

    if (dlg == nullptr)
    {
        delete state;
        return;
    }

    g_logoWnd = dlg;
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
}
