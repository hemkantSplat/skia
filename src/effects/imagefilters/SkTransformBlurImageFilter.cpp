/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/effects/imagefilters/SkTransformBlurImageFilter.h"

#include "include/core/SkFlattenable.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkM44.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPoint.h"
#include "include/core/SkPoint3.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkSpan.h"
#include "include/core/SkString.h"
#include "include/effects/SkImageFilters.h"
#include "include/private/base/SkTArray.h"
#include "src/core/SkImageFilterTypes.h"
#include "src/core/SkImageFilter_Base.h"
#include "src/core/SkKnownRuntimeEffects.h"
#include "src/core/SkReadBuffer.h"
#include "src/core/SkRectPriv.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/core/SkWriteBuffer.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

using SkTransformBlurPriv::kMaxEntries;
using SkTransformBlurPriv::kMaxSamples;

namespace {

// Pass 1 measures the pixel's path; pass 2 cuts it into K slots equal in the arc/time blend and taps
// each once at its time-weighted centroid, weighted by its time. Behind-eye/still pixels keep p.
constexpr char kTransformBlurCode[] = R"(
const int kMaxEntries = %d;
const int kMaxSlots = %d;
const float kSpacing = %f;
const float kBlend = %f;
uniform shader src;
uniform float entries;
uniform float minSamples;
uniform float maxSamples;
uniform float3x3 path[kMaxEntries];
half4 main(float2 p) {
  float total = 0;
  bool behind = false;
  float2 prev = p;
  for (int i = 0; i < kMaxEntries; i++) {
    if (float(i) >= entries) break;
    float3 h = path[i] * float3(p, 1);
    if (h.z <= 0) { behind = true; break; }
    float2 q = h.xy / h.z;
    if (i > 0) total += distance(prev, q);
    prev = q;
  }
  if (behind || total == 0) return src.eval(p);
  float count = clamp(ceil(total / kSpacing), minSamples, maxSamples);
  float dt = 1 / (entries - 1);
  float3 h0 = path[0] * float3(p, 1);
  float2 a = h0.xy / h0.z;
  float4 sum = float4(0);
  float2 centroid = float2(0);
  float weight = 0, closed = 0, start = 0;
  for (int i = 1; i < kMaxEntries; i++) {
    if (float(i) >= entries) break;
    float3 h = path[i] * float3(p, 1);
    float2 b = h.xy / h.z;
    float len = distance(a, b);
    // The blended measure runs linearly from g0 to g1 over this segment.
    float g0 = kBlend * start / total + (1 - kBlend) * (float(i) - 1) * dt;
    float g1 = kBlend * (start + len) / total + (1 - kBlend) * float(i) * dt;
    float u0 = 0;
    for (int j = 0; j <= kMaxSlots; j++) {
      // The open slot ends inside this segment unless it is the last slot, which ends the path.
      float boundary = (closed + 1) / count;
      bool closes = closed + 1 < count && boundary <= g1;
      float u1 = closes && g1 > g0 ? (boundary - g0) / (g1 - g0) : 1;
      float w = (u1 - u0) * dt;
      centroid += mix(a, b, 0.5 * (u0 + u1)) * w;
      weight += w;
      if (!closes) break;
      if (weight > 0) sum += float4(src.eval(centroid / weight)) * weight;
      centroid = float2(0);
      weight = 0;
      closed += 1;
      u0 = u1;
    }
    a = b;
    start += len;
  }
  if (weight > 0) sum += float4(src.eval(centroid / weight)) * weight;
  return half4(sum);
}
)";

SkMatrix lerp(const SkMatrix& a, const SkMatrix& b, float t) {
    SkMatrix m;
    for (int i = 0; i < 9; ++i) {
        m[i] = a[i] + (b[i] - a[i]) * t;
    }
    return m;
}

