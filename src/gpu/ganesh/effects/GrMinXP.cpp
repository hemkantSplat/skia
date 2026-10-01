/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ganesh/effects/GrMinXP.h"

#include "include/private/gpu/ganesh/GrTypesPriv.h"
#include "src/gpu/Blend.h"
#include "src/gpu/ganesh/GrCaps.h"
#include "src/gpu/ganesh/GrProcessorAnalysis.h"
#include "src/gpu/ganesh/glsl/GrGLSLFragmentShaderBuilder.h"

#include <memory>

namespace {

bool min_reads_dst(GrProcessorAnalysisCoverage coverage, const GrCaps& caps) {
    return !caps.minBlendEquationSupport() || GrProcessorAnalysisCoverage::kNone != coverage;
}

class MinXP : public GrXferProcessor {
public:
    MinXP(bool readsDst, GrProcessorAnalysisCoverage coverage)
            : INHERITED(kMinXP_ClassID, readsDst, coverage) {}

    const char* name() const override { return "Min"; }

    std::unique_ptr<ProgramImpl> makeProgramImpl() const override;

private:
    void onAddToKey(const GrShaderCaps&, skgpu::KeyBuilder*) const override {}

    void onGetBlendInfo(skgpu::BlendInfo* blendInfo) const override {
        // The min equation ignores coefficients; (ONE, ONE) is what WebGPU requires with it.
        blendInfo->fEquation = skgpu::BlendEquation::kMin;
        blendInfo->fSrcBlend = skgpu::BlendCoeff::kOne;
        blendInfo->fDstBlend = skgpu::BlendCoeff::kOne;
    }

    bool onIsEqual(const GrXferProcessor&) const override { return true; }

    using INHERITED = GrXferProcessor;
};

std::unique_ptr<GrXferProcessor::ProgramImpl> MinXP::makeProgramImpl() const {
    class Impl : public ProgramImpl {
    private:
        // Only uncovered draws reach the fixed-function path, so the colour passes through.
        void emitOutputsForBlendState(const EmitArgs& args) override {
            args.fXPFragBuilder->codeAppendf("%s = %s;", args.fOutputPrimary, args.fInputColor);
        }

        void emitBlendCodeForDstRead(GrGLSLXPFragmentBuilder* fragBuilder,
                                     GrGLSLUniformHandler*,
                                     const char* srcColor,
                                     const char* srcCoverage,
                                     const char* dstColor,
                                     const char* outColor,
                                     const char* outColorSecondary,
                                     const GrXferProcessor& proc) override {
            fragBuilder->codeAppendf("%s = min(%s, %s);", outColor, srcColor, dstColor);
            DefaultCoverageModulation(fragBuilder, srcCoverage, dstColor, outColor,
                                      outColorSecondary, proc);
        }
    };
    return std::make_unique<Impl>();
}

}  // namespace

const GrXPFactory* GrMinXPFactory::Get() {
    static constexpr const GrMinXPFactory gMinXPFactory;
    return &gMinXPFactory;
}

GrXPFactory::AnalysisProperties GrMinXPFactory::analysisProperties(
        const GrProcessorAnalysisColor&,
        const GrProcessorAnalysisCoverage& coverage,
        const GrCaps& caps,
        GrClampType) const {
    return min_reads_dst(coverage, caps) ? AnalysisProperties::kReadsDstInShader
                                         : AnalysisProperties::kNone;
}

GrDstReadCause GrMinXPFactory::dstReadCause(const GrProcessorAnalysisColor&,
                                            const GrProcessorAnalysisCoverage& coverage,
                                            const GrCaps& caps,
                                            GrClampType) const {
    return {caps.minBlendEquationSupport() ? GrDstReadReason::kCoverageBlend
                                           : GrDstReadReason::kAdvancedBlend,
            GrDstReadCause::kMinBlend};
}

sk_sp<const GrXferProcessor> GrMinXPFactory::makeXferProcessor(
        const GrProcessorAnalysisColor&,
        GrProcessorAnalysisCoverage coverage,
        const GrCaps& caps,
        GrClampType) const {
    return sk_make_sp<MinXP>(min_reads_dst(coverage, caps), coverage);
}
