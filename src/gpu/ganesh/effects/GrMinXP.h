/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrMinXP_DEFINED
#define GrMinXP_DEFINED

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
 * SkBlenders::Min: min(src, dst) per premul channel on the fixed-function min equation. That
 * equation cannot weigh by coverage, so covered draws (or targets without it) read dst instead.
 */
class GrMinXPFactory : public GrXPFactory {
public:
    static const GrXPFactory* Get();

private:
    constexpr GrMinXPFactory() {}

    AnalysisProperties analysisProperties(const GrProcessorAnalysisColor&,
                                          const GrProcessorAnalysisCoverage&,
                                          const GrCaps&,
                                          GrClampType) const override;

    GrDstReadCause dstReadCause(const GrProcessorAnalysisColor&,
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