// The four corners of 'r' through 'm'; none when one lands behind the eye (w <= 0).
std::optional<std::array<SkPoint, 4>> map_corners(const SkMatrix& m, const SkRect& r) {
    std::array<SkPoint3, 4> h;
    const std::array<SkPoint3, 4> corners = {{{r.fLeft, r.fTop, 1}, {r.fRight, r.fTop, 1},
                                              {r.fRight, r.fBottom, 1}, {r.fLeft, r.fBottom, 1}}};
    m.mapHomogeneousPoints(h.data(), corners.data(), 4);
    std::array<SkPoint, 4> points;
    for (int i = 0; i < 4; ++i) {
        if (!(h[i].fZ > 0)) {
            return {};
        }
        points[i] = {h[i].fX / h[i].fZ, h[i].fY / h[i].fZ};
    }
    return points;
}

// The shutter in one layer space: entry i maps output pixels to the input positions they sample.
class ShutterPath {
public:
    // None when an entry's layer-space plane is singular.
    static std::optional<ShutterPath> Make(SkSpan<const SkM44> entries, const SkM44& layer) {
        ShutterPath path;
        path.fCount = static_cast<int>(entries.size());
        const SkMatrix current = (layer * entries.back()).asM33();
        if (!current.invert(nullptr)) {
            return {};
        }
        for (int i = 0; i + 1 < path.fCount; ++i) {
            SkMatrix inverse;
            if (!(layer * entries[i]).asM33().invert(&inverse)) {
                return {};
            }
            path.fToInput[i] = SkMatrix::Concat(current, inverse);
        }
        path.fToInput[path.fCount - 1] = SkMatrix::I();
        return path;
    }

    // Input a region samples: each pixel's path is straight between entries, so the entries bound it.
    SkRect requiredInput(const SkRect& output) const {
        SkRect bounds = SkRect::MakeEmpty();
        for (int i = 0; i < fCount; ++i) {
            bounds.join(SkRect(skif::LayerSpace<SkMatrix>(fToInput[i]).mapRect(
                    skif::LayerSpace<SkRect>(output))));
        }
        return bounds.makeOutset(1, 1);  // bilinear taps
    }

    // Output pixels whose path crosses 'input', swept per segment; none when unbounded.
    std::optional<SkRect> output(const SkRect& input) const {
        skia_private::TArray<SkPoint> swept;
        swept.reserve_exact(4 * (kSweepSteps + 1) * (fCount - 1));
        float step = 0;
        for (int i = 0; i + 1 < fCount; ++i) {
            std::optional<std::array<SkPoint, 4>> previous;
            for (int s = 0; s <= kSweepSteps; ++s) {
                SkMatrix fromInput;
                const float t = static_cast<float>(s) / kSweepSteps;
                if (!lerp(fToInput[i], fToInput[i + 1], t).invert(&fromInput)) {
                    return {};
                }
                const std::optional<std::array<SkPoint, 4>> corners = map_corners(fromInput, input);
                if (!corners) {
                    return {};
                }
                for (int c = 0; c < 4; ++c) {
                    swept.push_back((*corners)[c]);
                    if (previous) {
                        step = std::max(step, SkPoint::Distance((*corners)[c], (*previous)[c]));
                    }
                }
                previous = corners;
            }
        }
        // The largest move between sweep steps bounds how far the path strays between them.
        SkRect bounds;
        bounds.setBounds(swept.data(), swept.size());
        return bounds.makeOutset(step + 1, step + 1);
    }

    skif::LayerSpace<SkIRect> requiredInput(const skif::LayerSpace<SkIRect>& output) const {
        return skif::LayerSpace<SkIRect>(
                this->requiredInput(SkRect::Make(SkIRect(output))).roundOut());
    }

    std::optional<skif::LayerSpace<SkIRect>> output(const skif::LayerSpace<SkIRect>& input) const {
        const std::optional<SkRect> bounds = this->output(SkRect::Make(SkIRect(input)));
        if (!bounds) {
            return skif::LayerSpace<SkIRect>::Unbounded();
        }
        return skif::LayerSpace<SkIRect>(bounds->roundOut());
    }

