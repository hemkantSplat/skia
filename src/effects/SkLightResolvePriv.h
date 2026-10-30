/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkLightResolvePriv_DEFINED
#define SkLightResolvePriv_DEFINED

#include "include/effects/SkRuntimeEffect.h"

// The light-resolve blenders' programs, as known runtime effects (kLightResolve, kLightAdd).
namespace SkLightResolvePriv {

// Every light and layer resolve: conversion mode, emissive and layer flags as uniforms.
SkRuntimeEffect* MakeResolveEffect(const SkRuntimeEffect::Options&);

// The resolve's emissive same-space form: rgb sums, the destination's alpha survives.
SkRuntimeEffect* MakeAddEffect(const SkRuntimeEffect::Options&);

}  // namespace SkLightResolvePriv

#endif
