#pragma once

#include <cstddef>
#include <cstdint>

namespace board {

/// Hardware gesture codes reported by the controller.
///
/// ⚠ The vendor header and the CST816S register document **disagree** on the
/// first two: the header calls 0x01 "down" and 0x02 "up", the register document
/// says 0x01 is slide-up and 0x02 is slide-down. One of them is wrong and only
/// hardware settles it. Nothing in this project depends on those two yet -- the
/// dial uses raw coordinates -- so the ambiguity is recorded rather than
/// guessed at.
enum class TouchGesture : uint8_t {
    None = 0x00,
    SlideA = 0x01,      ///< up or down; see the warning above
    SlideB = 0x02,      ///< the other one
    SlideLeft = 0x03,
    SlideRight = 0x04,
    Click = 0x05,
    DoubleClick = 0x0B,
    LongPress = 0x0C,
};

struct TouchPoint {
    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
    TouchGesture gesture = TouchGesture::None;

    /// True only on the poll where `gesture` first became non-None.
    ///
    /// The register may latch until the next gesture, and main.cpp polls every
    /// frame while a finger is down -- so a level-triggered reader turns one
    /// physical swipe into a dozen commands. Anything acting on a gesture must
    /// test this, not `gesture` alone.
    bool gestureIsNew = false;

    /// True when x and y are on the panel.
    ///
    /// Previously an off-panel coordinate forced `pressed` to false, which made
    /// a corrupt sample indistinguishable from a finger lift. That is fine for a
    /// drag -- both mean "do not move the wheel" -- and wrong for anything
    /// latched per touch: one bad sample mid-drag would look like a release, and
    /// the next gesture code would be accepted with the finger still down.
    ///
    /// Consumers that need a POSITION must test `pressed && positionValid`.
    /// Consumers that need to know whether the finger is DOWN test `pressed`.
    bool positionValid = false;
};

/// CST816-family capacitive touch, I2C 0x15.
///
/// **Single contact only, and that is a hardware limit rather than a driver
/// simplification.** The register map defines FingerNum as "0: no finger,
/// 1: one finger" -- there is no encoding for a second. Any interaction here
/// has to work with one point plus a gesture code.
///
/// The controller also **auto-sleeps after 2 s of no touch** by default, which
/// costs a noticeable delay on the first sample of a drag. `begin()` disables
/// that, because a dial that ignores the start of your gesture feels broken.
class Cst816 {
public:
    /// Reset the part, verify its ID and keep it awake.
    ///
    /// The reset pulse is not optional: held in reset the controller does not
    /// ACK its own address at all, so a bus scan reports it absent and it reads
    /// as a dead part rather than a held one.
    bool begin();

    /// Poll for a touch. Returns false on a bus error.
    bool read(TouchPoint& out);

    uint8_t chipId() const { return chipId_; }

    /// Hold the controller in its 1.6 mA dynamic mode, or let it fall back to
    /// its own 6 uA standby after two idle seconds.
    ///
    /// Awake costs 1.6 mA against 6 uA, which is most of the touch subsystem's
    /// budget -- but standby adds a wake delay to the first sample of a drag,
    /// and a picker that ignores the first few millimetres of a gesture reads
    /// as broken. So it is held awake only while the picker is actually live,
    /// which is the one posture touch is used in.
    ///
    /// Deliberately NOT the part's sleep mode: that is 1 uA but recoverable
    /// only by pulsing TP_RST, which would remove touch-to-wake entirely.
    bool setHeldAwake(bool awake);

private:
    bool readRegs(uint8_t reg, uint8_t* dst, size_t len);
    bool writeReg(uint8_t reg, uint8_t value);

    uint8_t chipId_ = 0;
    bool heldAwake_ = false;
    TouchGesture lastGesture_ = TouchGesture::None;
};

} // namespace board