    sk_sp<SkShader> makeShader(sk_sp<SkShader> input, int minSamples, int maxSamples) const {
        if (!input) {
            return nullptr;
        }
        SkRuntimeEffectBuilder builder(sk_ref_sp(GetKnownRuntimeEffect(
                SkKnownRuntimeEffects::StableKey::kTransformBlur)));

        // Column-major float3x3s; entries past fCount are never read.
        std::array<float, 9 * kMaxEntries> path{};
        for (int i = 0; i < fCount; ++i) {
            for (int k = 0; k < 9; ++k) {
                path[9 * i + k] = fToInput[i].rc(k % 3, k / 3);
            }
        }
        builder.child("src") = std::move(input);
        builder.uniform("entries") = static_cast<float>(fCount);
        builder.uniform("minSamples") = static_cast<float>(minSamples);
        builder.uniform("maxSamples") = static_cast<float>(maxSamples);
        builder.uniform("path").set(path.data(), path.size());
        return builder.makeShader();
    }

private:
    static constexpr int kSweepSteps = 16;

    ShutterPath() = default;

    int fCount = 0;
    std::array<SkMatrix, kMaxEntries> fToInput;
};

class SkTransformBlurImageFilter final : public SkImageFilter_Base {
public:
    SkTransformBlurImageFilter(SkSpan<const SkM44> path, int minSamples, int maxSamples,
                               sk_sp<SkImageFilter> input)
            : SkImageFilter_Base(&input, 1)
            , fPath(path.data(), path.size())
            , fMinSamples(minSamples)
            , fMaxSamples(maxSamples) {}

    SkRect computeFastBounds(const SkRect& src) const override;

protected:
    void flatten(SkWriteBuffer&) const override;

private:
    friend void ::SkRegisterTransformBlurImageFilterFlattenable();
    SK_FLATTENABLE_HOOKS(SkTransformBlurImageFilter)

    MatrixCapability onGetCTMCapability() const override { return MatrixCapability::kComplex; }

    skif::FilterResult onFilterImage(const skif::Context&) const override;

    skif::LayerSpace<SkIRect> onGetInputLayerBounds(
            const skif::Mapping& mapping,
            const skif::LayerSpace<SkIRect>& desiredOutput,
            std::optional<skif::LayerSpace<SkIRect>> contentBounds) const override;

    std::optional<skif::LayerSpace<SkIRect>> onGetOutputLayerBounds(
            const skif::Mapping& mapping,
            std::optional<skif::LayerSpace<SkIRect>> contentBounds) const override;

    std::optional<ShutterPath> path(const SkM44& layerMatrix) const {
        return ShutterPath::Make(fPath, layerMatrix);
    }

    skia_private::STArray<kMaxEntries, SkM44> fPath;
    int fMinSamples;
    int fMaxSamples;
};

}  // namespace

SkRuntimeEffect* SkTransformBlurPriv::MakeEffect(const SkRuntimeEffect::Options& options) {
    SkString code;
    code.appendf(kTransformBlurCode, kMaxEntries, kMaxSamples, kSampleSpacing,
                 SkTransformBlurPriv::kArcTimeBlend);
    return SkMakeRuntimeEffect(SkRuntimeEffect::MakeForShader, code.c_str(), options);
}

sk_sp<SkImageFilter> SkImageFilters::TransformBlur(SkSpan<const SkM44> path, int minSamples,
                                                   int maxSamples, sk_sp<SkImageFilter> input,
                                                   const CropRect& cropRect) {
    if (path.size() < 2 || path.size() > static_cast<size_t>(kMaxEntries) || minSamples < 1 ||
        maxSamples < minSamples) {
        return nullptr;
    }
    for (const SkM44& m : path) {
        if (!m.isFinite()) {
            return nullptr;
        }
    }
    maxSamples = std::min(maxSamples, kMaxSamples);
    minSamples = std::min(minSamples, maxSamples);

    const bool moves = std::any_of(path.begin(), path.end(),
                                   [&](const SkM44& m) { return m != path.back(); });
    const bool regular = std::all_of(path.begin(), path.end(),
                                     [](const SkM44& m) { return m.asM33().invert(nullptr); });
    sk_sp<SkImageFilter> filter = std::move(input);
    if (moves && regular) {
        filter = sk_make_sp<SkTransformBlurImageFilter>(path, minSamples, maxSamples,
                                                        std::move(filter));
    }
    if (cropRect) {
        filter = SkImageFilters::Crop(*cropRect, std::move(filter));
    }
    return filter;
}

