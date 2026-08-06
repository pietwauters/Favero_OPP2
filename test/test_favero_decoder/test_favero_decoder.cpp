// Unit tests for FaveroSerialDecoder / decodeFaveroFrame.
// Run with: pio test -e native -f test_favero_decoder
#include <doctest/doctest.h>
#include "FaveroSerialDecoder.h"
#include <vector>

namespace {

uint8_t sumChecksum(const uint8_t* buf, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum = static_cast<uint8_t>(sum + buf[i]);
    return sum;
}

// Builds a valid 10-byte frame (sync + 8 payload bytes) with a correct
// trailing checksum, regardless of what the payload bytes are.
std::vector<uint8_t> makeFrame(uint8_t rightScore, uint8_t leftScore,
                                uint8_t seconds, uint8_t minutes,
                                uint8_t lamps, uint8_t matchesAndPrio,
                                uint8_t chronoByte, uint8_t cards) {
    std::vector<uint8_t> f = {0xFF,      rightScore,     leftScore, seconds,
                               minutes,   lamps,          matchesAndPrio,
                               chronoByte, cards};
    f.push_back(sumChecksum(f.data(), f.size()));
    return f;
}

struct Capture {
    std::vector<FaveroFrame> frames;
};

void onFrame(const FaveroFrame& f, void* ctx) {
    static_cast<Capture*>(ctx)->frames.push_back(f);
}

}  // namespace

// ── decodeFaveroFrame — field mapping ───────────────────────────────────────
// Mirrors the spec's own worked example (right=6, left=12, time=2:56, red +
// right-yellow lamps on, left priority, 2 matches, left yellow card on).
// NOTE: the spec document's printed CRC byte (0x56) doesn't actually satisfy
// its own "sum of the 9 previous bytes mod 256" rule (correct value is
// 0xC5) — everything else about the example checks out, so this looks like
// a typo in the spec rather than a different checksum rule. Worth
// double-checking against a real capture once hardware is wired up.
TEST_CASE("decodeFaveroFrame: matches the spec's worked example fields") {
    const uint8_t buf[10] = {0xFF, 0x06, 0x12, 0x56, 0x02,
                              0x14, 0x0A, 0x00, 0x38, 0xC5};
    FaveroFrame f = decodeFaveroFrame(buf);

    CHECK(f.rightScore == 6);
    CHECK(f.leftScore == 12);
    CHECK(f.seconds == 56);
    CHECK(f.minutes == 2);

    CHECK(f.redOn == true);
    CHECK(f.rightYellowLamp == true);
    CHECK(f.greenOn == false);
    CHECK(f.leftWhite == false);
    CHECK(f.rightWhite == false);
    CHECK(f.leftYellowLamp == false);

    CHECK(f.numMatches == 2);
    CHECK(f.prioLeft == true);
    CHECK(f.prioRight == false);

    CHECK(f.chronoRunning == false);

    CHECK(f.yellowCardLeft == true);
    CHECK(f.redCardRight == false);
    CHECK(f.redCardLeft == false);
    CHECK(f.yellowCardRight == false);
}

TEST_CASE("decodeFaveroFrame: chrono running bit") {
    const uint8_t buf[10] = {0xFF, 0, 0, 0, 0, 0, 0, 0x01, 0, 0};
    FaveroFrame f = decodeFaveroFrame(buf);
    CHECK(f.chronoRunning == true);
}

// ── FaveroSerialDecoder — framing state machine ─────────────────────────────

TEST_CASE("decoder: acquires lock on first valid frame") {
    FaveroSerialDecoder dec;
    Capture cap;
    dec.setCallback(onFrame, &cap);

    auto frame = makeFrame(6, 12, 56, 2, 0x14, 0x0A, 0x00, 0x38);
    for (uint8_t b : frame) dec.feed(b);

    CHECK(dec.isLocked() == true);
    CHECK(dec.framesDecoded() == 1);
    CHECK(dec.framesRejected() == 0);
    REQUIRE(cap.frames.size() == 1);
    CHECK(cap.frames[0].rightScore == 6);
}

