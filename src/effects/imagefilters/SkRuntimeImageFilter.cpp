/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/effects/SkImageFilters.h"

#include "include/core/SkData.h"
#include "include/core/SkFlattenable.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkSize.h"
#include "include/core/SkSpan.h"
#include "include/core/SkString.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/private/base/SkAssert.h"
#include "include/private/base/SkTArray.h"
#include "src/base/SkSpinlock.h"
#include "src/core/SkImageFilterTypes.h"
#include "src/core/SkImageFilter_Base.h"
#include "src/core/SkPicturePriv.h"
#include "src/core/SkReadBuffer.h"
#include "src/core/SkRectPriv.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/core/SkSpecialImage.h"
#include "src/core/SkWriteBuffer.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace skia_private;
using RasterPolicy = SkImageFilters::RuntimeShaderRasterPolicy;

// NOTE: Not in an anonymous namespace so that SkRuntimeShaderBuilder can friend it.
class SkRuntimeImageFilter final : public SkImageFilter_Base {
public:
    SkRuntimeImageFilter(const SkRuntimeShaderBuilder& builder,
                         float maxSampleRadius,
                         std::string_view childShaderNames[],
                         const sk_sp<SkImageFilter> inputs[],
                         int inputCount, RasterPolicy policy)
            : SkImageFilter_Base(inputs, inputCount)
            , fRuntimeEffectBuilder(builder)
            , fMaxSampleRadius(maxSampleRadius)
            , fRasterPolicy(policy) {
        SkASSERT(maxSampleRadius >= 0.f);
        fChildShaderNames.reserve_exact(inputCount);
        for (int i = 0; i < inputCount; i++) {
            fChildShaderNames.push_back(SkString(childShaderNames[i]));
        }
    }

    SkRect computeFastBounds(const SkRect& src) const override;
    MatrixCapability getCTMCapability() const override {
        return fRasterPolicy == RasterPolicy::kDevice ? MatrixCapability::kComplex
                                                     : SkImageFilter_Base::getCTMCapability();
    }

protected:
    void flatten(SkWriteBuffer&) const override;

private:
    bool onSupportsRasterF16() const override { return true; }

    friend void ::SkRegisterRuntimeImageFilterFlattenable();
    static sk_sp<SkFlattenable> CreateProc(SkReadBuffer& buffer) {
        return Read(buffer, RasterPolicy::kParameter);
    }
    static sk_sp<SkFlattenable> CreateDeviceProc(SkReadBuffer& buffer) {
        return Read(buffer, RasterPolicy::kDevice);
    }
    static sk_sp<SkFlattenable> Read(SkReadBuffer&, RasterPolicy);
    Factory getFactory() const override {
#if defined(SK_DISABLE_EFFECT_DESERIALIZATION)
        return nullptr;
#else
        return fRasterPolicy == RasterPolicy::kDevice ? CreateDeviceProc : CreateProc;
#endif
    }
    const char* getTypeName() const override {
        return fRasterPolicy == RasterPolicy::kDevice
                ? "SkRuntimeImageFilter_DeviceResolution" : "SkRuntimeImageFilter";
    }

    bool onAffectsTransparentBlack() const override { return true; }
    // The opt-in policy keeps the CTM in layer space; shader evaluation remains in parameter space.
    MatrixCapability onGetCTMCapability() const override {
        return fRasterPolicy == RasterPolicy::kDevice ? MatrixCapability::kComplex
                                                     : MatrixCapability::kTranslate;
    }

    skif::FilterResult onFilterImage(const skif::Context&) const override;

    skif::LayerSpace<SkIRect> onGetInputLayerBounds(
            const skif::Mapping& mapping,
            const skif::LayerSpace<SkIRect>& desiredOutput,
            std::optional<skif::LayerSpace<SkIRect>> contentBounds) const override;

    std::optional<skif::LayerSpace<SkIRect>> onGetOutputLayerBounds(
            const skif::Mapping& mapping,
            std::optional<skif::LayerSpace<SkIRect>> contentBounds) const override;

    skif::LayerSpace<SkIRect> applyMaxSampleRadius(
            const skif::Mapping& mapping,
            skif::LayerSpace<SkIRect> bounds) const {
        skif::LayerSpace<SkISize> maxSampleRadius = mapping.paramToLayer(
                skif::ParameterSpace<SkSize>({fMaxSampleRadius, fMaxSampleRadius})).ceil();
        if (fRasterPolicy == RasterPolicy::kDevice) {
            const SkMatrix matrix = mapping.layerMatrix().asM33();
            // A parameter-space sampling square maps to the absolute row sums of the affine CTM.
            maxSampleRadius = skif::LayerSpace<SkSize>({
                    fMaxSampleRadius * (SkScalarAbs(matrix.getScaleX()) + SkScalarAbs(matrix.getSkewX())),
                    fMaxSampleRadius * (SkScalarAbs(matrix.getSkewY()) + SkScalarAbs(matrix.getScaleY()))}).ceil();
        }
        bounds.outset(maxSampleRadius);
        return bounds;
    }

