#include "doctest.h"

#include <cstring>

#include "settings/settings_codec.hpp"

using h0::Settings;

namespace {

Settings sample() {
    Settings s = h0::kDefaults;
    s.themeId = 2;
    s.backlightActive = 96;
    s.backlightDim = 12;
    s.dimAfterS = 30;
    s.blankAfterS = 120;
    s.alarmS = 30;
    s.mute = 1;
    s.batCalPermille = 1032;
    return s;
}

} // namespace

TEST_CASE("a record round-trips") {
    uint8_t rec[h0::kRecordBytes];
    h0::encodeRecord(sample(), 7, rec);

    uint32_t seq = 0;
    Settings out{};
    REQUIRE(h0::decodeRecord(rec, seq, out));
    CHECK(seq == 7);
    CHECK(out == sample());
}

TEST_CASE("the CRC catches every single-bit flip in the record") {
    // All 2048 bit positions, which is only achievable because recordCrc covers
    // the whole record rather than just the settings payload. A payload-only CRC
    // would leave seq and the 230 reserved bytes undefended and catch ~176.
    uint8_t good[h0::kRecordBytes];
    h0::encodeRecord(sample(), 1, good);

    int caught = 0;
    for (size_t byte = 0; byte < h0::kRecordBytes; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t rec[h0::kRecordBytes];
            std::memcpy(rec, good, sizeof(rec));
            rec[byte] ^= static_cast<uint8_t>(1u << bit);

            uint32_t seq = 0;
            Settings out{};
            if (!h0::decodeRecord(rec, seq, out)) ++caught;
        }
    }
    CHECK(caught == static_cast<int>(h0::kRecordBytes) * 8);
}

TEST_CASE("bad magic is rejected") {
    uint8_t rec[h0::kRecordBytes];
    h0::encodeRecord(sample(), 1, rec);
    rec[0] = 'X';

    uint32_t seq = 0;
    Settings out{};
    CHECK_FALSE(h0::decodeRecord(rec, seq, out));
}

TEST_CASE("an over-long payload length is rejected") {
    uint8_t rec[h0::kRecordBytes];
    h0::encodeRecord(sample(), 1, rec);
    rec[8] = 0xFF; // len low byte
    rec[9] = 0xFF; // len high byte

    uint32_t seq = 0;
    Settings out{};
    CHECK_FALSE(h0::decodeRecord(rec, seq, out));
}

TEST_CASE("an erased page is recognised and does not decode") {
    uint8_t rec[h0::kRecordBytes];
    std::memset(rec, 0xFF, sizeof(rec));
    CHECK(h0::isErased(rec));

    uint32_t seq = 0;
    Settings out{};
    CHECK_FALSE(h0::decodeRecord(rec, seq, out));
}

TEST_CASE("a decoded record is clamped, so garbage cannot escape the codec") {
    // A record can be CRC-valid and still hold nonsense: a downgrade, a bug, a
    // hand-edited image. Clamping at the codec boundary means no caller has to
    // remember to do it.
    Settings evil = h0::kDefaults;
    evil.backlightActive = 0;
    evil.themeId = 99;

    uint8_t rec[h0::kRecordBytes];
    h0::encodeRecord(evil, 1, rec);

    uint32_t seq = 0;
    Settings out{};
    REQUIRE(h0::decodeRecord(rec, seq, out));
    CHECK(out.backlightActive >= h0::kBacklightFloor);
    CHECK(out.themeId < static_cast<uint8_t>(h0::ThemeId::Count));
}

TEST_CASE("a version-0 record migrates to current defaults for unknown fields") {
    // Fields are only ever APPENDED, so an old record's tail is zero-filled and
    // migration is "take what is there, clamp the rest".
    uint8_t rec[h0::kRecordBytes];
    h0::encodeRecord(sample(), 3, rec);
    rec[12] = 0; // version low byte -> 0
    rec[13] = 0;
    // Recompute over the whole record so this tests migration, not corruption.
    const uint16_t crc = h0::recordCrc(rec);
    rec[10] = static_cast<uint8_t>(crc & 0xFF);
    rec[11] = static_cast<uint8_t>(crc >> 8);

    uint32_t seq = 0;
    Settings out{};
    REQUIRE(h0::decodeRecord(rec, seq, out));
    CHECK(out.version == Settings::kVersion);
    CHECK(out.backlightActive == 96); // the fields that WERE present survive
}
