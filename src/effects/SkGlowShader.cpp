/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/effects/SkGlowShader.h"

#include "include/core/SkImage.h"
#include "include/core/SkM44.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkTileMode.h"
#include "include/private/base/SkFloatingPoint.h"
#include "src/core/SkKnownRuntimeEffects.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/effects/SkGlowShaderPriv.h"

#include <array>
#include <cstdint>
#include <iterator>

using SkKnownRuntimeEffects::StableKey;

constexpr int kErfKeyCount =
        static_cast<int>(StableKey::kGlowErf32) - static_cast<int>(StableKey::kGlowErfBase) + 1;
static_assert(std::size(SkGlowShaderPriv::kErfBins) == kErfKeyCount);
static_assert(SkGlowShaderPriv::kErfBins[std::size(SkGlowShaderPriv::kErfBins) - 1] ==
              SkGlowShader::kMaxLobes);

namespace {

// The rounded-box core both forms add; half a device pixel of ramp at its edge.
constexpr char kCoreCode[] = R"(
uniform float4 coreColor;
uniform float4 coreBox;
uniform float4 coreRadii;
uniform float pxScale;
uniform float coreSigma;
uniform float coreMask;
float coreCoverage(float2 p) {
  float2 h = coreBox.zw * 0.5, q = p - coreBox.xy - h;
  float r = q.x < 0 ? (q.y < 0 ? coreRadii.x : coreRadii.w) : (q.y < 0 ? coreRadii.y : coreRadii.z);
  float2 d = abs(q) - h + r;
  float sd = min(max(d.x, d.y), 0) + length(max(d, float2(0))) - r;
  return clamp(0.5 - sd * pxScale, 0, 1);
}
)";

// Abramowitz-Stegun 7.1.26 erf; each lobe is the product of two erf intervals.
constexpr char kErfCode[] = R"(
uniform float count;
uniform float2 lobe[%d];
uniform float4 box[%d];
uniform float4 colour[%d];
float erf(float x) {
  float a = abs(x), t = 1 / (1 + 0.3275911 * a);
  float y = 1 - ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
  return x < 0 ? -y : y;
}
half4 main(float2 p) {
  // A blurred core is its box's Gaussian mass m; mu is that Gaussian's mean truncated to the box (the nearest edge once m vanishes).
  float cover = 1;
  float2 mu = p;
  if (coreSigma > 0) {
    float2 lo = coreBox.xy, hi = coreBox.xy + coreBox.zw;
    float2 a = (lo - p) / (1.41421356 * coreSigma), b = (hi - p) / (1.41421356 * coreSigma);
    float2 m = 0.5 * float2(erf(b.x) - erf(a.x), erf(b.y) - erf(a.y));
    float2 shift = 0.39894228 * coreSigma * (exp(-a * a) - exp(-b * b)) / max(m, float2(1e-4));
    mu = mix(clamp(p, lo, hi), p + shift, step(float2(1e-4), m));
    cover = m.x * m.y;
  } else if (coreMask != 0 || coreColor.a > 0) {
    cover = coreCoverage(p);
  }
  float4 c = float4(0);
  for (int i = 0; i < %d; i++) {
    if (float(i) >= count) break;
    float k = lobe[i].x;
    float4 b = box[i];
    float ix = erf((b.z - p.x) * k) - erf((b.x - p.x) * k);
    float iy = erf((b.w - p.y) * k) - erf((b.y - p.y) * k);
    float v = 0.25 * lobe[i].y * ix * iy;
    if (coreMask != 0 && coreSigma > 0) {
      // The lobe's part on the box: its own sigma (sigma^2 - coreSigma^2) at mu, times the box's mass.
      float k0 = inversesqrt(max(1 / (k * k) - 2 * coreSigma * coreSigma, 1e-8));
      float jx = erf((b.z - mu.x) * k0) - erf((b.x - mu.x) * k0);
      float jy = erf((b.w - mu.y) * k0) - erf((b.y - mu.y) * k0);
      v -= 0.25 * lobe[i].y * cover * jx * jy;
    }
    c += colour[i] * v;
  }
  if (coreMask != 0 && coreSigma == 0) c *= 1 - cover;
  if (coreColor.a > 0) c += coreColor * cover;
  return half4(c);
}
)";

// Linear interpolation between nearest-sampled profile texels, rows wrapped at lutWidth.
constexpr char kRadialCode[] = R"(
uniform shader lut;
uniform float4 tint;
uniform float invStep;
uniform float last;
uniform float lutWidth;
float4 texel(float i) {
  float row = floor((i + 0.5) / lutWidth);
  return float4(lut.eval(float2(i - row * lutWidth + 0.5, row + 0.5)));
}
half4 main(float2 p) {
  float t = length(p) * invStep;
  float4 c = float4(0);
  if (t < last) { float i = floor(t); c = mix(texel(i), texel(i + 1), t - i) * tint; }
  if (coreMask != 0) c *= 1 - coreCoverage(p);
  if (coreColor.a > 0) c += coreColor * coreCoverage(p);
  return half4(c);
}
)";