    struct ChildSpace {
        skif::Mapping mapping;
        skif::LayerSpace<SkMatrix> toChild;
        skif::LayerSpace<SkMatrix> fromChild;
        bool changed() const { return !static_cast<const SkMatrix&>(toChild).isIdentity(); }
    };
    std::optional<ChildSpace> childSpace(int index, const skif::Mapping& mapping) const {
        ChildSpace space{mapping, skif::LayerSpace<SkMatrix>(SkMatrix::I()),
                                 skif::LayerSpace<SkMatrix>(SkMatrix::I())};
        if (fRasterPolicy != RasterPolicy::kDevice || !this->getInput(index)) {
            return space;
        }
        skif::Mapping decomposition;
        if (!decomposition.decomposeCTM(mapping.layerMatrix(),
                as_IFB(this->getInput(index))->getCTMCapability(),
                skif::ParameterSpace<SkPoint>({0.f, 0.f}))) {
            return std::nullopt;
        }
        space.toChild = skif::LayerSpace<SkMatrix>(decomposition.deviceToLayer().asM33());
        space.fromChild = skif::LayerSpace<SkMatrix>(decomposition.layerToDevice().asM33());
        if (!space.mapping.adjustLayerSpace(decomposition.deviceToLayer())) {
            return std::nullopt;
        }
        return space;
    }
    skif::FilterResult childOutput(int index, const skif::Context& ctx) const;

    mutable SkSpinlock fRuntimeEffectLock;
    mutable SkRuntimeShaderBuilder fRuntimeEffectBuilder;
    STArray<1, SkString> fChildShaderNames;
    float fMaxSampleRadius;
    RasterPolicy fRasterPolicy;
};

sk_sp<SkImageFilter> SkImageFilters::RuntimeShader(const SkRuntimeShaderBuilder& builder,
                                                   SkScalar sampleRadius,
                                                   std::string_view childShaderName,
                                                   sk_sp<SkImageFilter> input) {
    // If no childShaderName is provided, check to see if we can implicitly assign it to the only
    // child in the effect.
    if (childShaderName.empty()) {
        auto children = builder.effect()->children();
        if (children.size() != 1) {
            return nullptr;
        }
        childShaderName = children.front().name;
    }

    return SkImageFilters::RuntimeShader(builder, sampleRadius, &childShaderName, &input, 1);
}

sk_sp<SkImageFilter> SkImageFilters::RuntimeShader(const SkRuntimeShaderBuilder& builder,
                                                   SkScalar maxSampleRadius,
                                                   std::string_view childShaderNames[],
                                                   const sk_sp<SkImageFilter> inputs[],
                                                   int inputCount) {
    return RuntimeShader(builder, maxSampleRadius, childShaderNames, inputs, inputCount,
                         RasterPolicy::kParameter);
}

sk_sp<SkImageFilter> SkImageFilters::RuntimeShader(const SkRuntimeShaderBuilder& builder,
                                                   SkScalar maxSampleRadius,
                                                   std::string_view childShaderNames[],
                                                   const sk_sp<SkImageFilter> inputs[],
                                                   int inputCount, RasterPolicy policy) {
    if (maxSampleRadius < 0.f ||
        (policy == RasterPolicy::kDevice && !std::isfinite(maxSampleRadius))) {
        return nullptr; // invalid sample radius
    }

    auto child_is_shader = [](const SkRuntimeEffect::Child* child) {
        return child && child->type == SkRuntimeEffect::ChildType::kShader;
    };

    for (int i = 0; i < inputCount; i++) {
        std::string_view name = childShaderNames[i];
        // All names must be non-empty, and present as a child shader in the effect:
        if (name.empty() || !child_is_shader(builder.effect()->findChild(name))) {
            return nullptr;
        }

        // We don't allow duplicates, either:
        for (int j = 0; j < i; j++) {
            if (name == childShaderNames[j]) {
                return nullptr;
            }
        }
    }

    return sk_sp<SkImageFilter>(new SkRuntimeImageFilter(builder, maxSampleRadius, childShaderNames,
                                                         inputs, inputCount, policy));
}

