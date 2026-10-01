/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkBlenders_DEFINED
#define SkBlenders_DEFINED

#include "include/core/SkBlender.h"

class SK_API SkBlenders {
public:
    /**
     *  Create a blender that implements the following:
     *     k1 * src * dst + k2 * src + k3 * dst + k4
     *  @param k1, k2, k3, k4 The four coefficients.
     *  @param enforcePMColor If true, the RGB channels will be clamped to the calculated alpha.
     */
    static sk_sp<SkBlender> Arithmetic(float k1, float k2, float k3, float k4, bool enforcePremul);

    /**
     *  Create a blender that returns src + dst per premul channel, clamped only where the
     *  destination cannot store values above 1 (unlike SkBlendMode::kPlus, which always saturates).
     *  Ganesh maps it to fixed-function (ONE, ONE), so float targets need no destination read.
     */
    static sk_sp<SkBlender> Add();

    /**
     *  Create a blender that returns min(src, dst) per premul channel. Ganesh maps it to the
     *  fixed-function min equation, so an uncovered draw needs no destination read. Drawing
     *  opaque white with it clamps the destination to 1 on float targets.
     */
    static sk_sp<SkBlender> Min();

    /**
     *  Where light-resolve colour lives: sRGB-encoded surface colour (light above coverage kept
     *  linear), or linear light.
     */
    enum class LightSpace { kEncoded, kLinear };

    /**
     *  Summed light stored in `light` space added to a destination in `into` space. Emissive light
     *  adds no coverage: the destination's alpha survives. Otherwise the sum adds coverage like
     *  kPlus, with alpha clamped to 1 and colour to alpha. Ganesh draws the emissive same-space
     *  form with fixed-function blending, without a destination read.
     */
    static sk_sp<SkBlender> LightResolve(LightSpace into, LightSpace light, bool emissive);

    /**
     *  An isolated layer resolved into `into`: covered pixels composite source-over, uncovered
     *  light adds as linear light. Into a linear destination this is exactly kSrcOver.
     */
    static sk_sp<SkBlender> LayerResolve(LightSpace into);

private:
    SkBlenders() = delete;
};

#endif
