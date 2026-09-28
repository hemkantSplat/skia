/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrLightAddXP_DEFINED
#define GrLightAddXP_DEFINED

#include "include/core/SkRefCnt.h"
#include "src/gpu/ganesh/GrXferProcessor.h"

class GrCaps;
class GrProcessorAnalysisColor;
enum class GrClampType;
enum class GrProcessorAnalysisCoverage;

// See the comment above GrXPFactory's definition about this warning suppression.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#endif
/**
 * SkBlenders::LightResolve's emissive same-space form: dst.rgb + src.rgb, dst.a kept. The XP writes
 * (src.rgb × coverage, 0) into fixed-function (ONE, ONE); clamped float targets read dst instead.
 */
class GrLightAddXPFactory : public GrXPFactory {
public:
    static const GrXPFactory* Get();

private:
    constexpr GrLightAddXPFactory() {}

    AnalysisProperties analysisProperties(const GrProcessorAnalysisColor&,
                                          const GrProcessorAnalysisCoverage&,
                                          const GrCaps&,
                                          GrClampType) const override;

    sk_sp<const GrXferProcessor> makeXferProcessor(const GrProcessorAnalysisColor&,
                                                   GrProcessorAnalysisCoverage,
                                                   const GrCaps&,
                                                   GrClampType) const override;

    using INHERITED = GrXPFactory;
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif
