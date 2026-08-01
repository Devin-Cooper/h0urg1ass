#pragma once

#include <cstdint>

namespace h0 {

/// The lowest voltage this device has actually run at, and the cutoff derived
/// from it.
///
/// The point of learning it rather than picking one is that the interesting
/// number -- where THIS board, under THIS load, stops working -- is a property
/// of the LDO's dropout and the pack's internal resistance, not of a generic
/// discharge curve.
///
/// **The floor is a lifetime minimum, not a per-cycle value**, and that is what
/// makes it affordable to persist. It is compared against the STORED floor, so
/// once the cutoff arms at floor + margin the device never goes that low again
/// and never writes again. The whole learning burst happens once, on the first
/// run to empty.
///
/// Stored in RAW units, deliberately. A floor learned before the gain is known
/// stays correct after it, because the correction is applied at use.
///
/// Stateless: the only state is the caller's stored value, which lives in
/// Settings because it has to survive the brownout that produced it.
class BatteryFloor {
public:
    /// Only the bottom of the curve is interesting. Staying out of the top
    /// keeps writes off the normal operating range entirely.
    static constexpr uint16_t kTrackBelowMv = 3700;

    /// How far a new low must beat the stored floor to be worth a write.
    ///
    /// 50, not 25: a descent from kTrackBelowMv to a ~3.38 V brownout costs 7
    /// writes at 50 and 13 at 25, and SettingsStore's wear note assumes one
    /// page program per settings SESSION. The floor only has to resolve to
    /// well inside the 250 mV margin, so the precision costs nothing.
    static constexpr uint16_t kStepMv = 50;

    /// Added to the learned floor to make the cutoff. A floor near 3.38 V --
    /// roughly where the RT9193 drops out at this load -- puts the cutoff near
    /// 3.63 V, about 10% SoC on a standard curve. Deliberately conservative:
    /// nominal empty should be real 10%, not real 0%.
    static constexpr uint16_t kMarginMv = 250;

    /// Below this the LDO's dropout starts corrupting the ADC reference, which
    /// makes the battery read artificially HIGH exactly when it is nearly flat.
    static constexpr uint16_t kCutoffMinMv = 3450;

    /// Above this the device would refuse to run on a healthy pack.
    static constexpr uint16_t kCutoffMaxMv = 3750;

    /// A new raw floor worth persisting, or 0 for nothing to do.
    ///
    /// `storedRaw` is `Settings::batFloorRawMv`; 0 means never learned.
    static uint16_t update(uint16_t rawMv, uint16_t correctedMv, uint16_t storedRaw);

    /// The armed cutoff in CORRECTED mV, or 0 when no floor has been learned --
    /// which is what leaves the low-battery route disabled until the first
    /// run to empty has taught it something.
    ///
    /// `storedRaw` must be a floor that SURVIVED A POWER CYCLE, never one
    /// learned during the current descent. The returned cutoff is always above
    /// the reading that produced the floor, so arming on a fresh one would end
    /// the run that is supposed to reach brownout -- see PowerPolicy::update,
    /// which takes it from `PowerInput::armedFloorRawMv` for exactly this
    /// reason.
    ///
    /// `permille` must be the gain the reading this cutoff will be compared
    /// against was corrected with. Correct the two with different gains and
    /// they no longer move together, which is a threshold that shifts under
    /// the value it is measuring.
    static uint16_t cutoffMv(uint16_t storedRaw, uint16_t permille);
};

} // namespace h0