TEST_CASE("decoder: ignores noise before the first sync byte") {
    FaveroSerialDecoder dec;
    Capture cap;
    dec.setCallback(onFrame, &cap);

    dec.feed(0x00);
    dec.feed(0x12);
    dec.feed(0x34);
    auto frame = makeFrame(1, 2, 3, 0, 0, 0, 0, 0);
    for (uint8_t b : frame) dec.feed(b);

    CHECK(dec.isLocked() == true);
    REQUIRE(cap.frames.size() == 1);
    CHECK(cap.frames[0].rightScore == 1);
}

TEST_CASE("decoder: all-zero payload (CRC==0xFF) does not cause a false resync") {
    // sum(0xFF + eight 0x00 bytes) == 0xFF, so this frame's checksum byte
    // is legitimately 0xFF too -- immediately followed by the next frame's
    // real sync byte. This is exactly the corner case that breaks a naive
    // "resync on every 0xFF" parser.
    FaveroSerialDecoder dec;
    Capture cap;
    dec.setCallback(onFrame, &cap);

    auto zeroFrame = makeFrame(0, 0, 0, 0, 0, 0, 0, 0);
    REQUIRE(zeroFrame.back() == 0xFF);  // sanity-check the corner case is real

    auto normalFrame = makeFrame(3, 4, 5, 1, 0, 0, 0, 0);

    for (uint8_t b : zeroFrame) dec.feed(b);
    for (uint8_t b : zeroFrame) dec.feed(b);
    for (uint8_t b : normalFrame) dec.feed(b);

    CHECK(dec.framesRejected() == 0);
    REQUIRE(cap.frames.size() == 3);
    CHECK(cap.frames[0].rightScore == 0);
    CHECK(cap.frames[1].rightScore == 0);
    CHECK(cap.frames[2].rightScore == 3);
    CHECK(cap.frames[2].leftScore == 4);
}

TEST_CASE("decoder: a dropped byte desyncs LOCKED mode, then recovers") {
    FaveroSerialDecoder dec;
    Capture cap;
    dec.setCallback(onFrame, &cap);

    auto good1 = makeFrame(1, 1, 0, 0, 0, 0, 0, 0);
    for (uint8_t b : good1) dec.feed(b);
    CHECK(dec.isLocked() == true);

    // Simulate one dropped byte: feed a frame with its first byte (the
    // sync byte) missing. LOCKED mode doesn't notice anything is wrong
    // until it has collected a full 10-byte window.
    auto good2 = makeFrame(2, 2, 0, 0, 0, 0, 0, 0);
    for (size_t i = 1; i < good2.size(); ++i) dec.feed(good2[i]);  // 9 bytes

    // The next frame's sync byte lands in the last slot of that
    // misaligned window, completing it -- and gets consumed as filler,
    // not recognised as a sync byte. So this window fails validation
    // (framesRejected) and the decoder drops to HUNTING, but the sync
    // byte that would have let it resync immediately is already gone.
    auto good3 = makeFrame(3, 3, 0, 0, 0, 0, 0, 0);
    for (uint8_t b : good3) dec.feed(b);
    CHECK(dec.framesRejected() >= 1);
    CHECK(dec.isLocked() == false);

    // Resync happens on the following frame's sync byte instead -- full
    // recovery takes up to two frame periods after a dropped byte, not
    // one, which is still well within the ~42ms cadence.
    auto good4 = makeFrame(4, 4, 0, 0, 0, 0, 0, 0);
    for (uint8_t b : good4) dec.feed(b);

    CHECK(dec.isLocked() == true);
    REQUIRE(cap.frames.size() >= 2);
    CHECK(cap.frames.back().rightScore == 4);
}

TEST_CASE("decoder: rejects a frame with a corrupted checksum byte and resyncs") {
    FaveroSerialDecoder dec;
    Capture cap;
    dec.setCallback(onFrame, &cap);

    auto good1 = makeFrame(5, 5, 0, 0, 0, 0, 0, 0);
    for (uint8_t b : good1) dec.feed(b);
    CHECK(dec.isLocked() == true);

    auto corrupted = makeFrame(6, 6, 0, 0, 0, 0, 0, 0);
    corrupted.back() ^= 0x01;  // flip a bit in the checksum
    for (uint8_t b : corrupted) dec.feed(b);
    CHECK(dec.isLocked() == false);

    auto good2 = makeFrame(7, 7, 0, 0, 0, 0, 0, 0);
    for (uint8_t b : good2) dec.feed(b);
    CHECK(dec.isLocked() == true);
    CHECK(cap.frames.back().rightScore == 7);
}
