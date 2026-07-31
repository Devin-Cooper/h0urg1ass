#pragma once

#include <cstdint>

namespace board {

class St7789_1in69;
class Cst816;
class Qmi8658;
class Buzzer;

/// Key1 on GPIO14, and the only programmable button on the board.
///
/// Key2 (BOOT) is QSPI_SS_N and Key3 (RESET) is the RUN pin -- neither is a
/// GPIO, so neither can be read. RESET additionally powers the board OFF on
/// battery rather than rebooting it, because the always-on domain reset
/// releases the GPIO15 latch before a single instruction runs. That is not
/// fixable in firmware; holding PWR while tapping RESET is the reboot, because
/// D1 holds Q3 on independently of the latch.
class PowerButton {
public:
    void begin();

    /// Debounced. The loop polls at ~30 Hz against a 100-300 ms press, and
    /// contact bounce behind a pull-up is 1-5 ms -- well inside one frame -- so
    /// two agreeing samples is insurance rather than necessity.
    bool isDown();

    /// Run the shutdown sequence and drop the battery latch. Never returns on
    /// battery. On USB it returns, because D4 keeps VSYS alive regardless.
    ///
    /// MUST be called only after the button has been RELEASED. While Key1 is
    /// held, D1 pulls Q3's gate low independently of the latch, so dropping
    /// GPIO15 with the finger down does nothing and the board dies later, at
    /// release, apparently at random. `h0::PowerPolicy` only ever returns
    /// `PowerAction::PowerOff` on the release sample, so a caller that acts on
    /// that action alone already satisfies this precondition -- it is
    /// documented here rather than re-checked, because there is no button
    /// state left to check by the time this runs.
    void shutdown(St7789_1in69& lcd, Cst816& touch, Qmi8658& imu, Buzzer& buzzer);

private:
    bool state_ = false;
    bool last_ = false;
};

} // namespace board
