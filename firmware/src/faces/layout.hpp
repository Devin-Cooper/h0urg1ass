#pragma once

#include <cstdint>

namespace h0 {

/// The rounded corners physically clip the panel at a ~44 px radius, measured
/// on hardware. Minimum diagonal clearance is r - r/sqrt(2) = 12.9 px, and a
/// 16 px inset was confirmed fully visible with margin. Nothing readable or
/// touchable goes outside this box.
///
/// The clip is a rounded RECTANGLE: within a corner quadrant the visible region
/// is a disc of radius 44 centred 44 px inwards from that corner, not the
/// complement of a disc centred on the corner point. Anything spanning
/// x in [44, 196] clears both quadrants at every y.
namespace safe {
inline constexpr int16_t INSET = 16;
inline constexpr int16_t X = INSET;
inline constexpr int16_t Y = INSET;
inline constexpr int16_t W = 240 - 2 * INSET; // 208
inline constexpr int16_t H = 280 - 2 * INSET; // 248

inline constexpr int16_t CORNER_R = 44;
} // namespace safe

} // namespace h0
