#pragma once

#include <1bit/core/framebuffer.hpp>

#include <string>

/// Compare `fb` against tests/golden/<name>.txt, reporting through doctest.
///
/// Baselines are Unicode braille art. Braille packs 2x4 pixels per glyph
/// losslessly, so a baseline is the framebuffer *re-spelled*, not a rendering of
/// it -- which means an unintended change shows up as a readable diff rather
/// than as a percentage.
///
/// With ONEBIT_UPDATE_GOLDENS=1 in the environment this rewrites the baseline
/// instead of comparing. A missing baseline is always a failure, never a silent
/// create -- otherwise the first broken render quietly becomes the reference.
void checkGolden(const onebit::IFramebuffer& fb, const std::string& name);

/// Braille encoding of `fb`, for diagnostics.
std::string toBraille(const onebit::IFramebuffer& fb);
