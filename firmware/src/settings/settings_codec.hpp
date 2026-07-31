#pragma once

#include <cstddef>
#include <cstdint>

#include "settings/settings.hpp"

namespace h0 {

/// One record per 256-byte flash page, per power-and-time.md section 5.4.
///
///   offset  size  field
///     0      4    magic  'H','G','0','1'
///     4      4    seq    uint32 LE, monotonic, never reused
///     8      2    len    uint16 LE, payload bytes actually used
///    10      2    crc16  CCITT over payload[0..len)
///    12    244    payload
///
/// Aligned to a page boundary deliberately: the W25Q allows programming at any
/// byte offset, but repeatedly partial-programming an already-programmed page
/// is a grey area across NOR vendors, and alignment buys certainty for free.
inline constexpr size_t kRecordBytes = 256;
inline constexpr size_t kHeaderBytes = 12;
inline constexpr size_t kPayloadBytes = kRecordBytes - kHeaderBytes; // 244

/// Bytes of `payload` a v1 Settings occupies. The rest is zero-filled and
/// reserved -- issue #8's screenOffAfterS goes there, needing no migration.
inline constexpr size_t kSettingsBytes = 14;

/// `seed` lets a CRC be continued across two disjoint regions, which is what
/// `recordCrc` needs to skip over the stored CRC itself.
uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t seed = 0xFFFF);

/// CRC over the WHOLE record except the two bytes holding the CRC.
///
/// Covering only the settings payload would leave `seq` and the 230 reserved
/// bytes unprotected -- and `seq` is what decides which record wins a scan, so
/// a single bit flip there silently resurrects an older set of settings with no
/// symptom at all.
uint16_t recordCrc(const uint8_t* rec);

/// Serialise into a full `kRecordBytes` record. `out` must be that large.
void encodeRecord(const Settings& s, uint32_t seq, uint8_t* out);

/// Returns false on bad magic, an out-of-range `len`, or a CRC mismatch.
/// On success `out` is CLAMPED, so no caller can receive an unusable Settings.
bool decodeRecord(const uint8_t* rec, uint32_t& outSeq, Settings& out);

/// True when every byte is 0xFF -- a page that has never been programmed.
bool isErased(const uint8_t* rec);

} // namespace h0
