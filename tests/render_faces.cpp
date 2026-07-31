// Renders faces to PBM files so they can be looked at.
//
// The golden baselines prove a face has not *changed*; they cannot tell you
// whether it looks any good. This produces images for that judgement, which is
// the only way to settle a question of taste.
//
//   ./render_faces <outdir>

#include <1bit/core/framebuffer.hpp>

#include "app/settings_ui.hpp"
#include "faces/setting_face.hpp"
#include "faces/settings_face.hpp"
#include "faces/timer_face.hpp"
#include "power/battery_model.hpp"
#include "render/raster_ops.hpp"
#include "sand/sand_sim.hpp"
#include "timer/timer_model.hpp"

#include <cstdio>
#include <string>

namespace {

constexpr uint64_t SEC = 1'000'000ull;
using Panel = onebit::Framebuffer<240, 280>;

/// Binary PBM (P4): one bit per pixel, 1 = black, rows padded to whole bytes.
/// Matches the framebuffer's own packing closely enough to be a near-copy, and
/// every image tool on the planet reads it.
bool writePbm(const onebit::IFramebuffer& fb, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P4\n%d %d\n", fb.width(), fb.height());
    const int stride = (fb.width() + 7) / 8;
    for (int16_t y = 0; y < fb.height(); ++y) {
        for (int b = 0; b < stride; ++b) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int16_t x = static_cast<int16_t>(b * 8 + bit);
                if (x < fb.width() && fb.getPixel(x, y) == onebit::BLACK) {
                    byte |= static_cast<uint8_t>(0x80u >> bit);
                }
            }
            std::fputc(byte, f);
        }
    }
    std::fclose(f);
    return true;
}

void emit(const onebit::IFramebuffer& fb, const std::string& dir, const std::string& name) {
    const std::string path = dir + "/" + name + ".pbm";
    std::printf("%s %s\n", writePbm(fb, path) ? "wrote" : "FAILED", path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : ".";
    Panel fb;

    // The picker, at rest and mid-drag.
    { h0::PickerState p; p.minutes = 0;  p.seconds = 0;  h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-a-zero");
    { h0::PickerState p; p.minutes = 12; p.seconds = 30; h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-b-1230");
    { h0::PickerState p; p.minutes = 12; p.seconds = 30;
      p.minutesOffset = -13; p.activeColumn = 1; h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-c-dragging");
    { h0::PickerState p; p.minutes = 99; p.seconds = 5;
      p.activeColumn = 2; h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-d-99min");

    // The composed face -- readout over sand -- through a whole drain, plus the
    // states and postures that only exist as a composite.
    {
        h0::TimerModel st;
        st.setDuration(300 * SEC);
        st.start(0);
        h0::TimerFace face;
        face.restart(st, 1234);
        const char* tags[] = {"a-full", "b-quarter", "c-half", "d-most", "e-done"};
        const uint64_t marks[] = {0, 75 * SEC, 150 * SEC, 240 * SEC, 300 * SEC};
        uint64_t now = 0;
        for (int m = 0; m < 5; ++m) {
            while (now < marks[m]) { now += 33'333ull; face.tick(st, now); }
            face.render(fb, st, now);
            emit(fb, dir, std::string("composed-") + tags[m]);
        }

        // Expired: the safe box inverts.
        st.tick(400 * SEC);
        face.render(fb, st, 400 * SEC);
        emit(fb, dir, "composed-expired");
    }

    // Sand packed against the housing, which is what tilt and the inverted
    // posture actually look like.
    {
        h0::TimerModel st;
        st.setDuration(300 * SEC);
        st.start(0);
        h0::TimerFace face;
        face.restart(st, 77);
        face.setGravity(h0::Gravity::N);
        uint64_t now = 0;
        while (now < 90 * SEC) { now += 33'333ull; face.tick(st, now); }
        face.render(fb, st, now);
        emit(fb, dir, "composed-tilt-N");

        face.setGravity(h0::Gravity::SE);
        while (now < 150 * SEC) { now += 33'333ull; face.tick(st, now); }
        face.render(fb, st, now);
        emit(fb, dir, "composed-tilt-SE");

        // And the whole frame as the inverted posture shows it.
        h0::rotate180(fb);
        emit(fb, dir, "composed-rotated");
    }

    // The settings face.
    {
        h0::SettingsUi ui;
        ui.open(h0::kDefaults);
        const h0::BatteryReading bat{3820, 3940, false, true, true};
        h0::SettingsFace::renderAt(fb, ui, bat);
        emit(fb, dir, "settings-theme");
    }

    return 0;
}
