/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBlendMode.h"
#include "include/core/SkBlender.h"
#include "include/core/SkData.h"
#include "include/core/SkString.h"
#include "include/effects/SkBlenders.h"
#include "src/core/SkKnownRuntimeEffects.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/effects/SkLightResolvePriv.h"

using SkKnownRuntimeEffects::StableKey;

namespace {

// CSS transfer functions: sRGB below coverage, light above coverage kept linear.
constexpr char kTransferCode[] = R"(
float3 cssEncode(float3 c) {
  return mix(c*12.92,1.055*pow(max(c,float3(0)),float3(1.0/2.4))-0.055,step(float3(0.0031308),c));
}
float3 cssDecode(float3 c) {
  return mix(c/12.92,pow(max((c+0.055)/1.055,float3(0)),float3(2.4)),step(float3(0.04045),c));
}
float4 cssEncoded(float4 c) {
  if (c.a <= 0) return c;
  float3 surface = min(c.rgb, float3(c.a));
  return float4(cssEncode(surface/c.a)*c.a + (c.rgb - surface), c.a);
}
float4 cssLinear(float4 c) {
  if (c.a <= 0) return c;
  float3 surface = min(c.rgb, float3(c.a));
  return float4(cssDecode(surface/c.a)*c.a + (c.rgb - surface), c.a);
}
)";

// mode: 0 adds as stored, 1 decodes the destination first, 2 encodes it first.
constexpr char kResolveCode[] = R"(
uniform float mode;
uniform float emissive;
uniform float layer;
half4 main(half4 source, half4 destination) {
  float4 s = float4(source), d = float4(destination);
  if (layer > 0.5 && s.a > 0) return half4(s + d*(1 - s.a));
  if (emissive > 0.5) {
    if (mode == 2) return half4(half3(cssDecode(cssEncode(d.rgb) + s.rgb)), half(d.a));
    if (mode == 1) d = cssLinear(d);
    d.rgb += s.rgb;
    return half4(mode == 1 ? cssEncoded(d) : d);
  }
  if (mode == 1) d = cssLinear(d); else if (mode == 2) d = cssEncoded(d);
  d += s; d.a = min(d.a, 1); d.rgb = min(d.rgb, float3(d.a));
  if (mode == 1) d = cssEncoded(d); else if (mode == 2) d = cssLinear(d);
  return half4(d);
}
)";

constexpr char kAddCode[] = R"(
half4 main(half4 source, half4 destination) {
  return half4(destination.rgb + source.rgb, destination.a);
}
)";

float resolve_mode(SkBlenders::LightSpace into, SkBlenders::LightSpace light) {
    return into == light ? 0 : into == SkBlenders::LightSpace::kEncoded ? 1 : 2;
}

sk_sp<SkBlender> make_resolve(float mode, bool emissive, bool layer) {
    const float uniforms[] = {mode, emissive ? 1.f : 0.f, layer ? 1.f : 0.f};
    return GetKnownRuntimeEffect(StableKey::kLightResolve)
            ->makeBlender(SkData::MakeWithCopy(uniforms, sizeof(uniforms)));
}

}  // namespace

SkRuntimeEffect* SkLightResolvePriv::MakeResolveEffect(const SkRuntimeEffect::Options& options) {
    SkString code(kTransferCode);
    code.append(kResolveCode);
    return SkMakeRuntimeEffect(SkRuntimeEffect::MakeForBlender, code.c_str(), options);
}

SkRuntimeEffect* SkLightResolvePriv::MakeAddEffect(const SkRuntimeEffect::Options& options) {
    return SkMakeRuntimeEffect(SkRuntimeEffect::MakeForBlender, kAddCode, options);
}

sk_sp<SkBlender> SkBlenders::LightResolve(LightSpace into, LightSpace light, bool emissive) {
    if (emissive && into == light) {
        return GetKnownRuntimeEffect(StableKey::kLightAdd)->makeBlender(/*uniforms=*/nullptr);
    }
    return make_resolve(resolve_mode(into, light), emissive, /*layer=*/false);
}

sk_sp<SkBlender> SkBlenders::LayerResolve(LightSpace into) {
    // Uncovered light in a layer is linear, so into a linear frame the whole rule is source-over.
    if (into == LightSpace::kLinear) {
        return SkBlender::Mode(SkBlendMode::kSrcOver);
    }
    return make_resolve(resolve_mode(into, LightSpace::kLinear), /*emissive=*/true, /*layer=*/true);
}
