/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkTransformBlurImageFilter_DEFINED
#define SkTransformBlurImageFilter_DEFINED

#include "include/effects/SkRuntimeEffect.h"

// SkImageFilters::TransformBlur's limits and program; SkKnownRuntimeEffects registers the program.
namespace SkTransformBlurPriv {

// Most path entries (uniform shutter times) one blur takes.
inline constexpr int kMaxEntries = 16;
// Most taps per output pixel.
inline constexpr int kMaxSamples = 64;
// Target arc length between a pixel's taps, in layer pixels.
inline constexpr float kSampleSpacing = 1;
// Slots are equal in blend * arc / length + (1 - blend) * shutter time: pure arc length leaves
// slow stretches (dwell) under-sampled once maxSamples caps K, and half time restores them.
inline constexpr float kArcTimeBlend = 0.5f;

SkRuntimeEffect* MakeEffect(const SkRuntimeEffect::Options&);

}  // namespace SkTransformBlurPriv

#endif
