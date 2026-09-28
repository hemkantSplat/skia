/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkGlowShaderPriv_DEFINED
#define SkGlowShaderPriv_DEFINED

#include "include/effects/SkRuntimeEffect.h"

// SkGlowShader's programs; SkKnownRuntimeEffects registers them under stable keys.
namespace SkGlowShaderPriv {

// Lobe-count bins, one erf program each; a glow takes the smallest bin that holds its lobes.
inline constexpr int kErfBins[] = {4, 8, 16, 32};

SkRuntimeEffect* MakeErfEffect(int lobes, const SkRuntimeEffect::Options&);
SkRuntimeEffect* MakeRadialEffect(const SkRuntimeEffect::Options&);

}  // namespace SkGlowShaderPriv

#endif
