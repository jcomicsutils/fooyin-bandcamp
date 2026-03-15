#pragma once

#include <core/engine/audioinput.h>

#define MINIMP3_EXT_ENABLED
#include "minimp3_ex.h"

#include <QMutex>
#include <memory>

namespace Fooyin::Bandcamp {

    inline constexpr const char* kVirtualExt = "bcstream";
    inline constexpr const char* kScheme     = "bandcamp";

    // Forward declare the shared streaming state
    struct BandcampStreamState;

    // ── AudioReader ───────────────────────────────────────────────────────────

    class BandcampReader : public AudioReader
    {
    public:
        [[nodiscard]] QStringList extensions()      const override;
        [[nodiscard]] bool canReadCover()           const override;
        [[nodiscard]] bool canWriteMetaData()       const override;

        bool readTrack(const AudioSource& source, Track& track) override;

        QByteArray readCover(const AudioSource& source,
                             const Track&       track,
                             Track::Cover       cover) override;
    };

    // ── AudioDecoder ──────────────────────────────────────────────────────────

    class BandcampDecoder : public AudioDecoder
    {
    public:
        BandcampDecoder();
        ~BandcampDecoder() override;

        [[nodiscard]] QStringList extensions() const override;
        [[nodiscard]] bool isSeekable()        const override;

        std::optional<AudioFormat> init(const AudioSource& source,
                                        const Track&       track,
                                        DecoderOptions     options) override;
                                        void start()            override;
                                        void stop()             override;
                                        void seek(uint64_t pos) override;

                                        AudioBuffer readBuffer(size_t bytes) override;

    private:
        void stopInternal();                       // call with m_mutex held
        [[nodiscard]] uint64_t currentMs() const;  // call with m_mutex held

        // Safely copies the shared pointer preventing data races
        std::shared_ptr<BandcampStreamState> getState() const;

        // Custom minimp3 callbacks for streaming
        static size_t streamReadCb(void *buf, size_t size, void *user_data);
        static int streamSeekCb(uint64_t position, void *user_data);

        mutable QMutex m_mutex;        // guards m_dec, m_initialized
        bool         m_initialized{false};
        AudioFormat  m_format;
        mp3dec_ex_t* m_dec{nullptr};
        mp3dec_io_t  m_io{};

        // Thread-safe Streaming state
        mutable QMutex m_statePtrMutex; // Guards access to the m_streamState pointer itself
        std::shared_ptr<BandcampStreamState> m_streamState;
        size_t m_readPos{0};
    };

} // namespace Fooyin::Bandcamp
