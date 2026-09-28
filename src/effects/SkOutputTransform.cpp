/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/effects/SkOutputTransform.h"

#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkTileMode.h"
#include "include/private/base/SkFloatingPoint.h"
#include "include/private/base/SkTPin.h"
#include "src/core/SkKnownRuntimeEffects.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/effects/SkOutputTransformPriv.h"

#include <iterator>

using SkKnownRuntimeEffects::StableKey;
using ToneCurve = SkOutputTransform::ToneCurve;

static_assert(static_cast<int>(StableKey::kToneNeutral) - static_cast<int>(StableKey::kToneBase) ==
              static_cast<int>(ToneCurve::kLast));

namespace {

// Every stage runs on unpremultiplied colour; zero coverage carries no colour.
constexpr char kStageMain[] = R"(
half4 main(half4 color) {
  float a = float(color.a);
  float3 c = a > 0.0 ? float3(color.rgb) / a : float3(0.0);
  c = %s;
  return half4(half3(c) * half(a), half(a));
}
)";

// sRGB transfer (IEC 61966-2-1), float precision.
constexpr char kTransferCode[] = R"(
float3 srgbToLinear(float3 c) {
  float3 lo = c / 12.92;
  float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, float3(2.4));
  return mix(lo, hi, step(float3(0.04045), c));
}
float3 linearToSrgb(float3 c) {
  c = max(c, 0.0);
  return mix(c * 12.92, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, step(float3(0.0031308), c));
}
)";

// three.js tonemapping_pars_fragment (r182) in SkSL; float maths because AgX loses digits in fp16.
constexpr char kTonePrelude[] = R"(
uniform float exposure;
float3 saturate3(float3 v) { return clamp(v, 0.0, 1.0); }
)";

