#include "settings/settings_codec.hpp"

#include <cstring>

namespace h0 {

namespace {

constexpr uint8_t kMagic[4] = {'H', 'G', '0', '1'};

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t seed) {
    uint16_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

uint16_t recordCrc(const uint8_t* rec) {
    // Bytes 0..9 (magic, seq, len) then 12..255 (payload), skipping the CRC
    // field itself at 10..11.
    return crc16Ccitt(rec + kHeaderBytes, kPayloadBytes, crc16Ccitt(rec, 10));
}

void encodeRecord(const Settings& s, uint32_t seq, uint8_t* out) {
    std::memset(out, 0, kRecordBytes);
    std::memcpy(out, kMagic, sizeof(kMagic));
    put32(out + 4, seq);
    put16(out + 8, static_cast<uint16_t>(kSettingsBytes));

    // Field by field, little-endian, never a struct memcpy. Padding and member
    // reordering are exactly the kind of thing that survives review and
    // corrupts settings on a compiler upgrade.
    uint8_t* p = out + kHeaderBytes;
    put16(p + 0, s.version);
    p[2] = s.themeId;
    p[3] = s.backlightActive;
    p[4] = s.backlightDim;
    put16(p + 5, s.dimAfterS);
    put16(p + 7, s.blankAfterS);
    put16(p + 9, s.alarmS);
    p[11] = s.mute;
    put16(p + 12, s.batCalPermille);
    put16(p + 14, s.offAfterS);
    p[16] = s.batCalAuto;
    put16(p + 17, s.batFloorRawMv);

    // Last, so it covers everything written above.
    put16(out + 10, recordCrc(out));
}

bool decodeRecord(const uint8_t* rec, uint32_t& outSeq, Settings& out) {
    if (std::memcmp(rec, kMagic, sizeof(kMagic)) != 0) return false;

    const uint16_t len = get16(rec + 8);
    if (len == 0 || len > kPayloadBytes) return false;

    if (recordCrc(rec) != get16(rec + 10)) return false;

    // Read only what this build knows about, and only what the record actually
    // carries. A shorter record leaves the remaining fields at their defaults,
    // which is the whole migration story for append-only fields.
    Settings s = kDefaults;
    const uint8_t* p = rec + kHeaderBytes;
    if (len >= 2)  s.version = get16(p + 0);
    if (len >= 3)  s.themeId = p[2];
    if (len >= 4)  s.backlightActive = p[3];
    if (len >= 5)  s.backlightDim = p[4];
    if (len >= 7)  s.dimAfterS = get16(p + 5);
    if (len >= 9)  s.blankAfterS = get16(p + 7);
    if (len >= 11) s.alarmS = get16(p + 9);
    if (len >= 12) s.mute = p[11];
    if (len >= 14) s.batCalPermille = get16(p + 12);
    if (len >= 16) s.offAfterS = get16(p + 14);
    if (len >= 17) s.batCalAuto = p[16];
    if (len >= 19) s.batFloorRawMv = get16(p + 17);

    clamp(s);
    out = s;
    outSeq = get32(rec + 4);
    return true;
}

bool isErased(const uint8_t* rec) {
    for (size_t i = 0; i < kRecordBytes; ++i) {
        if (rec[i] != 0xFF) return false;
    }
    return true;
}

} // namespace h0
