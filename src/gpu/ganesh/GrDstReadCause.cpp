/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ganesh/GrDstReadCause.h"

const char* GrDstReadCause::ReasonName(GrDstReadReason reason) {
    switch (reason) {
        case GrDstReadReason::kNone:          return "none";
        case GrDstReadReason::kBlender:       return "blender";
        case GrDstReadReason::kPlusClamp:     return "plusClamp";
        case GrDstReadReason::kCoverageBlend: return "coverageBlend";
        case GrDstReadReason::kLCD:           return "lcd";
        case GrDstReadReason::kAdvancedBlend: return "advancedBlend";
        case GrDstReadReason::kLightAddClamp: return "lightAddClamp";
    }
    return "none";
}

const char* GrDstReadCause::BlendName(uint8_t blend) {
    if (blend <= (uint8_t)SkBlendMode::kLastMode) {
        return SkBlendMode_Name((SkBlendMode)blend);
    }
    switch (blend) {
        case kAddBlend:      return "Add";
        case kBlenderBlend:  return "Blender";
        case kLightAddBlend: return "LightAdd";
        default:             return "Unknown";
    }
}
