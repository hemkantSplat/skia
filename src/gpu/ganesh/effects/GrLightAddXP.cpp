/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ganesh/effects/GrLightAddXP.h"

#include "include/private/gpu/ganesh/GrTypesPriv.h"
#include "src/gpu/Blend.h"
#include "src/gpu/ganesh/GrProcessorAnalysis.h"
#include "src/gpu/ganesh/glsl/GrGLSLFragmentShaderBuilder.h"

#include <memory>

namespace {

// Clamped float targets (GrClampType::kManual) cannot saturate in hardware, so they read dst.
class LightAddXP : public GrXferProcessor {
public:
    LightAddXP(bool readsDst, GrProcessorAnalysisCoverage coverage)
            : INHERITED(kLightAddXP_ClassID, readsDst, coverage) {}

    const char* name() const override { return "Light Add"; }

    std::unique_ptr<ProgramImpl> makeProgramImpl() const override;

private:
    void onAddToKey(const GrShaderCaps&, skgpu::KeyBuilder*) const override {}

    void onGetBlendInfo(skgpu::BlendInfo* blendInfo) const override {
        blendInfo->fEquation = skgpu::BlendEquation::kAdd;
        blendInfo->fSrcBlend = skgpu::BlendCoeff::kOne;
        blendInfo->fDstBlend = skgpu::BlendCoeff::kOne;
    }

    bool onIsEqual(const GrXferProcessor&) const override { return true; }

    using INHERITED = GrXferProcessor;
};

std::unique_ptr<GrXferProcessor::ProgramImpl> LightAddXP::makeProgramImpl() const {
    class Impl : public ProgramImpl {
    private:
        // Zero alpha leaves dst alpha to (ONE, ONE); per-channel coverage also serves LCD.
        void emitOutputsForBlendState(const EmitArgs& args) override {
            args.fXPFragBuilder->codeAppendf("%s = half4(%s.rgb * %s.rgb, 0);", args.fOutputPrimary,
                                             args.fInputColor, args.fInputCoverage);
        }

        void emitBlendCodeForDstRead(GrGLSLXPFragmentBuilder* fragBuilder,
                                     GrGLSLUniformHandler*,
                                     const char* srcColor,
                                     const char* srcCoverage,
                                     const char* dstColor,
                                     const char* outColor,
                                     const char* outColorSecondary,
                                     const GrXferProcessor& proc) override {
            fragBuilder->codeAppendf("%s = saturate(half4(%s.rgb + %s.rgb, %s.a));", outColor,
                                     dstColor, srcColor, dstColor);
            DefaultCoverageModulation(fragBuilder, srcCoverage, dstColor, outColor,
                                      outColorSecondary, proc);
        }
    };
    return std::make_unique<Impl>();
}

}  // namespace

const GrXPFactory* GrLightAddXPFactory::Get() {
    static constexpr const GrLightAddXPFactory gLightAddXPFactory;
    return &gLightAddXPFactory;
}

GrXPFactory::AnalysisProperties GrLightAddXPFactory::analysisProperties(
        const GrProcessorAnalysisColor&,
        const GrProcessorAnalysisCoverage&,
        const GrCaps&,
        GrClampType clampType) const {
    // Premultiplying coverage into the colour scales rgb alike and alpha is dropped anyway.
    return GrClampType::kManual == clampType ? AnalysisProperties::kReadsDstInShader
                                             : AnalysisProperties::kCompatibleWithCoverageAsAlpha;
}

sk_sp<const GrXferProcessor> GrLightAddXPFactory::makeXferProcessor(
        const GrProcessorAnalysisColor&,
        GrProcessorAnalysisCoverage coverage,
        const GrCaps&,
        GrClampType clampType) const {
    return sk_make_sp<LightAddXP>(GrClampType::kManual == clampType, coverage);
}
