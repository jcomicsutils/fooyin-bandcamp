#include "replaygainparser.h"

#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cstring>

namespace Fooyin::Bandcamp {

namespace {

float parseGainString(const QString& str, bool* ok)
{
    // String might be formatted as "-4.00 dB", "+1.7 dB", "-4.00", etc.
    QString cleaned = str.trimmed();
    if (cleaned.endsWith(QLatin1String("dB"), Qt::CaseInsensitive)) {
        cleaned.chop(2);
        cleaned = cleaned.trimmed();
    }
    return cleaned.toFloat(ok);
}

void parseId3Txxx(const QByteArray& frameData, uint8_t version, ReplayGainData& result)
{
    if (frameData.size() < 2)
        return;

    uint8_t encoding = static_cast<uint8_t>(frameData.at(0));
    const char* payload = frameData.constData() + 1;
    int payloadLen = frameData.size() - 1;

    QString desc;
    QString val;

    if (encoding == 0 || encoding == 3) {
        // ISO-8859-1 or UTF-8 (single zero terminator between desc and val)
        int termPos = -1;
        for (int i = 0; i < payloadLen; ++i) {
            if (payload[i] == '\0') {
                termPos = i;
                break;
            }
        }
        if (termPos >= 0) {
            if (encoding == 3) {
                desc = QString::fromUtf8(payload, termPos);
                val  = QString::fromUtf8(payload + termPos + 1, payloadLen - termPos - 1);
            } else {
                desc = QString::fromLatin1(payload, termPos);
                val  = QString::fromLatin1(payload + termPos + 1, payloadLen - termPos - 1);
            }
        }
    } else if (encoding == 1 || encoding == 2) {
        // UTF-16 with BOM (1) or UTF-16BE (2), double zero terminator on 2-byte boundary
        int termPos = -1;
        for (int i = 0; i + 1 < payloadLen; i += 2) {
            if (payload[i] == '\0' && payload[i + 1] == '\0') {
                termPos = i;
                break;
            }
        }
        if (termPos >= 0) {
            auto extractU16 = [](const char* p, int len) -> QString {
                if (len < 2) return {};
                return QString::fromUtf16(reinterpret_cast<const char16_t*>(p), len / 2);
            };
            desc = extractU16(payload, termPos);
            val  = extractU16(payload + termPos + 2, payloadLen - termPos - 2);
        }
    }

    desc = desc.trimmed().toUpper();
    val  = val.trimmed();

    bool parseOk = false;
    if (desc == QLatin1String("REPLAYGAIN_TRACK_GAIN") && !result.hasTrackGain) {
        float gain = parseGainString(val, &parseOk);
        if (parseOk) {
            result.hasTrackGain = true;
            result.trackGainDb  = gain;
        }
    } else if (desc == QLatin1String("REPLAYGAIN_TRACK_PEAK") && !result.hasTrackPeak) {
        float peak = val.toFloat(&parseOk);
        if (parseOk && peak > 0.0f) {
            result.hasTrackPeak = true;
            result.trackPeak    = peak;
        }
    } else if (desc == QLatin1String("REPLAYGAIN_ALBUM_GAIN") && !result.hasAlbumGain) {
        float gain = parseGainString(val, &parseOk);
        if (parseOk) {
            result.hasAlbumGain = true;
            result.albumGainDb  = gain;
        }
    } else if (desc == QLatin1String("REPLAYGAIN_ALBUM_PEAK") && !result.hasAlbumPeak) {
        float peak = val.toFloat(&parseOk);
        if (parseOk && peak > 0.0f) {
            result.hasAlbumPeak = true;
            result.albumPeak    = peak;
        }
    }
}

size_t parseId3v2(const QByteArray& data, ReplayGainData& result)
{
    if (data.size() < 10 || std::memcmp(data.constData(), "ID3", 3) != 0)
        return 0;

    const uint8_t majorVersion = static_cast<uint8_t>(data.at(3));
    const uint8_t flags        = static_cast<uint8_t>(data.at(5));

    if (majorVersion < 3 || majorVersion > 4)
        return 0;

    const uint32_t tagSize =
        ((static_cast<uint8_t>(data.at(6)) & 0x7F) << 21) |
        ((static_cast<uint8_t>(data.at(7)) & 0x7F) << 14) |
        ((static_cast<uint8_t>(data.at(8)) & 0x7F) << 7)  |
        (static_cast<uint8_t>(data.at(9)) & 0x7F);

    const size_t totalId3Size = 10 + tagSize + ((flags & 0x10) ? 10 : 0);
    const size_t limit = std::min<size_t>(data.size(), 10 + tagSize);

    size_t pos = 10;
    // Skip extended header if present
    if (flags & 0x20) {
        if (pos + 4 <= limit) {
            uint32_t extSize = 0;
            if (majorVersion == 3) {
                extSize = (static_cast<uint8_t>(data.at(pos)) << 24) |
                          (static_cast<uint8_t>(data.at(pos + 1)) << 16) |
                          (static_cast<uint8_t>(data.at(pos + 2)) << 8) |
                          static_cast<uint8_t>(data.at(pos + 3));
                pos += 4 + extSize;
            } else {
                extSize = ((static_cast<uint8_t>(data.at(pos)) & 0x7F) << 21) |
                          ((static_cast<uint8_t>(data.at(pos + 1)) & 0x7F) << 14) |
                          ((static_cast<uint8_t>(data.at(pos + 2)) & 0x7F) << 7) |
                          (static_cast<uint8_t>(data.at(pos + 3)) & 0x7F);
                pos += extSize;
            }
        }
    }

    while (pos + 10 <= limit) {
        const char* frameHeader = data.constData() + pos;
        if (frameHeader[0] == '\0')
            break; // Padding reached

        uint32_t frameSize = 0;
        if (majorVersion == 4) {
            frameSize = ((static_cast<uint8_t>(frameHeader[4]) & 0x7F) << 21) |
                        ((static_cast<uint8_t>(frameHeader[5]) & 0x7F) << 14) |
                        ((static_cast<uint8_t>(frameHeader[6]) & 0x7F) << 7)  |
                        (static_cast<uint8_t>(frameHeader[7]) & 0x7F);
        } else {
            frameSize = (static_cast<uint8_t>(frameHeader[4]) << 24) |
                        (static_cast<uint8_t>(frameHeader[5]) << 16) |
                        (static_cast<uint8_t>(frameHeader[6]) << 8)  |
                        static_cast<uint8_t>(frameHeader[7]);
        }

        if (frameSize == 0 || pos + 10 + frameSize > limit)
            break;

        if (std::memcmp(frameHeader, "TXXX", 4) == 0) {
            QByteArray framePayload = data.mid(static_cast<int>(pos + 10), static_cast<int>(frameSize));
            parseId3Txxx(framePayload, majorVersion, result);
        }

        pos += 10 + frameSize;
    }

    return totalId3Size;
}

void parseLameTag(const QByteArray& data, size_t audioOffset, ReplayGainData& result)
{
    if (audioOffset >= static_cast<size_t>(data.size()))
        return;

    const size_t searchLimit = std::min<size_t>(data.size(), audioOffset + 8192);
    if (searchLimit < audioOffset + 36)
        return;

    // Search for "LAME" or "Info" / "Xing"
    const char* raw = data.constData();
    const char* found = nullptr;

    for (size_t i = audioOffset; i + 4 <= searchLimit; ++i) {
        if (std::memcmp(raw + i, "LAME", 4) == 0) {
            found = raw + i;
            break;
        }
    }

    if (!found)
        return;

    const size_t lameOffset = found - raw;
    if (lameOffset + 36 > static_cast<size_t>(data.size()))
        return;

    // Bytes 11-14: Peak signal amplitude (32-bit big-endian fixed point, / 2^23)
    const uint32_t rawPeak =
        (static_cast<uint8_t>(found[11]) << 24) |
        (static_cast<uint8_t>(found[12]) << 16) |
        (static_cast<uint8_t>(found[13]) << 8)  |
        static_cast<uint8_t>(found[14]);

    if (rawPeak > 0 && !result.hasTrackPeak) {
        result.hasTrackPeak = true;
        result.trackPeak    = static_cast<float>(static_cast<double>(rawPeak) / static_cast<double>(1 << 23));
    }

    // Decode 16-bit ReplayGain helper
    auto decodeGainField = [](uint16_t rg, bool& hasGain, float& gainDb) {
        const uint8_t  name = (rg >> 13) & 0x07;
        const uint8_t  orig = (rg >> 10) & 0x07;
        const bool     sign = (rg >> 9)  & 0x01;
        const uint16_t val  = rg & 0x01FF;

        if (name != 0 && orig != 0) {
            float g = static_cast<float>(val) / 10.0f;
            if (sign) g = -g;
            hasGain = true;
            gainDb  = g;
        }
    };

    // Bytes 15-16: Radio / Track ReplayGain
    const uint16_t rawTrackRg =
        (static_cast<uint8_t>(found[15]) << 8) | static_cast<uint8_t>(found[16]);
    if (!result.hasTrackGain) {
        decodeGainField(rawTrackRg, result.hasTrackGain, result.trackGainDb);
    }

    // Bytes 17-18: Audiophile / Album ReplayGain
    const uint16_t rawAlbumRg =
        (static_cast<uint8_t>(found[17]) << 8) | static_cast<uint8_t>(found[18]);
    if (!result.hasAlbumGain) {
        decodeGainField(rawAlbumRg, result.hasAlbumGain, result.albumGainDb);
    }
}

} // namespace

size_t id3v2TagSize(const QByteArray& data)
{
    if (data.size() < 10 || std::memcmp(data.constData(), "ID3", 3) != 0)
        return 0;

    const uint8_t majorVersion = static_cast<uint8_t>(data.at(3));
    const uint8_t flags        = static_cast<uint8_t>(data.at(5));

    if (majorVersion < 3 || majorVersion > 4)
        return 0;

    const uint32_t tagSize =
        ((static_cast<uint8_t>(data.at(6)) & 0x7F) << 21) |
        ((static_cast<uint8_t>(data.at(7)) & 0x7F) << 14) |
        ((static_cast<uint8_t>(data.at(8)) & 0x7F) << 7)  |
        (static_cast<uint8_t>(data.at(9)) & 0x7F);

    return 10 + tagSize + ((flags & 0x10) ? 10 : 0);
}

ReplayGainData parseReplayGain(const QByteArray& data)
{
    ReplayGainData result;

    const size_t audioOffset = parseId3v2(data, result);
    parseLameTag(data, audioOffset, result);

    return result;
}

} // namespace Fooyin::Bandcamp
