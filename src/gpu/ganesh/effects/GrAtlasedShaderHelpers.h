/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrAtlasedShaderHelpers_DEFINED
#define GrAtlasedShaderHelpers_DEFINED

#include "include/private/base/SkAssert.h"
#include "src/core/SkSLTypeShared.h"
#include "src/gpu/ganesh/GrGeometryProcessor.h"
#include "src/gpu/ganesh/GrShaderCaps.h"
#include "src/gpu/ganesh/glsl/GrGLSLFragmentShaderBuilder.h"
#include "src/gpu/ganesh/glsl/GrGLSLVarying.h"
#include "src/gpu/ganesh/glsl/GrGLSLVertexGeoBuilder.h"

static inline void append_index_uv_varyings(GrGeometryProcessor::ProgramImpl::EmitArgs& args,
                                            int numTextureSamplers,
                                            const char* inTexCoordsName,
                                            const char* inAtlasRectName, int inset,
                                            const char* atlasDimensionsInvName,
                                            GrGLSLVarying* uv,
                                            GrGLSLVarying* texIdx,
                                            GrGLSLVarying* st) {
    using Interpolation = GrGLSLVaryingHandler::Interpolation;
    const auto integerInterpolation = args.fShaderCaps->fFlatInterpolationSupport
            ? Interpolation::kMustBeFlat : Interpolation::kInterpolated;
    // This extracts the texture index and texel coordinates from the same variable
    // Packing structure: texel coordinates have the 2-bit texture page encoded in bits 13 & 14 of
    // the x coordinate. It would be nice to use bits 14 and 15, but iphone6 has problem with those
    // bits when in gles. Iphone6 works fine with bits 14 and 15 in metal.
    if (args.fShaderCaps->fIntegerSupport) {
        if (numTextureSamplers <= 1) {
            args.fVertBuilder->codeAppendf(
                "int texIdx = 0;"
                "float2 unormTexCoords = float2(%s.x, %s.y);"
           , inTexCoordsName, inTexCoordsName);
        } else {
            args.fVertBuilder->codeAppendf(
                "int2 coords = int2(%s.x, %s.y);"
                "int texIdx = coords.x >> 13;"
                "float2 unormTexCoords = float2(coords.x & 0x1FFF, coords.y);"
            , inTexCoordsName, inTexCoordsName);
        }
    } else {
        if (numTextureSamplers <= 1) {
            args.fVertBuilder->codeAppendf(
                "float texIdx = 0;"
                "float2 unormTexCoords = float2(%s.x, %s.y);"
            , inTexCoordsName, inTexCoordsName);
        } else {
            args.fVertBuilder->codeAppendf(
                "float2 coord = float2(%s.x, %s.y);"
                "float texIdx = floor(coord.x * exp2(-13));"
                "float2 unormTexCoords = float2(coord.x - texIdx * exp2(13), coord.y);"
            , inTexCoordsName, inTexCoordsName);
        }
    }

    // Interpolate only mask-local texels; packing remains flat per primitive.
    uv->reset(SkSLType::kFloat2);
    args.fVaryingHandler->addVarying("LocalTextureCoords", uv);
    args.fVertBuilder->codeAppendf(
            "%s = float2(%s) - float2(%s.xy);", uv->vsOut(), inTexCoordsName, inAtlasRectName);
    GrGLSLVarying origin(SkSLType::kFloat4);
    args.fVaryingHandler->addVarying("AtlasOrigin", &origin, integerInterpolation);
    args.fVertBuilder->codeAppendf(
            "%s = float4(unormTexCoords - %s, %s.zw);",
            origin.vsOut(), uv->vsOut(), inAtlasRectName);
    // Recover integer attributes exactly on backends without flat interpolation.
    args.fFragBuilder->codeAppendf("float4 atlasRect = floor(%s + 0.5);"
                                  "float2 atlasOrigin = atlasRect.xy; float2 atlasInv = %s;"
                                  "float4 atlasBounds = float4(atlasOrigin - %d + 0.5,"
                                  "atlasOrigin + atlasRect.zw + %d - 0.5);",
                                  origin.fsIn(), atlasDimensionsInvName, inset, inset);

    // On ANGLE there is a significant cost to using an int varying. We don't know of any case where
    // it is worse to use a float so for now we always do.
    texIdx->reset(SkSLType::kFloat);
    // If we computed the local var "texIdx" as an int we will need to cast it to float
    const char* cast = args.fShaderCaps->fIntegerSupport ? "float" : "";
    args.fVaryingHandler->addVarying("TexIndex", texIdx, integerInterpolation);
    args.fVertBuilder->codeAppendf("%s = %s(texIdx);", texIdx->vsOut(), cast);

    if (st) {
        *st = *uv;
    }
}