void SkRegisterRuntimeImageFilterFlattenable() {
    SK_REGISTER_FLATTENABLE(SkRuntimeImageFilter);
#if !defined(SK_DISABLE_EFFECT_DESERIALIZATION)
    SkFlattenable::Register("SkRuntimeImageFilter_DeviceResolution",
                            SkRuntimeImageFilter::CreateDeviceProc);
#endif
}

sk_sp<SkFlattenable> SkRuntimeImageFilter::Read(SkReadBuffer& buffer, RasterPolicy policy) {
    // We don't know how many inputs to expect yet. Passing -1 allows any number of children.
    SK_IMAGEFILTER_UNFLATTEN_COMMON(common, -1);
    if (common.cropRect()) {
        return nullptr;
    }

    // Read the SkSL string and convert it into a runtime effect
    SkString sksl;
    buffer.readString(&sksl);
    auto effect = SkMakeCachedRuntimeEffect(SkRuntimeEffect::MakeForShader, std::move(sksl));
    if (!buffer.validate(effect != nullptr)) {
        return nullptr;
    }

    // Read the uniform data and make sure it matches the size from the runtime effect
    sk_sp<SkData> uniforms = buffer.readByteArrayAsData();
    if (!buffer.validate(uniforms->size() == effect->uniformSize())) {
        return nullptr;
    }

    // Read the child shader names
    STArray<4, std::string_view> childShaderNames;
    STArray<4, SkString> childShaderNameStrings;
    childShaderNames.resize(common.inputCount());
    childShaderNameStrings.resize(common.inputCount());
    for (int i = 0; i < common.inputCount(); i++) {
        buffer.readString(&childShaderNameStrings[i]);
        childShaderNames[i] = childShaderNameStrings[i].c_str();
    }

    SkRuntimeShaderBuilder builder(std::move(effect), std::move(uniforms));

    // Populate the builder with the corresponding children
    for (const SkRuntimeEffect::Child& child : builder.effect()->children()) {
        std::string_view name = child.name;
        switch (child.type) {
            case SkRuntimeEffect::ChildType::kBlender: {
                builder.child(name) = buffer.readBlender();
                break;
            }
            case SkRuntimeEffect::ChildType::kColorFilter: {
                builder.child(name) = buffer.readColorFilter();
                break;
            }
            case SkRuntimeEffect::ChildType::kShader: {
                builder.child(name) = buffer.readShader();
                break;
            }
        }
    }

    float maxSampleRadius = 0.f; // default before sampleRadius was exposed in the factory
    if (!buffer.isVersionLT(SkPicturePriv::kRuntimeImageFilterSampleRadius)) {
        maxSampleRadius = buffer.readScalar();
    }

    if (!buffer.isValid()) {
        return nullptr;
    }

    return SkImageFilters::RuntimeShader(builder, maxSampleRadius, childShaderNames.data(),
                                         common.inputs(), common.inputCount(), policy);
}

