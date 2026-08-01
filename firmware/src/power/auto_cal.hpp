#pragma once

#include <cstdint>

namespace h0 {

/// Learns the battery divider's gain from the charger, with no meter.
///
/// The divider's two resistors are +/-5% each, so the ratio ranges 2.81..3.21
/// -- a -6.3%/+7.0% GAIN error, which at 3.7 V is +/-0.26 V and swamps the
/// whole useful discharge curve. It is a gain term, so one multiplier fixes it,
/// and until now that multiplier had to come from a DMM. In practice it never
/// did, so batCalPermille stayed at 1000 and the gauge stayed a rumour.
///
/// The reference was on the board all along. During its constant-voltage phase
/// the ETA6096 actively regulates the battery terminal -- the exact node the
/// divider taps, since BATS is a separate sense wire specifically to avoid
/// drop -- to 4.21 V typ, 4.17 min, 4.25 max. That is +/-0.95% part to part,
/// which is the accuracy ceiling here and is comparable to a careful manual
/// calibration.
///
/// **Detection is a rise followed by a plateau, and deliberately ignores
/// `onUsb`.** That flag reads USB *enumeration*, so a dumb charger or a
/// suspended host looks exactly like battery -- and it is not needed anyway:
/// only a charger can raise a cell's terminal voltage, so the rise is a
/// stronger signal than the flag would have been. It also disposes of the
/// pack-disconnected case for free, where the charger drives an open circuit
/// straight to CV with no climb.
class AutoCal {
public:
    /// The ETA6096's CV setpoint. Datasheet Electrical Characteristics,
    /// "Battery CV Voltage", at I_BAT = 0 mA.
    static constexpr uint16_t kCvMv = 4210;

    /// A climb of this much above the running minimum means a charger.
    static constexpr uint16_t kRiseMv = 100;

    /// The plateau's flatness band. About 3 LSB at 2.417 mV/LSB.
    static constexpr uint16_t kFlatMv = 8;

    /// How long the plateau must hold. Samples arrive at 1 Hz, so 5 minutes.
    /// CV lasts until the current tapers to the 130 mA termination, which on
    /// this pack is many minutes, so the margin is large.
    static constexpr uint32_t kFlatSamples = 300;

    /// A fall of this much below the session's peak ends the charge session.
    static constexpr uint16_t kFallMv = 100;

    /// How far a newly learned gain must differ from the stored one to be
    /// worth a flash write.
    static constexpr uint16_t kCalDeadband = 3;

    /// Feed the RAW, uncorrected, filtered reading once per second.
    ///
    /// Returns a gain in permille worth storing, or 0 for nothing this sample.
    /// Non-zero at most once per charge session.
    uint16_t push(uint16_t rawMv);

    /// True while a charge session is in progress.
    ///
    /// This is what `BatteryReading::charging` is sourced from. No static
    /// voltage threshold can do this job: a full resting cell and a cell held
    /// at CV differ by tens of millivolts, and the old 4220 mV threshold sat
    /// ABOVE the charger's own 4210 mV regulation point, so on a typical part
    /// it could never fire at all.
    bool charging() const { return charging_; }

    /// Whether a learned gain should be persisted over the stored one.
    ///
    /// `autoArmed` is `Settings::batCalAuto`. The check lives HERE, not at the
    /// call site in board/battery.cpp, because no board code is compiled into
    /// the host suite -- putting "manual wins" there would make the one rule
    /// the user explicitly asked for the one rule nothing can test.
    static bool shouldStore(uint16_t learned, uint16_t stored, bool autoArmed);

private:
    void endSession();

    bool primed_ = false;
    bool charging_ = false;
    bool anchored_ = false; ///< this session already produced a gain
    uint16_t min_ = 0;      ///< running minimum, the rise is measured from here
    uint16_t peak_ = 0;     ///< session maximum, the fall is measured from here
    uint16_t flatRef_ = 0;  ///< the value the flatness band is centred on
    uint32_t flatFor_ = 0;  ///< consecutive samples inside the band
};

} // namespace h0
