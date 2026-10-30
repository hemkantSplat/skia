/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkOutputTransform_DEFINED
#define SkOutputTransform_DEFINED

#include "include/core/SkM44.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypes.h"

class SkColorFilter;
class SkImage;

/**
 *  The stages between a composited frame and the presented pixels, as colour filters that compose
 *  with SkColorFilters::Compose in stage order: decode → grade → tone map → encode → LUT. Each
 *  unpremultiplies (colour with zero alpha becomes zero), applies its stage and premultiplies.
 *  An 8-bit present dithers through the paint: draw the frame as an image shader with dither on.
 */
class SK_API SkOutputTransform {
public:
    /** sRGB-encoded surface colour to linear light; values above 1 are light and stay linear. */
    static sk_sp<SkColorFilter> Decode();

    /** Linear light to sRGB-encoded colour: the sRGB curve on max(c, 0), unclamped above 1. */
    static sk_sp<SkColorFilter> Encode();

    /** three.js's tone-mapping curves (r182), on scene-linear sRGB primaries. */
    enum class ToneCurve { kLinear, kReinhard, kCineon, kACES, kAgX, kNeutral, kLast = kNeutral };

    /** A linear gain of `exposure`, then `curve`; null for a non-finite exposure. */
    static sk_sp<SkColorFilter> ToneMap(ToneCurve curve, float exposure);

    /** Floats in the parametric grade's uniform block. */
    static constexpr int kGradeUniformCount = 30;

    /**
     *  The parametric grade on scene-linear Rec.709: `uniforms` is the packed block (balance 3x3,
     *  lift, slope, power, contrast, saturation, vibrance, hue 3x3; matrices column-major) and
     *  `curves` the 256x2 table strip (row 0 red/green/blue, row 1 master), sampled at texel
     *  centres. Null for a wrong-sized block or no curves.
     */
    static sk_sp<SkColorFilter> Grade(SkSpan<const float> uniforms, sk_sp<SkImage> curves);

    /**
     *  A creative 3D LUT of edge `size` packed as a size² x size strip of blue slices (pixel
     *  (b·size + r, g) is entry (r, g, b)), trilinear over [domainMin, domainMax] and mixed with
     *  the input by `intensity`. Null for an invalid strip or size.
     */
    static sk_sp<SkColorFilter> LutStrip(sk_sp<SkImage> strip, int size, float intensity,
                                         const SkV3& domainMin = {0, 0, 0},
                                         const SkV3& domainMax = {1, 1, 1});

private:
    SkOutputTransform() = delete;
};

#endif
