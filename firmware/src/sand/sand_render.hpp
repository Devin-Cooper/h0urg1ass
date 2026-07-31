#pragma once

#include <1bit/core/framebuffer.hpp>

#include "faces/layout.hpp"
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

/// The lintel: the housing the readout sits in, hung from the vessel ceiling.
///
/// It is a wall, not a decoration. Being part of the wall grid is what makes
/// the readout legible at one bit: sand cannot enter a wall, so the interior
/// is guaranteed empty, and `renderSand` -- which *assigns* bytes rather than
/// or-ing them -- repaints it white every frame for free. Black glyphs on black
/// sand stops being a drawing problem and becomes physically impossible.
///
/// **It hangs off the ceiling with no cell above it, and that is load-bearing.**
/// The centreline attractor marches resting grains toward HOLE_CX. Any obstacle
/// with free space above it that spans that column collects a tower of grains
/// that can never leave -- destroying the one property the upper chamber has to
/// have, which is that it empties. With the top row welded to the border, that
/// failure mode does not exist.
inline constexpr int LINTEL_CX0 = 23;
inline constexpr int LINTEL_CX1 = 81;
inline constexpr int LINTEL_CY0 = 1; ///< row 0 is the border itself
inline constexpr int LINTEL_CY1 = 26;

inline constexpr int16_t LINTEL_X = ORIGIN_X + SCALE * LINTEL_CX0;         // 62
inline constexpr int16_t LINTEL_W = SCALE * (LINTEL_CX1 - LINTEL_CX0 + 1); // 118
inline constexpr int16_t LINTEL_Y = ORIGIN_Y + SCALE * LINTEL_CY0;         // 18
inline constexpr int16_t LINTEL_H = SCALE * (LINTEL_CY1 - LINTEL_CY0 + 1); // 52

/// The interior: never drawn, never enterable, and exactly the readout's home.
inline constexpr int16_t LINTEL_IN_X = LINTEL_X + SCALE;      // 64
inline constexpr int16_t LINTEL_IN_W = LINTEL_W - 2 * SCALE;  // 114
inline constexpr int16_t LINTEL_IN_Y = LINTEL_Y;              // 18 -- no top rail
inline constexpr int16_t LINTEL_IN_H = LINTEL_H - SCALE;      // 50

static_assert(LINTEL_CY0 == 1, "the lintel must hang off the ceiling, or it strands sand");
static_assert(LINTEL_X >= safe::X && LINTEL_X + LINTEL_W <= safe::X + safe::W,
              "lintel must lie inside the safe box");
static_assert(LINTEL_X >= safe::CORNER_R && LINTEL_X + LINTEL_W <= 240 - safe::CORNER_R,
              "lintel must clear both corner quadrants at every y");

/// The readout panel. A PIXEL rect, not a grid rect: since it is composited
/// over the sand rather than carved out of the physics, it is free of the
/// ORIGIN + SCALE * cell quantisation the lintel had.
///
/// 182 x 92 at the ceiling. Budget is 204 px of vessel interior (x 18..221) --
/// the whole safe box lies inside the rounded rect, so the full width is
/// available at every row including the top edge.
inline constexpr int16_t PANEL_X = 29;
inline constexpr int16_t PANEL_Y = 18;
inline constexpr int16_t PANEL_W = 182;
inline constexpr int16_t PANEL_H = 92;

static_assert(PANEL_X >= safe::X && PANEL_X + PANEL_W <= safe::X + safe::W,
              "panel must lie inside the safe box");
static_assert(PANEL_Y >= safe::Y && PANEL_Y + PANEL_H <= safe::Y + safe::H,
              "panel must lie inside the safe box");

/// The same rect in grid space, for asking what sand is behind it.
inline constexpr int PANEL_CX0 = (PANEL_X - ORIGIN_X) / SCALE;
inline constexpr int PANEL_CY0 = (PANEL_Y - ORIGIN_Y) / SCALE;
inline constexpr int PANEL_CX1 = (PANEL_X + PANEL_W - 1 - ORIGIN_X) / SCALE;
inline constexpr int PANEL_CY1 = (PANEL_Y + PANEL_H - 1 - ORIGIN_Y) / SCALE;

static_assert(PANEL_CX0 >= 0 && PANEL_CX1 < SandGrid::W, "panel grid rect must be in bounds");
static_assert(PANEL_CY0 >= 0 && PANEL_CY1 < SandGrid::H, "panel grid rect must be in bounds");

} // namespace sandgeom

/// Build the vessel: border, floor, and a hole of the given half-width.
SandGrid makeVessel(int holeHalfWidth);

/// Fill the lintel solid. This is the PHYSICS shape -- what sand cannot enter.
void fillLintelSolid(SandGrid& w);

/// Draw the lintel's jambs and soffit only. This is the INK shape -- what the
/// eye sees. The interior is deliberately absent so it renders white.
void drawLintelOutline(SandGrid& w);

/// Draw sand and vessel into `fb`.
///
/// Expands each grid row into two framebuffer rows a byte at a time through a
/// bit-doubling table. Both the grid and the framebuffer are bit-packed
/// MSB-first, so this is a table lookup per byte rather than four `setPixel`
/// calls per grain -- about 3,200 lookups for a full frame against roughly
/// 18,000 virtual calls.
void renderSand(onebit::IFramebuffer& fb, const SandGrid& sand, const SandGrid& walls);

} // namespace h0
