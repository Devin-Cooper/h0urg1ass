// Renders faces to PBM files so they can be looked at.
//
// The golden baselines prove a face has not *changed*; they cannot tell you
// whether it looks any good. This produces images for that judgement, which is
// the only way to settle a question of taste.
//
//   ./render_faces <outdir>

#include <1bit/core/framebuffer.hpp>

#include "faces/digits_face.hpp"
#include "faces/hourglass_face.hpp"
#include "faces/setting_face.hpp"
#include "faces/splitflap_face.hpp"
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

    // Hourglass across the drain, paused so the frames are comparable.
    const struct { float f; const char* tag; } levels[] = {
        {1.00f, "100"}, {0.85f, "085"}, {0.70f, "070"}, {0.50f, "050"},
        {0.30f, "030"}, {0.15f, "015"}, {0.00f, "000"},
    };
    for (const auto& l : levels) {
        h0::HourglassFace::renderAt(fb, l.f, true, 0);
        emit(fb, dir, std::string("hourglass-") + l.tag);
    }

    // Digits in every state the face can be in.
    h0::DigitsFace digits;
    h0::TimerModel t;
    t.setDuration(12 * 60 * SEC);
    digits.render(fb, t, 0);
    emit(fb, dir, "digits-idle");

    t.start(0);
    digits.render(fb, t, 90 * SEC);
    emit(fb, dir, "digits-running");

    t.pause(90 * SEC);
    digits.render(fb, t, 90 * SEC);
    emit(fb, dir, "digits-paused");

    t.resume(90 * SEC);
    t.tick(13 * 60 * SEC);
    digits.render(fb, t, 13 * 60 * SEC);
    emit(fb, dir, "digits-expired");

    // Split-flap, mid-tick and settled, so the flip animation is visible.
    h0::SplitFlapFace flap;
    h0::TimerModel ft;
    ft.setDuration(12 * 60 * SEC);
    ft.start(0);
    flap.render(fb, ft, 0);
    emit(fb, dir, "splitflap-running");
    flap.render(fb, ft, 60 * SEC);        // a tick has just landed
    emit(fb, dir, "splitflap-tick");
    flap.render(fb, ft, 60 * SEC + 40'000ull); // mid-flip
    emit(fb, dir, "splitflap-midflip");

    // The picker, at rest and mid-drag.
    { h0::PickerState p; p.minutes = 0;  p.seconds = 0;  h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-a-zero");
    { h0::PickerState p; p.minutes = 12; p.seconds = 30; h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-b-1230");
    { h0::PickerState p; p.minutes = 12; p.seconds = 30;
      p.minutesOffset = -13; p.activeColumn = 1; h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-c-dragging");
    { h0::PickerState p; p.hours = 1; p.minutes = 45; p.seconds = 5;
      p.activeColumn = 2; h0::SettingFace::renderAt(fb, p); }
    emit(fb, dir, "pick-d-hours");

    return 0;
}
