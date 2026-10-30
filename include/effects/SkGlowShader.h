/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkGlowShader_DEFINED
#define SkGlowShader_DEFINED

#include "include/core/SkAlphaType.h"
#include "include/core/SkColor.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypes.h"

class SkImage;
class SkShader;

/**
 *  Single-draw glows in local coordinates: Gaussian-blurred boxes in closed form, or a radial
 *  profile image, each plus an optional sharp rounded-box core. Output is premultiplied, unclamped.
 */
class SK_API SkGlowShader {
public:
    /** Most lobes one erf glow evaluates. */
    static constexpr int kMaxLobes = 32;

    /** Colours here are premultiplied, in the destination's working space. */
    using PMColor = SkRGBA4f<kPremul_SkAlphaType>;

    /** color × weight × (box ⊗ Gaussian(sigma)); sigma > 0. */
    struct Lobe {
        SkRect  box;
        PMColor color;
        float   sigma;
        float   weight;
    };

    /** A sharp rounded box at full strength; its edge ramps over half a device pixel. */
    struct Core {
        PMColor     color = {0, 0, 0, 0};
        SkRect      box = SkRect::MakeEmpty();
        float       radii[4] = {0, 0, 0, 0};  // one circular radius per corner: TL, TR, BR, BL
        float       pxScale = 1;              // device pixels per local unit
    };

    /** The sum of at most kMaxLobes lobes plus the core; null for invalid input. */
    static sk_sp<SkShader> Make(SkSpan<const Lobe> lobes, const Core* core = nullptr);

    /**
     *  Texel i of `profile` (row-major, wrapped at its width) is the colour at distance i × step,
     *  interpolated and zero from texel length - 1 on, times `tint`; plus the core.
     */
    static sk_sp<SkShader> MakeRadial(sk_sp<SkImage> profile, int length, float step,
                                      const PMColor& tint, const Core* core = nullptr);

private:
    SkGlowShader() = delete;
};

#endif