SkV4 v4(const SkGlowShader::PMColor& c) { return {c.fR, c.fG, c.fB, c.fA}; }

bool finite(const SkGlowShader::PMColor& c) { return SkIsFinite(c.fR, c.fG, c.fB, c.fA); }

bool valid(const SkGlowShader::Core& core) {
    return finite(core.color) && core.box.isFinite() &&
           SkIsFinite(core.radii[0], core.radii[1], core.radii[2], core.radii[3]) &&
           SkIsFinite(core.pxScale) && core.pxScale > 0 && SkIsFinite(core.sigma) &&
           core.sigma >= 0 && SkIsFinite(core.maskLobes);
}

void set_core(SkRuntimeEffectBuilder* builder, const SkGlowShader::Core* core) {
    const SkGlowShader::Core none;
    const SkGlowShader::Core& c = core ? *core : none;
    builder->uniform("coreColor") = v4(c.color);
    builder->uniform("coreBox") = SkV4{c.box.fLeft, c.box.fTop, c.box.width(), c.box.height()};
    builder->uniform("coreRadii") = SkV4{c.radii[0], c.radii[1], c.radii[2], c.radii[3]};
    builder->uniform("pxScale") = c.pxScale;
    builder->uniform("coreSigma") = c.sigma;
    builder->uniform("coreMask") = c.maskLobes != 0 ? 1.f : 0.f;
}

}  // namespace

SkRuntimeEffect* SkGlowShaderPriv::MakeErfEffect(int lobes,
                                                 const SkRuntimeEffect::Options& options) {
    SkString code(kCoreCode);
    code.appendf(kErfCode, lobes, lobes, lobes, lobes);
    return SkMakeRuntimeEffect(SkRuntimeEffect::MakeForShader, code.c_str(), options);
}

SkRuntimeEffect* SkGlowShaderPriv::MakeRadialEffect(const SkRuntimeEffect::Options& options) {
    SkString code(kCoreCode);
    code.append(kRadialCode);
    return SkMakeRuntimeEffect(SkRuntimeEffect::MakeForShader, code.c_str(), options);
}

sk_sp<SkShader> SkGlowShader::Make(SkSpan<const Lobe> lobes, const Core* core) {
    if (lobes.size() > static_cast<size_t>(kMaxLobes) || (core && !valid(*core))) {
        return nullptr;
    }
    for (const Lobe& l : lobes) {
        if (!(l.sigma > 0) || !SkIsFinite(l.sigma, l.weight) || !l.box.isFinite() ||
            !finite(l.color)) {
            return nullptr;
        }
    }
    int bin = 0;
    while (SkGlowShaderPriv::kErfBins[bin] < static_cast<int>(lobes.size())) {
        ++bin;
    }
    const auto key = static_cast<StableKey>(static_cast<int>(StableKey::kGlowErfBase) + bin);
    SkRuntimeEffectBuilder builder(sk_ref_sp(GetKnownRuntimeEffect(key)));

    // Every bin's arrays are full length; entries past `count` are never read.
    std::array<SkV2, kMaxLobes> k{};
    std::array<SkV4, kMaxLobes> box{}, colour{};
    for (size_t i = 0; i < lobes.size(); ++i) {
        const Lobe& l = lobes[i];
        k[i] = {1 / (SK_FloatSqrt2 * l.sigma), l.weight};
        box[i] = {l.box.fLeft, l.box.fTop, l.box.fRight, l.box.fBottom};
        colour[i] = v4(l.color);
    }
    const int length = SkGlowShaderPriv::kErfBins[bin];
    builder.uniform("count") = static_cast<float>(lobes.size());
    builder.uniform("lobe").set(k.data(), length);
    builder.uniform("box").set(box.data(), length);
    builder.uniform("colour").set(colour.data(), length);
    set_core(&builder, core);
    return builder.makeShader();
}

sk_sp<SkShader> SkGlowShader::MakeRadial(sk_sp<SkImage> profile, int length, float step,
                                         const PMColor& tint, const Core* core) {
    // A profile carries any blur in its texels, so its core is sharp.
    if (!profile || length < 1 || !SkIsFinite(step) || !(step > 0) || !finite(tint) ||
        (core && (!valid(*core) || core->sigma > 0))) {
        return nullptr;
    }
    if (length > static_cast<int64_t>(profile->width()) * profile->height()) {
        return nullptr;
    }
    SkRuntimeEffectBuilder builder(sk_ref_sp(GetKnownRuntimeEffect(StableKey::kGlowRadial)));
    builder.child("lut") = profile->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                               SkSamplingOptions(SkFilterMode::kNearest));
    builder.uniform("tint") = v4(tint);
    builder.uniform("invStep") = 1 / step;
    builder.uniform("last") = static_cast<float>(length - 1);
    builder.uniform("lutWidth") = static_cast<float>(profile->width());
    set_core(&builder, core);
    return builder.makeShader();
}