static inline void append_multitexture_lookup(GrGeometryProcessor::ProgramImpl::EmitArgs& args,
                                              int numTextureSamplers,
                                              const GrGLSLVarying& texIdx,
                                              const char* coordName,
                                              const char* colorName) {
    SkASSERT(numTextureSamplers > 0);
    // This shouldn't happen, but will avoid a crash if it does
    if (numTextureSamplers <= 0) {
        args.fFragBuilder->codeAppendf("%s = float4(1);", colorName);
        return;
    }

    auto* frag = args.fFragBuilder;
    const bool linear = args.fGeomProc.textureSampler(0).samplerState().filter() ==
                        GrSamplerState::Filter::kLinear;
    frag->codeAppend("{");
    if (linear) {
        frag->codeAppendf("float2 p = %s - 0.5; float2 base = floor(p);"
                          "float2 weight = p - base;", coordName);
    } else {
        frag->codeAppendf("float2 base = floor(%s);", coordName);
    }
    // Integer texel centres and power-of-two normalization are exact at every atlas origin.
    frag->codeAppend("float2 centre = atlasOrigin + base + 0.5;");
    for (int i = 0; i < numTextureSamplers; ++i) {
        if (i < numTextureSamplers - 1) {
            frag->codeAppendf("if (floor(%s + 0.5) == %d) {", texIdx.fsIn(), i);
        } else {
            frag->codeAppend("{");
        }
        if (linear) {
            const char* offsets[] = {"float2(0,0)", "float2(1,0)",
                                     "float2(0,1)", "float2(1,1)"};
            for (int tap = 0; tap < 4; ++tap) {
                frag->codeAppendf("float2 tap%d = clamp(centre + %s, atlasBounds.xy, atlasBounds.zw) * atlasInv;"
                                  "float4 c%d = float4(", tap, offsets[tap], tap);
                SkString coord = SkStringPrintf("tap%d", tap);
                frag->appendTextureLookup(args.fTexSamplers[i], coord.c_str());
                frag->codeAppend(");");
            }
            frag->codeAppendf("%s = half4(mix(mix(c0, c1, weight.x),"
                              "mix(c2, c3, weight.x), weight.y));", colorName);
        } else {
            frag->codeAppendf("float2 tap = clamp(centre, atlasBounds.xy, atlasBounds.zw) * atlasInv; %s = ", colorName);
            frag->appendTextureLookup(args.fTexSamplers[i], "tap");
            frag->codeAppend(";");
        }
        frag->codeAppend(i < numTextureSamplers - 1 ? "} else " : "}");
    }
    frag->codeAppend("}");
}

// Special lookup function for sdf lcd -- avoids duplicating conditional logic three times
static inline void append_multitexture_lookup_lcd(GrGeometryProcessor::ProgramImpl::EmitArgs& args,
                                                  int numTextureSamplers,
                                                  const GrGLSLVarying& texIdx,
                                                  const char* coordName,
                                                  const char* offsetName,
                                                  const char* distanceName) {
    SkASSERT(numTextureSamplers > 0);
    // This shouldn't happen, but will avoid a crash if it does
    if (numTextureSamplers <= 0) {
        args.fFragBuilder->codeAppendf("%s = half3(1);", distanceName);
        return;
    }

    args.fFragBuilder->codeAppend("{ half4 sampleColor;");
    append_multitexture_lookup(args, numTextureSamplers, texIdx, coordName, "sampleColor");
    args.fFragBuilder->codeAppendf("%s.y = sampleColor.r;"
                                  "float2 adjusted = %s - %s;",
                                  distanceName, coordName, offsetName);
    append_multitexture_lookup(args, numTextureSamplers, texIdx, "adjusted", "sampleColor");
    args.fFragBuilder->codeAppendf("%s.x = sampleColor.r; adjusted = %s + %s;",
                                  distanceName, coordName, offsetName);
    append_multitexture_lookup(args, numTextureSamplers, texIdx, "adjusted", "sampleColor");
    args.fFragBuilder->codeAppendf("%s.z = sampleColor.r; }", distanceName);
}

#endif
