#pragma once

#include <1bit/core/framebuffer.hpp>

#include "sand/sand_grid.hpp"

namespace h0 {

/// Where the sand grid lives on the panel, and what the vessel looks like.
///
/// The vessel is a **horizontal floor with a hole**, not an hourglass outline.
/// A bowtie wastes most of its area on taper, and its sloping walls are what
/// strand grains; a flat floor gives full-width chambers and a shape that can be
/// described in two lines.
namespace sandgeom {

/// 2 px per cell, and the grid sits exactly on the measured 16 px safe inset --
/// so 208 x 248 px of sand, clear of the rounded corners.
inline constexpr int16_t SCALE = 2;
inline constexpr int16_t ORIGIN_X = 16;
inline constexpr int16_t ORIGIN_Y = 16;

static_assert(SandGrid::W * SCALE == 208, "sand grid must span the safe width");
static_assert(SandGrid::H * SCALE == 248, "sand grid must span the safe height");

// The origin is byte-aligned in the framebuffer (16 / 8 = 2), which is what
// lets a row be expanded with byte-wide table lookups instead of per-pixel
// writes. Losing that alignment would cost roughly an order of magnitude.
static_assert(ORIGIN_X % 8 == 0, "row expansion assumes a byte-aligned origin");

inline constexpr int FLOOR_ROW = SandGrid::H / 2;
inline constexpr int HOLE_CX = SandGrid::W / 2;

} // namespace sandgeom

/// Build the vessel: border, floor, and a hole of the given half-width.
SandGrid makeVessel(int holeHalfWidth);

/// Draw sand and vessel into `fb`.
///
/// Expands each grid row into two framebuffer rows a byte at a time through a
/// bit-doubling table. Both the grid and the framebuffer are bit-packed
/// MSB-first, so this is a table lookup per byte rather than four `setPixel`
/// calls per grain -- about 3,200 lookups for a full frame against roughly
/// 18,000 virtual calls.
void renderSand(onebit::IFramebuffer& fb, const SandGrid& sand, const SandGrid& walls);

} // namespace h0
