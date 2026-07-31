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

TEST_CASE("a short record leaves absent fields at their defaults") {
    // Fields are only ever APPENDED. A record shorter than kSettingsBytes means
    // newer fields are absent, not zero. Each field reads only if present in the
    // payload, otherwise defaults are left untouched. This test verifies that the
    // progressive if(len >= N) thresholds prevent silent field drops.

    // Test case 1: len=3, payload has only version and themeId.
    uint8_t rec[h0::kRecordBytes];
    std::memset(rec, 0, sizeof(rec));
    rec[0] = 'H'; rec[1] = 'G'; rec[2] = '0'; rec[3] = '1'; // magic
    rec[4] = 5; rec[5] = 0; rec[6] = 0; rec[7] = 0; // seq=5 LE
    rec[8] = 3; rec[9] = 0; // len=3 LE

    // Payload: version (2 bytes) + themeId (1 byte)
    uint8_t* p = rec + h0::kHeaderBytes;
    p[0] = 0; p[1] = 0; // version=0
    p[2] = 3; // themeId=3

    // Compute and set CRC over the whole record
    const uint16_t crc = h0::recordCrc(rec);
    rec[10] = static_cast<uint8_t>(crc & 0xFF);
    rec[11] = static_cast<uint8_t>(crc >> 8);

    uint32_t seq = 0;
    Settings out{};
    REQUIRE(h0::decodeRecord(rec, seq, out));
    CHECK(out.version == h0::Settings::kVersion); // clamped
    CHECK(out.themeId == 3); // present field survives
    CHECK(out.backlightActive == h0::kDefaults.backlightActive); // absent field is default
    CHECK(out.backlightDim == h0::kDefaults.backlightDim); // absent field is default
    CHECK(out.dimAfterS == h0::kDefaults.dimAfterS); // absent field is default
}

TEST_CASE("a short record truncating mid-byte leaves later fields at defaults") {
    // Truncate a record right after the first byte of a 2-byte field (blankAfterS is at p[7-8]).
    // This tests that a record claiming len=8 (p[0-7]) correctly skips blankAfterS and all later fields.
    uint8_t rec[h0::kRecordBytes];
    std::memset(rec, 0, sizeof(rec));
    rec[0] = 'H'; rec[1] = 'G'; rec[2] = '0'; rec[3] = '1'; // magic
    rec[4] = 6; rec[5] = 0; rec[6] = 0; rec[7] = 0; // seq=6 LE
    rec[8] = 8; rec[9] = 0; // len=8 LE (payload has p[0-7])

    // Payload: version + themeId + backlightActive + backlightDim + dimAfterS (8 bytes total)
    uint8_t* p = rec + h0::kHeaderBytes;
    p[0] = 1; p[1] = 0; // version=1
    p[2] = 2; // themeId=2
    p[3] = 96; // backlightActive=96
    p[4] = 12; // backlightDim=12
    p[5] = 30; p[6] = 0; // dimAfterS=30 LE
    p[7] = 99; // partial blankAfterS (only first byte present)

    // Compute and set CRC over the whole record
    const uint16_t crc = h0::recordCrc(rec);
    rec[10] = static_cast<uint8_t>(crc & 0xFF);
    rec[11] = static_cast<uint8_t>(crc >> 8);

    uint32_t seq = 0;
    Settings out{};
    REQUIRE(h0::decodeRecord(rec, seq, out));
    CHECK(out.version == 1); // present
    CHECK(out.themeId == 2); // present
    CHECK(out.backlightActive == 96); // present
    CHECK(out.backlightDim == 12); // present
    CHECK(out.dimAfterS == 30); // present
    CHECK(out.blankAfterS == h0::kDefaults.blankAfterS); // absent (len < 9)
    CHECK(out.alarmS == h0::kDefaults.alarmS); // absent
    CHECK(out.mute == h0::kDefaults.mute); // absent
}
