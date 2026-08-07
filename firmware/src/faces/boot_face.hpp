#pragma once

#include <1bit/core/framebuffer.hpp>

namespace h0 {

/// The power-on splash.
///
/// It exists because main() holds a 3 s window with USB up before touching any
/// bus that can wedge -- that window is what makes picotool reflashing work
/// without the BOOT button, and the watchdog reboots into it. Held dark, it is
/// indistinguishable from the device still being off.
///
/// Deliberately static and argument-free: it draws before the IMU, the touch
/// controller and the battery are up, so there is nothing yet to report.
class BootFace {
public:
    static void renderAt(onebit::IFramebuffer& fb);
};

} // namespace h0