constexpr const char* kToneCurves[] = {
// kLinear
R"(
float3 toneMap(float3 color) {
  return saturate3(exposure * color);
})",
// kReinhard
R"(
float3 toneMap(float3 color) {
  color *= exposure;
  return saturate3(color / (float3(1.0) + color));
})",
// kCineon
R"(
float3 toneMap(float3 color) {
  color *= exposure;
  color = max(float3(0.0), color - 0.004);
  return pow((color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06), float3(2.2));
})",
// kACES
R"(
float3 RRTAndODTFit(float3 v) {
  float3 a = v * (v + 0.0245786) - 0.000090537;
  float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return a / b;
}
float3 toneMap(float3 color) {
  float3x3 ACESInputMat = float3x3(
    float3(0.59719, 0.07600, 0.02840),
    float3(0.35458, 0.90834, 0.13383),
    float3(0.04823, 0.01566, 0.83777));
  float3x3 ACESOutputMat = float3x3(
    float3(1.60475, -0.10208, -0.00327),
    float3(-0.53108, 1.10813, -0.07276),
    float3(-0.07367, -0.00605, 1.07602));
  color *= exposure / 0.6;
  color = ACESInputMat * color;
  color = RRTAndODTFit(color);
  color = ACESOutputMat * color;
  return saturate3(color);
})",
// kAgX
R"(
float3 agxDefaultContrastApprox(float3 x) {
  float3 x2 = x * x;
  float3 x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 toneMap(float3 color) {
  float3x3 LINEAR_REC2020_TO_LINEAR_SRGB = float3x3(
    float3(1.6605, -0.1246, -0.0182),
    float3(-0.5876, 1.1329, -0.1006),
    float3(-0.0728, -0.0083, 1.1187));
  float3x3 LINEAR_SRGB_TO_LINEAR_REC2020 = float3x3(
    float3(0.6274, 0.0691, 0.0164),
    float3(0.3293, 0.9195, 0.0880),
    float3(0.0433, 0.0113, 0.8956));
  float3x3 AgXInsetMatrix = float3x3(
    float3(0.856627153315983, 0.137318972929847, 0.11189821299995),
    float3(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
    float3(0.0482516061458583, 0.101439036467562, 0.811302368396859));
  float3x3 AgXOutsetMatrix = float3x3(
    float3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
    float3(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
    float3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
  float AgxMinEv = -12.47393;
  float AgxMaxEv = 4.026069;
  color *= exposure;
  color = LINEAR_SRGB_TO_LINEAR_REC2020 * color;
  color = AgXInsetMatrix * color;
  color = max(color, 1e-10);
  color = log2(color);
  color = (color - AgxMinEv) / (AgxMaxEv - AgxMinEv);
  color = clamp(color, 0.0, 1.0);
  color = agxDefaultContrastApprox(color);
  color = AgXOutsetMatrix * color;
  color = pow(max(float3(0.0), color), float3(2.2));
  color = LINEAR_REC2020_TO_LINEAR_SRGB * color;
  return clamp(color, 0.0, 1.0);
})",
// kNeutral
R"(
float3 toneMap(float3 color) {
  float StartCompression = 0.8 - 0.04;
  float Desaturation = 0.15;
  color *= exposure;
  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;
  float peak = max(color.r, max(color.g, color.b));
  if (peak < StartCompression) return color;
  float d = 1.0 - StartCompression;
  float newPeak = 1.0 - d * d / (peak + d - StartCompression);
  color *= newPeak / peak;
  float g = 1.0 - 1.0 / (Desaturation * (peak - newPeak) + 1.0);
  return mix(color, float3(newPeak), g);
})",
};
static_assert(std::size(kToneCurves) == static_cast<size_t>(ToneCurve::kLast) + 1);

// The parametric grade; curves interpolate in the shader between texel-centre samples.
constexpr char kGradeCode[] = R"(
uniform float3x3 gradeBalance;
uniform float3 gradeLift;
uniform float3 gradeSlope;
uniform float3 gradePower;
uniform float gradeContrast;
uniform float gradeSaturation;
uniform float gradeVibrance;
uniform float3x3 gradeHue;
uniform shader gradeCurves;

float3 gradeSignedPow(float3 v, float3 p) { return sign(v) * pow(abs(v), p); }
float3 gradeTowards(float3 c, float amount) {
  float y = dot(c, float3(0.2126, 0.7152, 0.0722));
  return mix(float3(y), c, amount);
}
float3 gradeCurveRow(float v, float row) {
  float t = clamp(v, 0.0, 1.0) * 255.0;
  float i = min(floor(t), 254.0);
  float3 lo = float3(gradeCurves.eval(float2(i + 0.5, row)).rgb);
  float3 hi = float3(gradeCurves.eval(float2(i + 1.5, row)).rgb);
  return lo + (hi - lo) * (t - i) + (v - clamp(v, 0.0, 1.0));
}
float3 gradeRgb(float3 c) {
  c = gradeBalance * c;
  c = gradeSignedPow(c * gradeSlope + gradeLift, gradePower);
  c = gradeSignedPow(c / 0.18, float3(gradeContrast)) * 0.18;
  c = gradeTowards(c, gradeSaturation);
  float hi = max(c.r, max(c.g, c.b));
  float chroma = hi > 0.0 ? clamp((hi - min(c.r, min(c.g, c.b))) / hi, 0.0, 1.0) : 0.0;
  c = gradeTowards(c, 1.0 + gradeVibrance * (1.0 - chroma));
  c = gradeHue * c;
  c = float3(gradeCurveRow(c.r, 0.5).r, gradeCurveRow(c.g, 0.5).g, gradeCurveRow(c.b, 0.5).b);
  return float3(gradeCurveRow(c.r, 1.5).r, gradeCurveRow(c.g, 1.5).r, gradeCurveRow(c.b, 1.5).r);
}
)";
constexpr int kCurveTableSize = 256;

// A slice strip: bilinear in red/green from the sampler, blue interpolated here.
constexpr char kLutCode[] = R"(
uniform shader lut;
uniform float4 lutMin;
uniform float4 lutMax;
uniform float lutSize;
uniform float lutIntensity;
float3 lutSlice(float3 c, float b) {
  float2 p = float2(b * lutSize + 0.5 + c.r * (lutSize - 1.0), 0.5 + c.g * (lutSize - 1.0));
  return float3(lut.eval(p).rgb);
}
float3 lutGrade(float3 c) {
  float3 d = clamp((c - lutMin.rgb) / max(lutMax.rgb - lutMin.rgb, 1e-6), 0.0, 1.0);
  float bPos = d.b * (lutSize - 1.0);
  float b0 = floor(bPos);
  float3 graded = mix(lutSlice(d, b0), lutSlice(d, min(b0 + 1.0, lutSize - 1.0)), bPos - b0);
  return mix(c, graded, lutIntensity);
}
)";

