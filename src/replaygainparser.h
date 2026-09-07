#pragma once

#include <QByteArray>

namespace Fooyin::Bandcamp {

struct ReplayGainData
{
    bool  hasTrackGain{false};
    float trackGainDb{0.0f};
    bool  hasAlbumGain{false};
    float albumGainDb{0.0f};
    bool  hasTrackPeak{false};
    float trackPeak{1.0f};
    bool  hasAlbumPeak{false};
    float albumPeak{1.0f};
};

ReplayGainData parseReplayGain(const QByteArray& data);
size_t id3v2TagSize(const QByteArray& data);

} // namespace Fooyin::Bandcamp
