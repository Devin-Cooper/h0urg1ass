#include "doctest.h"

#include "fake_flash.hpp"
#include "settings/settings_store.hpp"

using h0::Settings;
using h0::SettingsStore;

namespace {

constexpr uint32_t BASE = SettingsStore::kSectorA;
constexpr size_t SPAN = 2 * h0::IFlashBackend::kSectorBytes;

Settings tweaked(uint8_t theme) {
    Settings s = h0::kDefaults;
    s.themeId = theme;
    return s;
}

} // namespace

TEST_CASE("a blank device loads defaults and writes nothing") {
    // Deliberately NOT section 5.4's "write seq=1 at boot". A device that is
    // never configured never writes, and a failing flash is not retried on
    // every power-on.
    //
    // No check on settings() itself: current_'s member initializer is already
    // kDefaults, so that assertion would hold even with load()'s body
    // deleted. The counters below are the real claim.
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();

    CHECK(f.eraseCount() == 0);
    CHECK(f.programCount() == 0); // the actual claim; eraseCount alone says nothing
}

TEST_CASE("the first commit writes seq 1 and survives a reboot") {
    FakeFlash f(BASE, SPAN);
    {
        SettingsStore store(f);
        store.load();
        REQUIRE(store.commit(tweaked(2)));
    }

    // The title claims a seq value; SettingsStore does not expose seq_, so
    // read the raw record back and decode it directly rather than just
    // asserting on the settings content, which says nothing about seq.
    uint8_t rec[h0::kRecordBytes];
    f.read(BASE, rec, h0::kRecordBytes);
    uint32_t seq = 0;
    Settings decoded{};
    REQUIRE(h0::decodeRecord(rec, seq, decoded));
    CHECK(seq == 1);

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 2);
}

TEST_CASE("the highest seq wins, not the last page scanned") {
    // commit() always appends in slot order, so seq order and slot-scan order
    // never disagree through the normal API -- a "last decodable record
    // scanned wins" implementation would pass every test that only calls
    // commit(). Write directly instead: put the HIGHER seq at the LOWER slot
    // (scanned first) and the LOWER seq at the HIGHER slot (scanned last), so
    // the two rules make different predictions and only one can be right.
    FakeFlash f(BASE, SPAN);

    uint8_t recHigh[h0::kRecordBytes];
    uint8_t recLow[h0::kRecordBytes];
    h0::encodeRecord(tweaked(2), /*seq=*/9, recHigh);
    h0::encodeRecord(tweaked(1), /*seq=*/3, recLow);

    const uint32_t slot0 = BASE;
    const uint32_t slot1 = BASE + static_cast<uint32_t>(h0::kRecordBytes);
    REQUIRE(f.programPage(slot0, recHigh)); // seq 9, scanned FIRST
    REQUIRE(f.programPage(slot1, recLow));  // seq 3, scanned LAST

    SettingsStore store(f);
    store.load();
    CHECK(store.settings().themeId == 2); // seq 9's theme, not seq 3's
}

TEST_CASE("sixteen commits fill a sector; the seventeenth rolls and arms an erase") {
    // The ordering is the whole point: write the NEW sector's first record,
    // THEN erase the old one, so there is never an instant with no valid record
    // on the device.
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();

    for (int i = 0; i < SettingsStore::kRecordsPerSector; ++i) {
        REQUIRE(store.commit(tweaked(1)));
    }
    CHECK_FALSE(store.needsErase());
    CHECK(f.eraseCount() == 0);

    REQUIRE(store.commit(tweaked(3)));
    CHECK(store.needsErase());
    CHECK(f.eraseCount() == 0); // armed, NOT performed -- a 400 ms stall is not
                                // allowed to land on a user action

    REQUIRE(store.runDeferredErase());
    CHECK(f.eraseCount() == 1);
    CHECK_FALSE(store.needsErase());

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 3);
}

TEST_CASE("a power cut at any byte of a page program leaves the previous record") {
    for (long die = 0; die < 256; ++die) {
        FakeFlash f(BASE, SPAN);
        SettingsStore store(f);
        store.load();
        REQUIRE(store.commit(tweaked(1)));

        f.dieAfter(die);
        store.commit(tweaked(2)); // may fail; that is the point
        f.revive();

        SettingsStore reopened(f);
        reopened.load();
        // Either the new record landed whole, or it did not land at all.
        // Never a third outcome.
        const uint8_t t = reopened.settings().themeId;
        CHECK((t == 1 || t == 2));
    }
}

