// Favero data-port decoder.
//
// Favero apparatus continuously broadcasts a 10-byte status frame at
// 2400-N-8-1, roughly every 42ms:
//
//   [0] 0xFF sync
//   [1] right score       BCD
//   [2] left score        BCD
//   [3] seconds           BCD
//   [4] minutes           BCD, low nibble only (0-9)
//   [5] lamps: D0=left white, D1=right white, D2=red(left on-target),
//              D3=green(right on-target), D4=right yellow lamp,
//              D5=left yellow lamp
//   [6] D0-D1=num matches (0-3), D2=right priority, D3=left priority
//   [7] D0=chrono running (rest: Favero-internal, ignore)
//   [8] D0=right red card, D1=left red card,
//       D2=right yellow card, D3=left yellow card
//   [9] checksum = sum(bytes[0..8]) mod 256, no carry
//
// This header has no Arduino/ESP-IDF dependency — it's host-testable.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

struct FaveroFrame {
    uint8_t rightScore = 0;
    uint8_t leftScore  = 0;
    uint8_t minutes    = 0;
    uint8_t seconds    = 0;

    bool leftWhite  = false;
    bool rightWhite = false;
    bool redOn      = false;  // left on-target
    bool greenOn    = false;  // right on-target
    bool rightYellowLamp = false;
    bool leftYellowLamp  = false;

    uint8_t numMatches = 0;  // 0-3
    bool    prioRight  = false;
    bool    prioLeft   = false;

    bool chronoRunning = false;

    bool redCardRight    = false;
    bool redCardLeft     = false;
    bool yellowCardRight = false;
    bool yellowCardLeft  = false;
};

/// Decode a validated 10-byte buffer into a FaveroFrame.
/// Caller is responsible for sync-byte and checksum validation first
/// (see FaveroSerialDecoder) — this function does no validation itself.
FaveroFrame decodeFaveroFrame(const uint8_t buffer[10]);

/// Byte-at-a-time frame synchroniser for the Favero data port.
///
/// Framing strategy (see design notes — this is the part that's easy to
/// get wrong): a frame whose 9 payload/sync bytes sum to 0xFF produces a
/// checksum byte that is *also* 0xFF (e.g. an all-zero payload: sum =
/// 0xFF + 0*8 = 0xFF). That means the wire can legitimately contain two
/// consecutive 0xFF bytes — the real checksum, immediately followed by
/// the next frame's real sync byte. A parser that resyncs on every 0xFF
/// it sees will misinterpret the checksum byte as a new frame start and
/// desync permanently.
///
/// So 0xFF is only used to *acquire* framing (HUNTING state). Once a
/// valid frame has been decoded, the decoder trusts the fixed 10-byte
/// cadence (LOCKED state) and stops scanning for 0xFF — it just checks
/// buffer[0]==0xFF and the checksum on every subsequent frame, and only
/// falls back to HUNTING if either check fails.
class FaveroSerialDecoder {
public:
    using FrameCallback = void (*)(const FaveroFrame& frame, void* ctx);

    void setCallback(FrameCallback cb, void* ctx = nullptr) {
        m_callback    = cb;
        m_callbackCtx = ctx;
    }

    /// Feed one incoming byte from the Favero serial line.
    void feed(uint8_t byte);

    bool     isLocked() const { return m_locked; }
    uint32_t framesDecoded() const { return m_framesDecoded; }
    uint32_t framesRejected() const { return m_framesRejected; }

private:
    static constexpr uint8_t SYNC      = 0xFF;
    static constexpr size_t  FRAME_LEN = 10;

    static bool checksumValid(const uint8_t buf[FRAME_LEN]);
    void        emit(const uint8_t buf[FRAME_LEN]);

    uint8_t m_buffer[FRAME_LEN] = {};
    size_t  m_index             = 0;
    bool    m_locked            = false;

    FrameCallback m_callback    = nullptr;
    void*         m_callbackCtx = nullptr;

    uint32_t m_framesDecoded  = 0;
    uint32_t m_framesRejected = 0;
};