SkRuntimeEffect* make_stage(const char* prelude, const char* statement,
                            const SkRuntimeEffect::Options& options) {
    SkString code(prelude);
    code.appendf(kStageMain, statement);
    return SkMakeRuntimeEffect(SkRuntimeEffect::MakeForColorFilter, code.c_str(), options);
}

const SkRuntimeEffect* effect(StableKey key) { return GetKnownRuntimeEffect(key); }

}  // namespace

SkRuntimeEffect* SkOutputTransformPriv::MakeDecodeEffect(const SkRuntimeEffect::Options& options) {
    // Light above coverage is linear already, so only the surface part decodes.
    return make_stage(kTransferCode, "srgbToLinear(min(c, float3(1.0))) + max(c - 1.0, 0.0)",
                      options);
}

SkRuntimeEffect* SkOutputTransformPriv::MakeEncodeEffect(const SkRuntimeEffect::Options& options) {
    return make_stage(kTransferCode, "linearToSrgb(c)", options);
}

SkRuntimeEffect* SkOutputTransformPriv::MakeToneEffect(ToneCurve curve,
                                                       const SkRuntimeEffect::Options& options) {
    SkString prelude(kTonePrelude);
    prelude.append(kToneCurves[static_cast<int>(curve)]);
    return make_stage(prelude.c_str(), "toneMap(c)", options);
}

SkRuntimeEffect* SkOutputTransformPriv::MakeGradeEffect(const SkRuntimeEffect::Options& options) {
    return make_stage(kGradeCode, "gradeRgb(c)", options);
}

SkRuntimeEffect* SkOutputTransformPriv::MakeLutEffect(const SkRuntimeEffect::Options& options) {
    return make_stage(kLutCode, "lutGrade(c)", options);
}

sk_sp<SkColorFilter> SkOutputTransform::Decode() {
    return effect(StableKey::kOutputDecode)->makeColorFilter(/*uniforms=*/nullptr);
}

sk_sp<SkColorFilter> SkOutputTransform::Encode() {
    return effect(StableKey::kOutputEncode)->makeColorFilter(/*uniforms=*/nullptr);
}

sk_sp<SkColorFilter> SkOutputTransform::ToneMap(ToneCurve curve, float exposure) {
    if (curve < ToneCurve::kLinear || curve > ToneCurve::kLast || !SkIsFinite(exposure)) {
        return nullptr;
    }
    const auto key = static_cast<StableKey>(static_cast<int>(StableKey::kToneBase) +
                                            static_cast<int>(curve));
    return effect(key)->makeColorFilter(SkData::MakeWithCopy(&exposure, sizeof(exposure)));
}

sk_sp<SkColorFilter> SkOutputTransform::Grade(SkSpan<const float> uniforms, sk_sp<SkImage> curves) {
    const SkRuntimeEffect* grade = effect(StableKey::kOutputGrade);
    if (uniforms.size() != kGradeUniformCount || uniforms.size_bytes() != grade->uniformSize() ||
        !curves || curves->width() != kCurveTableSize || curves->height() != 2) {
        return nullptr;
    }
    sk_sp<SkShader> table = curves->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                               SkSamplingOptions(SkFilterMode::kNearest));
    SkRuntimeEffect::ChildPtr children[] = {std::move(table)};
    return grade->makeColorFilter(SkData::MakeWithCopy(uniforms.data(), uniforms.size_bytes()),
                                  children);
}

sk_sp<SkColorFilter> SkOutputTransform::LutStrip(sk_sp<SkImage> strip, int size, float intensity,
                                                 const SkV3& domainMin, const SkV3& domainMax) {
    if (!strip || size < 2 || strip->width() != size * size || strip->height() != size ||
        !SkIsFinite(intensity, domainMin.x, domainMin.y, domainMin.z) ||
        !SkIsFinite(domainMax.x, domainMax.y, domainMax.z)) {
        return nullptr;
    }
    SkRuntimeEffectBuilder builder(sk_ref_sp(effect(StableKey::kOutputLut)));
    builder.child("lut") = strip->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                             SkSamplingOptions(SkFilterMode::kLinear));
    builder.uniform("lutMin") = SkV4{domainMin.x, domainMin.y, domainMin.z, 0};
    builder.uniform("lutMax") = SkV4{domainMax.x, domainMax.y, domainMax.z, 0};
    builder.uniform("lutSize") = static_cast<float>(size);
    builder.uniform("lutIntensity") = SkTPin(intensity, 0.f, 1.f);
    return builder.makeColorFilter();
}
