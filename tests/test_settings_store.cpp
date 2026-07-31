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
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();

    CHECK(store.settings() == h0::kDefaults);
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
    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 2);
}

TEST_CASE("the highest seq wins, not the last page scanned") {
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();
    for (uint8_t i = 0; i < 5; ++i) REQUIRE(store.commit(tweaked(i % 4)));

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == 4 % 4);
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
    FakeFlash f(BASE, SPAN);
    uint8_t junk[h0::kRecordBytes];
    for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = static_cast<uint8_t>(i * 7 + 1);
    for (int i = 0; i < SettingsStore::kRecordsPerSector; ++i) {
        f.programPage(BASE + static_cast<uint32_t>(i) * 256, junk);
    }

    SettingsStore store(f);
    store.load();
    CHECK(store.settings() == h0::kDefaults);
    CHECK(f.eraseCount() == 0);
}

TEST_CASE("a device that is never idle still saves, forced erase and all") {
    // The pathological case: both sectors full and runDeferredErase never
    // gated in. Eating a 400 ms stall beats silently failing to save.
    FakeFlash f(BASE, SPAN);
    SettingsStore store(f);
    store.load();

    for (int i = 0; i < 2 * SettingsStore::kRecordsPerSector + 1; ++i) {
        REQUIRE(store.commit(tweaked(static_cast<uint8_t>(i % 4))));
        // Deliberately never call runDeferredErase().
    }
    CHECK(f.eraseCount() >= 1); // it forced one rather than giving up

    SettingsStore reopened(f);
    reopened.load();
    CHECK(reopened.settings().themeId == (2 * SettingsStore::kRecordsPerSector) % 4);
}
