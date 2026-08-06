#include "FaveroSerialDecoder.h"

namespace {
uint8_t bcdToDecimal(uint8_t bcd) {
    return static_cast<uint8_t>((bcd >> 4) * 10 + (bcd & 0x0F));
}
}  // namespace

FaveroFrame decodeFaveroFrame(const uint8_t buffer[10]) {
    FaveroFrame f;

    f.rightScore = bcdToDecimal(buffer[1]);
    f.leftScore  = bcdToDecimal(buffer[2]);
    f.seconds    = bcdToDecimal(buffer[3]);
    f.minutes    = bcdToDecimal(buffer[4]);

    const uint8_t lamps = buffer[5];
    f.leftWhite       = lamps & (1 << 0);
    f.rightWhite      = lamps & (1 << 1);
    f.redOn           = lamps & (1 << 2);
    f.greenOn         = lamps & (1 << 3);
    f.rightYellowLamp = lamps & (1 << 4);
    f.leftYellowLamp  = lamps & (1 << 5);

    const uint8_t matchesAndPrio = buffer[6];
    f.numMatches = matchesAndPrio & 0x03;
    f.prioRight  = matchesAndPrio & (1 << 2);
    f.prioLeft   = matchesAndPrio & (1 << 3);

    f.chronoRunning = buffer[7] & (1 << 0);

    const uint8_t cards = buffer[8];
    f.redCardRight    = cards & (1 << 0);
    f.redCardLeft     = cards & (1 << 1);
    f.yellowCardRight = cards & (1 << 2);
    f.yellowCardLeft  = cards & (1 << 3);

    return f;
}

bool FaveroSerialDecoder::checksumValid(const uint8_t buf[FRAME_LEN]) {
    uint8_t sum = 0;
    for (size_t i = 0; i < FRAME_LEN - 1; ++i) {
        sum = static_cast<uint8_t>(sum + buf[i]);
    }
    return sum == buf[FRAME_LEN - 1];
}

void FaveroSerialDecoder::emit(const uint8_t buf[FRAME_LEN]) {
    ++m_framesDecoded;
    if (m_callback) {
        const FaveroFrame frame = decodeFaveroFrame(buf);
        m_callback(frame, m_callbackCtx);
    }
}

void FaveroSerialDecoder::feed(uint8_t byte) {
    if (!m_locked) {
        // HUNTING: only byte[0] of a candidate frame must be the sync byte.
        if (m_index == 0) {
            if (byte != SYNC) {
                return;  // keep scanning
            }
        }
        m_buffer[m_index++] = byte;
        if (m_index < FRAME_LEN) {
            return;
        }
        m_index = 0;
        if (checksumValid(m_buffer)) {
            m_locked = true;
            emit(m_buffer);
        } else {
            ++m_framesRejected;
            // Stay in HUNTING; next byte is evaluated as a fresh candidate
            // sync. (Deliberately not rescanning the discarded buffer for
            // an embedded 0xFF — the next real frame arrives within one
            // ~42ms period regardless.)
        }
        return;
    }

    // LOCKED: trust the fixed 10-byte cadence — do NOT resync on 0xFF
    // seen mid-frame (see class comment re: the CRC==0xFF corner case).
    m_buffer[m_index++] = byte;
    if (m_index < FRAME_LEN) {
        return;
    }
    m_index = 0;
    if (m_buffer[0] == SYNC && checksumValid(m_buffer)) {
        emit(m_buffer);
    } else {
        ++m_framesRejected;
        m_locked = false;  // drop back to HUNTING
    }
}
