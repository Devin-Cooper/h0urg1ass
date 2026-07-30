#pragma once

#include "faces/face.hpp"

namespace h0 {

/// A literal hourglass: sand drains from the upper bulb to the lower one as the
/// timer runs. The face the project is named after, and the one that reads from
/// across a room without parsing digits.
///
/// This is the **shaped-fill** implementation -- sand is a computed region whose
/// area tracks `remaining/total`, not a simulation. A physics-driven variant is
/// a separate, explicitly optional piece of work; this one must look good on its
/// own because it is what ships.
///
/// Two properties are load-bearing and easy to lose:
///
/// * **Area, not height, tracks the fraction.** The bulbs taper, so a linear
///   height mapping would drain visibly fast at the top and slow at the neck.
///   Both levels are solved for equal area instead.
/// * **The dither is screen-anchored.** Patterns are indexed off absolute
///   framebuffer coordinates, so the texture appears to stay still while the
///   *boundary* moves through it. Anchoring it to the sand region instead makes
///   the whole body crawl as the level changes, which is the single most
///   distracting artifact available in one bit.
class HourglassFace : public IFace {
public:
    void render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) override;
    const char* name() const override { return "hourglass"; }

    /// Meaningless without a duration to be a fraction of.
    bool supports(const TimerModel& t) const override { return t.duration() > 0; }

    /// Render at an explicit fill fraction, bypassing the timer. Used by the
    /// golden tests to pin specific levels, and by the face comparison tool.
    /// `phase` advances the falling stream's animation.
    static void renderAt(onebit::IFramebuffer& fb, float fraction, bool running,
                         uint32_t phase);
};

} // namespace h0