void SkRegisterTransformBlurImageFilterFlattenable() {
    SK_REGISTER_FLATTENABLE(SkTransformBlurImageFilter);
}

sk_sp<SkFlattenable> SkTransformBlurImageFilter::CreateProc(SkReadBuffer& buffer) {
    SK_IMAGEFILTER_UNFLATTEN_COMMON(common, 1);
    const int count = buffer.readInt();
    if (!buffer.validate(count >= 2 && count <= kMaxEntries)) {
        return nullptr;
    }
    std::array<SkM44, kMaxEntries> path;
    for (int i = 0; i < count; ++i) {
        buffer.read(&path[i]);
    }
    const int minSamples = buffer.readInt();
    const int maxSamples = buffer.readInt();
    return SkImageFilters::TransformBlur({path.data(), static_cast<size_t>(count)}, minSamples,
                                         maxSamples, common.getInput(0), common.cropRect());
}

void SkTransformBlurImageFilter::flatten(SkWriteBuffer& buffer) const {
    this->SkImageFilter_Base::flatten(buffer);
    buffer.writeInt(fPath.size());
    for (const SkM44& m : fPath) {
        buffer.write(m);
    }
    buffer.writeInt(fMinSamples);
    buffer.writeInt(fMaxSamples);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

skif::FilterResult SkTransformBlurImageFilter::onFilterImage(const skif::Context& ctx) const {
    using ShaderFlags = skif::FilterResult::ShaderFlags;

    const std::optional<ShutterPath> path = this->path(ctx.mapping().layerMatrix());
    if (!path) {
        return this->getChildOutput(0, ctx);
    }
    const skif::LayerSpace<SkIRect> required = path->requiredInput(ctx.desiredOutput());
    const skif::FilterResult input = this->getChildOutput(0, ctx.withNewDesiredOutput(required));
    if (!input) {
        return {};
    }
    skif::LayerSpace<SkIRect> output =
            path->output(input.layerBounds()).value_or(ctx.desiredOutput());
    if (!output.intersect(ctx.desiredOutput())) {
        return {};
    }

    skif::FilterResult::Builder builder{ctx};
    builder.add(input, required, ShaderFlags::kSampledRepeatedly | ShaderFlags::kNonTrivialSampling,
                SkFilterMode::kLinear);
    return builder.eval(
            [&](SkSpan<sk_sp<SkShader>> inputs) {
                return path->makeShader(inputs[0], fMinSamples, fMaxSamples);
            },
            output);
}

skif::LayerSpace<SkIRect> SkTransformBlurImageFilter::onGetInputLayerBounds(
        const skif::Mapping& mapping,
        const skif::LayerSpace<SkIRect>& desiredOutput,
        std::optional<skif::LayerSpace<SkIRect>> contentBounds) const {
    const std::optional<ShutterPath> path = this->path(mapping.layerMatrix());
    const skif::LayerSpace<SkIRect> required =
            path ? path->requiredInput(desiredOutput) : desiredOutput;
    return this->getChildInputLayerBounds(0, mapping, required, contentBounds);
}

std::optional<skif::LayerSpace<SkIRect>> SkTransformBlurImageFilter::onGetOutputLayerBounds(
        const skif::Mapping& mapping,
        std::optional<skif::LayerSpace<SkIRect>> contentBounds) const {
    const std::optional<skif::LayerSpace<SkIRect>> childOutput =
            this->getChildOutputLayerBounds(0, mapping, contentBounds);
    const std::optional<ShutterPath> path = this->path(mapping.layerMatrix());
    if (!childOutput || !path) {
        return childOutput;
    }
    return path->output(*childOutput);
}

SkRect SkTransformBlurImageFilter::computeFastBounds(const SkRect& src) const {
    const SkRect bounds = this->getInput(0) ? this->getInput(0)->computeFastBounds(src) : src;
    const std::optional<ShutterPath> path = this->path(SkM44());
    if (!path) {
        return bounds;
    }
    return path->output(bounds).value_or(SkRectPriv::MakeLargeS32());
}