TEST_CASE("a power cut mid-roll leaves both sectors valid and the erase re-armed") {
    FakeFlash f(BASE, SPAN);
    {
        SettingsStore store(f);
        store.load();
        for (int i = 0; i < SettingsStore::kRecordsPerSector; ++i) {
            REQUIRE(store.commit(tweaked(1)));
        }
        REQUIRE(store.commit(tweaked(3))); // rolls, arms the erase
        // Power cut here: the erase never runs.
    }
    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 3);
    CHECK(reopened.needsErase()); // detected at boot and re-armed
}

TEST_CASE("a power cut mid-erase still leaves the surviving record findable") {
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();
    for (int i = 0; i < SettingsStore::kRecordsPerSector; ++i) {
        REQUIRE(store.commit(tweaked(1)));
    }
    REQUIRE(store.commit(tweaked(3)));

    f.dieAfter(1000); // partway through the 4096-byte erase
    store.runDeferredErase();
    f.revive();

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 3);
}

TEST_CASE("an all-garbage sector yields defaults and is NOT wiped") {
    // A device that erases its own settings sector on a bad read destroys the
    // evidence of why. The next commit takes a free page anyway.
    //
    // No check on settings() alone: current_'s member initializer is already
    // kDefaults, so that would hold even if load() never scanned anything.
    // eraseCount()==0 is a real claim already; the commit below makes "the
    // next commit takes a free page anyway" an assertion instead of only a
    // comment.
    FakeFlash f(BASE, SPAN);
    uint8_t junk[h0::kRecordBytes];
    for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = static_cast<uint8_t>(i * 7 + 1);
    for (int i = 0; i < SettingsStore::kRecordsPerSector; ++i) {
        f.programPage(BASE + static_cast<uint32_t>(i) * 256, junk);
    }

    SettingsStore store(f);
    store.load();
    CHECK(f.eraseCount() == 0);

    // Sector A is full of garbage, no free slot in it -- the commit must land
    // in sector B without erasing the garbage sector to make room.
    REQUIRE(store.commit(tweaked(2)));
    CHECK(f.eraseCount() == 0);

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 2);
}

TEST_CASE("a device that is never idle still saves, forced erase and all") {
    // The pathological case: both sectors full and runDeferredErase never
    // gated in. Eating a 400 ms stall beats silently failing to save.
    //
    // Themes cycle over {1, 2, 3} -- never 0, which is kDefaults.themeId --
    // so "the last record survived" and "load() silently fell back to
    // defaults" are outcomes this test can actually tell apart. (With a 4-way
    // cycle including 0, the final theme and the default theme coincide, and
    // the durability check below would pass even if load() found nothing.)
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();

    uint8_t lastTheme = 0;
    for (int i = 0; i < 2 * SettingsStore::kRecordsPerSector + 1; ++i) {
        lastTheme = static_cast<uint8_t>((i % 3) + 1);
        REQUIRE(store.commit(tweaked(lastTheme)));
        // Deliberately never call runDeferredErase().
    }
    REQUIRE(lastTheme != h0::kDefaults.themeId); // guards the premise above

    // Exactly one forced erase across the whole run, not one per commit --
    // that distinction is the entire point of deferring erases at all.
    CHECK(f.eraseCount() == 1);

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == lastTheme);
}

TEST_CASE("a ghost record with an erased-high seq loses to the CRC, not to seq") {
    // Erase only sets bits (0xFF), so seq -- 4 bytes wide -- can only grow as
    // erasure proceeds. eraseSector's own byte-serial dieAfter model can never
    // produce a record whose magic and len are untouched while only seq has
    // gained bits: seq sits between them in the layout (magic 0-3, seq 4-7,
    // len 8-9), so any single contiguous sweep that reaches seq has already
    // touched one of its neighbours. Real NOR erase is a single electrical
    // pulse, not byte-serial, so that specific interrupted state IS reachable
    // on real hardware -- forceErasedRange constructs it directly rather than
    // leaving it untested because the fake's sequential model can't reach it.
    //
    // If a scanner trusted seq alone, this ghost -- seq erased to
    // 0xFFFFFFFF, the maximum possible value -- would beat every real record
    // ever written. It must not: recordCrc() covers seq, and only seq changed,
    // so the stored CRC no longer matches.
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();
    REQUIRE(store.commit(tweaked(2))); // live record: slot 0, seq 1

    uint8_t ghost[h0::kRecordBytes];
    h0::encodeRecord(tweaked(9), /*seq=*/2, ghost);
    const uint32_t slot1 = BASE + static_cast<uint32_t>(h0::kRecordBytes);
    REQUIRE(f.programPage(slot1, ghost));
    f.forceErasedRange(slot1 + 4, 4); // just the seq field -> 0xFFFFFFFF

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 2); // the live record, not the ghost
}
