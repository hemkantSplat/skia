/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrDstReadCause_DEFINED
#define GrDstReadCause_DEFINED

#include "include/core/SkBlendMode.h"

#include <cstdint>

/** Why a draw needs its destination as a texture; the XP factory or colour FP that asked decides it. */
enum class GrDstReadReason : uint8_t {
    kNone,
    kBlender,        // A colour FP reads the destination: a runtime or other non-mode SkBlender.
    kPlusClamp,      // kPlus saturates in the shader on a target whose hardware blend does not clamp.
    kCoverageBlend,  // The mode's coverage formula needs a second output; no dual-source blending.
    kLCD,            // LCD coverage with a mode or colour fixed-function blending cannot express.
    kAdvancedBlend,  // An advanced mode (multiply, screen, ...) without hardware blend equations.
    kLightAddClamp,  // The light-add XP on a clamped-float target.
    kLast = kLightAddClamp,
};

/**
 * A draw's destination read: the reason plus the blend it reads for. The blend is an SkBlendMode or
 * one of the non-mode blends below. Packs into 9 bits (reason 3, blend 6) for GrProcessorSet::Analysis.
 */
struct GrDstReadCause {
    static constexpr uint8_t kAddBlend = (uint8_t)SkBlendMode::kLastMode + 1;  // SkBlenders::Add
    static constexpr uint8_t kBlenderBlend = kAddBlend + 1;   // A non-mode SkBlender.
    static constexpr uint8_t kLightAddBlend = kAddBlend + 2;  // SkBlenders::LightResolve's add.
    static constexpr uint8_t kMinBlend = kAddBlend + 3;       // SkBlenders::Min
    static constexpr uint8_t kUnknownBlend = kAddBlend + 4;
    static_assert(kUnknownBlend < 64);

    GrDstReadReason fReason = GrDstReadReason::kNone;
    uint8_t fBlend = kUnknownBlend;

    static constexpr GrDstReadCause Mode(GrDstReadReason reason, SkBlendMode mode) {
        return {reason, (uint8_t)mode};
    }

    static constexpr int kPackedBits = 9;
    constexpr uint16_t pack() const { return (uint16_t)((uint16_t)fReason | (fBlend << 3)); }
    static constexpr GrDstReadCause Unpack(uint16_t bits) {
        return {(GrDstReadReason)(bits & 7), (uint8_t)(bits >> 3)};
    }

    static const char* ReasonName(GrDstReadReason);
    static const char* BlendName(uint8_t blend);
};
static_assert((int)GrDstReadReason::kLast < 8);

#endif