void SkRuntimeImageFilter::flatten(SkWriteBuffer& buffer) const {
    this->SkImageFilter_Base::flatten(buffer);
    fRuntimeEffectLock.acquire();
    buffer.writeString(fRuntimeEffectBuilder.effect()->source().c_str());
    buffer.writeDataAsByteArray(fRuntimeEffectBuilder.uniforms().get());
    for (const SkString& name : fChildShaderNames) {
        buffer.writeString(name.c_str());
    }
    for (size_t x = 0; x < fRuntimeEffectBuilder.children().size(); x++) {
        buffer.writeFlattenable(fRuntimeEffectBuilder.children()[x].flattenable());
    }
    fRuntimeEffectLock.release();

    buffer.writeScalar(fMaxSampleRadius);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

skif::FilterResult SkRuntimeImageFilter::childOutput(int index, const skif::Context& ctx) const {
    auto space = this->childSpace(index, ctx.mapping());
    if (!space) {
        return {};
    }
    if (!space->changed()) {
        return this->getChildOutput(index, ctx);
    }
    auto desired = space->toChild.mapRect(ctx.desiredOutput());
    desired.outset(skif::LayerSpace<SkISize>({1, 1}));
    auto childCtx = ctx.withNewMapping(space->mapping).withNewDesiredOutput(desired);
    auto required = this->getChildInputLayerBounds(index, space->mapping, desired, std::nullopt);
    auto source = ctx.source().applyTransform(childCtx.withNewDesiredOutput(required),
                                              space->toChild, SkFilterMode::kLinear);
    // Resolve the child's source in its raster space, retaining native F16 storage and cache identity.
    auto [image, origin] = source.imageAndOffset(childCtx.withNewDesiredOutput(required));
    auto result = this->getChildOutput(index,
            childCtx.withNewSource(skif::FilterResult(std::move(image), origin)));
    return result.applyTransform(ctx, space->fromChild, SkFilterMode::kLinear);
}

skif::FilterResult SkRuntimeImageFilter::onFilterImage(const skif::Context& ctx) const {
    using ShaderFlags = skif::FilterResult::ShaderFlags;
    if (fRasterPolicy == RasterPolicy::kDevice) {
        const SkMatrix matrix = ctx.mapping().layerMatrix().asM33();
        if (!matrix.isFinite() || matrix.hasPerspective() || !matrix.invert(nullptr)) {
            return {};
        }
    }

    const int inputCount = this->countInputs();
    SkASSERT(inputCount == fChildShaderNames.size());

    skif::Context inputCtx = ctx.withNewDesiredOutput(
            this->applyMaxSampleRadius(ctx.mapping(), ctx.desiredOutput()));
    skif::FilterResult::Builder builder{ctx};
    for (int i = 0; i < inputCount; ++i) {
        // Record the input context's desired output as the sample bounds for the child shaders
        // since the runtime shader can go up to max sample radius away from its desired output
        // (which is the default sample bounds if we didn't override it here).
        builder.add(this->childOutput(i, inputCtx),
                    inputCtx.desiredOutput(),
                    ShaderFlags::kNonTrivialSampling);
    }
    return builder.eval([&](SkSpan<sk_sp<SkShader>> inputs) {
        // lock the mutation of the builder and creation of the shader so that the builder's state
        // is const and is safe for multi-threaded access.
        fRuntimeEffectLock.acquire();
        for (int i = 0; i < inputCount; i++) {
            fRuntimeEffectBuilder.child(fChildShaderNames[i].c_str()) = inputs[i];
        }
        sk_sp<SkShader> shader = fRuntimeEffectBuilder.makeShader();

        // Remove the inputs from the builder to avoid unnecessarily prolonging the input shaders'
        // lifetimes.
        for (int i = 0; i < inputCount; i++) {
            fRuntimeEffectBuilder.child(fChildShaderNames[i].c_str()) = nullptr;
        }
        fRuntimeEffectLock.release();

        return shader;
    }, {}, /*evaluateInParameterSpace=*/true);
}

skif::LayerSpace<SkIRect> SkRuntimeImageFilter::onGetInputLayerBounds(
        const skif::Mapping& mapping,
        const skif::LayerSpace<SkIRect>& desiredOutput,
        std::optional<skif::LayerSpace<SkIRect>> contentBounds) const {
    const SkMatrix matrix = mapping.layerMatrix().asM33();
    if (fRasterPolicy == RasterPolicy::kDevice &&
        (!matrix.isFinite() || matrix.hasPerspective() || !matrix.invert(nullptr))) {
        return skif::LayerSpace<SkIRect>::Empty();
    }
    const int inputCount = this->countInputs();
    if (inputCount <= 0) {
        return skif::LayerSpace<SkIRect>::Empty();
    } else {
        // Provide 'maxSampleRadius' pixels (in layer space) to the child shaders.
        skif::LayerSpace<SkIRect> requiredInput =
                this->applyMaxSampleRadius(mapping, desiredOutput);

        // Union of all child input bounds so that one source image can provide for all of them.
        return skif::LayerSpace<SkIRect>::Union(
                inputCount,
                [&](int i) {
                    auto space = this->childSpace(i, mapping);
                    if (!space) {
                        return skif::LayerSpace<SkIRect>::Empty();
                    }
                    if (!space->changed()) {
                        return this->getChildInputLayerBounds(i, mapping, requiredInput, contentBounds);
                    }
                    auto desired = space->toChild.mapRect(requiredInput);
                    desired.outset(skif::LayerSpace<SkISize>({1, 1}));
                    auto content = contentBounds ? std::optional(space->toChild.mapRect(*contentBounds))
                                                 : std::nullopt;
                    auto required = this->getChildInputLayerBounds(i, space->mapping, desired, content);
                    required = space->fromChild.mapRect(required);
                    required.outset(skif::LayerSpace<SkISize>({1, 1}));
                    return required;
                });
    }
}

std::optional<skif::LayerSpace<SkIRect>> SkRuntimeImageFilter::onGetOutputLayerBounds(
        const skif::Mapping& /*mapping*/,
        std::optional<skif::LayerSpace<SkIRect>> /*contentBounds*/) const {
    // Pessimistically assume it can cover anything
    return skif::LayerSpace<SkIRect>::Unbounded();
}

SkRect SkRuntimeImageFilter::computeFastBounds(const SkRect& src) const {
    // Can't predict what the RT Shader will generate (see onGetOutputLayerBounds)
    return SkRectPriv::MakeLargeS32();
}
