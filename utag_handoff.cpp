#include "utag_common.h"

namespace utag_state
{
    // Retagging reopens the track within a moment; anything later is a
    // manual stop/replay and must get a fresh connection.
    constexpr ULONGLONG kHandoffMaxAgeMs = 2000;

    std::mutex g_handoffMutex;
    std::string g_handoffUrl;
    service_ptr_t<input_decoder> g_handoffDecoder;
    ULONGLONG g_handoffTime = 0;
    HANDLE g_handoffTimer = nullptr;            // guarded by g_handoffMutex
    std::atomic<bool> g_handoffShutdown{ false }; // set once on_quit runs

    // Drops the parked decoder if it is stale (or always, when forced).
    // The decoder is released outside the lock, because closing the
    // connection can take a moment.
    void handoff_clear(bool force)
    {
        service_ptr_t<input_decoder> doomed;

        {
            std::lock_guard<std::mutex> lock(g_handoffMutex);

            if (!g_handoffDecoder.is_valid())
                return;

            if (force ||
                GetTickCount64() - g_handoffTime >= kHandoffMaxAgeMs)
            {
                doomed = g_handoffDecoder;
                g_handoffDecoder.release();
                g_handoffUrl.clear();
            }
        }
    }

    VOID CALLBACK handoff_timer(PVOID, BOOLEAN)
    {
        try
        {
            handoff_clear(false);
        }
        catch (...)
        {
        }
    }

    void handoff_store(
        const std::string& url,
        const service_ptr_t<input_decoder>& decoder)
    {
        // Once shutdown has started, park nothing: a pending timer could
        // otherwise fire after this DLL has been unloaded.
        if (g_handoffShutdown.load())
            return;

        service_ptr_t<input_decoder> old;
        HANDLE oldTimer = nullptr;

        {
            std::lock_guard<std::mutex> lock(g_handoffMutex);

            old = g_handoffDecoder;
            g_handoffDecoder = decoder;
            g_handoffUrl = url;
            g_handoffTime = GetTickCount64();

            oldTimer = g_handoffTimer;
            g_handoffTimer = nullptr;
        }

        old.release();

        // The previous timer is finished or superseded; release it. A null
        // completion event makes this call non-blocking.
        if (oldTimer != nullptr)
            DeleteTimerQueueTimer(nullptr, oldTimer, nullptr);

        HANDLE timer = nullptr;
        if (CreateTimerQueueTimer(
            &timer, nullptr, handoff_timer, nullptr,
            static_cast<DWORD>(kHandoffMaxAgeMs + 500), 0, WT_EXECUTEONLYONCE))
        {
            bool shutdown = false;
            {
                std::lock_guard<std::mutex> lock(g_handoffMutex);
                shutdown = g_handoffShutdown.load();
                if (!shutdown)
                    g_handoffTimer = timer;   // on_quit will now find and delete it
            }

            // on_quit already collected its timer: ours must not outlive the DLL.
            // Blocking delete is safe here, no lock is held (the callback takes it).
            if (shutdown)
                DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
        }
    }

    bool handoff_take(
        const std::string& url,
        service_ptr_t<input_decoder>& out)
    {
        std::lock_guard<std::mutex> lock(g_handoffMutex);

        if (!g_handoffDecoder.is_valid())
            return false;

        if (g_handoffUrl != url ||
            GetTickCount64() - g_handoffTime >= kHandoffMaxAgeMs)
        {
            return false;
        }

        out = g_handoffDecoder;
        g_handoffDecoder.release();
        g_handoffUrl.clear();
        return true;
    }

    // Close any parked stream when foobar2000 shuts down.
    class handoff_initquit : public initquit
    {
    public:
        void on_init() override {}

        void on_quit() override
        {
            g_handoffShutdown.store(true);
            handoff_clear(true);

            // Wait for a running timer callback and release the timer, so
            // nothing can call into this DLL after it is unloaded.
            HANDLE timer = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_handoffMutex);
                timer = g_handoffTimer;
                g_handoffTimer = nullptr;
            }
            if (timer != nullptr)
                DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
        }
    };

    initquit_factory_t<handoff_initquit> g_handoff_initquit_factory;

    // -------------------------------------------------------------------------
    // Last known technical info (codec, bitrate, ...) of the playing stream.
    // Retag / Properties / library reloads open the stub without a decoder
    // (input_open_info_*), which would otherwise wipe the codec info.
    // -------------------------------------------------------------------------
    std::mutex g_techMutex;
    std::string g_techUrl;
    file_info_impl g_techInfo;
    bool g_techValid = false;

    void tech_store(const std::string& url, const file_info& src)
    {
        // Only cache info that actually names a codec.
        if (src.info_get("codec") == nullptr)
            return;

        std::lock_guard<std::mutex> lock(g_techMutex);
        g_techInfo.reset();
        g_techInfo.copy_info(src);
        g_techUrl = url;
        g_techValid = true;
    }

    bool tech_load(const std::string& url, file_info& dst)
    {
        std::lock_guard<std::mutex> lock(g_techMutex);

        if (!g_techValid || g_techUrl != url)
        {
            return false;
        }

        dst.copy_info(g_techInfo);
        return true;
    }

    // Apply playback-discovered technical info to foobar2000's metadata DB.
    // Properties reads the metadb snapshot; changing our private cache alone
    // does not update that snapshot. The forced hint is committed on main thread.
    void tech_refresh_metadb(const std::string& path, const file_info& decoderInfo)
    {
        try
        {
            metadb_handle_ptr handle = metadb::get()->handle_create(path.c_str(), 0);
            file_info_impl mergedInfo;
            if (!handle->get_info(mergedInfo))
            {
                return;
            }

            // Don't dispatch a database update if technical info is already current.
            if (file_info::g_is_info_equal(mergedInfo, decoderInfo))
                return;

            // Preserve the reserved stream URL before replacing technical info.
            // copy_info() replaces the complete technical-info section, so without
            // restoring "@" the Properties snapshot loses the URL on playback.
            const char* reservedUrlPtr = mergedInfo.info_get("@");
            const std::string reservedUrl =
                reservedUrlPtr != nullptr ? reservedUrlPtr : std::string();

            // Metadata tags and ReplayGain are separate from the technical-info
            // section and remain intact; restore "@" explicitly after the copy.
            mergedInfo.copy_info(decoderInfo);
            if (!reservedUrl.empty())
                mergedInfo.info_set("@", reservedUrl.c_str());

            const t_filestats stats = handle->get_filestats();
            fb2k::inMainThread([handle, mergedInfo, stats]() {
                try
                {
                    auto hints = metadb_hint_list_v3::create();
                    hints->add_hint_forced(handle, mergedInfo, stats, true);
                    hints->on_done();
                }
                catch (const std::exception&)
                {
                }
                });
        }
        catch (const std::exception&)
        {
        }
    }

    void tech_clear(const std::string& url)
    {
        std::lock_guard<std::mutex> lock(g_techMutex);

        if (g_techValid && g_techUrl == url)
        {
            g_techInfo.reset();
            g_techUrl.clear();
            g_techValid = false;
        }
    }
}
