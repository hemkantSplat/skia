
/*
 * Copyright 2010 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef GrColor_DEFINED
#define GrColor_DEFINED

#include "include/core/SkColor.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/private/gpu/ganesh/GrTypesPriv.h"
#include "src/base/SkHalf.h"
#include "src/core/SkColorData.h"
#include "src/core/SkColorPriv.h"
#include "src/gpu/BufferWriter.h"
#include "src/gpu/ganesh/GrCaps.h"

/**
 * GrColor is 4 bytes for R, G, B, A, in a specific order defined below. Whether the color is
 * premultiplied or not depends on the context in which it is being used.
 */
typedef uint32_t GrColor;

// shift amount to assign a component to a GrColor int
// These shift values are chosen for compatibility with GL attrib arrays
// ES doesn't allow BGRA vertex attrib order so if they were not in this order
// we'd have to swizzle in shaders.
#ifdef SK_CPU_BENDIAN
    #define GrColor_SHIFT_R     24
    #define GrColor_SHIFT_G     16
    #define GrColor_SHIFT_B     8
    #define GrColor_SHIFT_A     0
#else
    #define GrColor_SHIFT_R     0
    #define GrColor_SHIFT_G     8
    #define GrColor_SHIFT_B     16
    #define GrColor_SHIFT_A     24
#endif

/**
 *  Pack 4 components (RGBA) into a GrColor int
 */
static inline GrColor GrColorPackRGBA(unsigned r, unsigned g, unsigned b, unsigned a) {
    SkASSERT((uint8_t)r == r);
    SkASSERT((uint8_t)g == g);
    SkASSERT((uint8_t)b == b);
    SkASSERT((uint8_t)a == a);
    return  (r << GrColor_SHIFT_R) |
            (g << GrColor_SHIFT_G) |
            (b << GrColor_SHIFT_B) |
            (a << GrColor_SHIFT_A);
}

// extract a component (byte) from a GrColor int

#define GrColorUnpackR(color)   (((color) >> GrColor_SHIFT_R) & 0xFF)
#define GrColorUnpackG(color)   (((color) >> GrColor_SHIFT_G) & 0xFF)
#define GrColorUnpackB(color)   (((color) >> GrColor_SHIFT_B) & 0xFF)
#define GrColorUnpackA(color)   (((color) >> GrColor_SHIFT_A) & 0xFF)

/**
 *  Since premultiplied means that alpha >= color, we construct a color with
 *  each component==255 and alpha == 0 to be "illegal"
 */
#define GrColor_ILLEGAL     (~(0xFF << GrColor_SHIFT_A))

/** Normalizes and coverts an uint8_t to a float. [0, 255] -> [0.0, 1.0] */
static inline float GrNormalizeByteToFloat(uint8_t value) {
    static const float ONE_OVER_255 = 1.f / 255.f;
    return value * ONE_OVER_255;
}

// Vertex precision follows the destination, including normalized formats wider than eight bits.
static inline bool GrVertexColorIsWide(const SkPMColor4f& color, GrClampType clampType) {
    return clampType != GrClampType::kAuto || !color.fitsInBytes();
}

static inline skgpu::VertexColorType GrVertexColorStorage(const SkPMColor4f& color,
                                                         GrClampType clampType,
                                                         const GrCaps& caps) {
    if (!GrVertexColorIsWide(color, clampType)) {
        return skgpu::VertexColorType::kByte;
    }
    return caps.halfFloatVertexAttributeSupport() ? skgpu::VertexColorType::kHalf
                                                 : skgpu::VertexColorType::kFloat;
}

static inline uint64_t SkPMColor4f_toFP16(const SkPMColor4f& color) {
    uint64_t halfColor;
    to_half(skvx::float4::Load(color.vec())).store(&halfColor);
    return halfColor;
}

#endif
