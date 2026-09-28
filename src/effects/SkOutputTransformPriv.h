/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkOutputTransformPriv_DEFINED
#define SkOutputTransformPriv_DEFINED

#include "include/effects/SkOutputTransform.h"
#include "include/effects/SkRuntimeEffect.h"

// The output-transform stages' programs, registered as known runtime effects (kOutput*, kTone*).
namespace SkOutputTransformPriv {

SkRuntimeEffect* MakeDecodeEffect(const SkRuntimeEffect::Options&);
SkRuntimeEffect* MakeEncodeEffect(const SkRuntimeEffect::Options&);
SkRuntimeEffect* MakeToneEffect(SkOutputTransform::ToneCurve, const SkRuntimeEffect::Options&);
SkRuntimeEffect* MakeGradeEffect(const SkRuntimeEffect::Options&);
SkRuntimeEffect* MakeLutEffect(const SkRuntimeEffect::Options&);

}  // namespace SkOutputTransformPriv

#endif
