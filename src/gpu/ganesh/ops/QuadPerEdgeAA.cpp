/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "src/gpu/ganesh/GrColor.h"
#include "src/gpu/ganesh/ops/QuadPerEdgeAA.h"

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkRect.h"
#include "include/private/base/SkMath.h"
#include "src/base/SkArenaAlloc.h"
#include "src/base/SkVx.h"
#include "src/core/SkSLTypeShared.h"
#include "src/gpu/KeyBuilder.h"
#include "src/gpu/ganesh/GrBuffer.h"
#include "src/gpu/ganesh/GrCaps.h"
#include "src/gpu/ganesh/GrColorSpaceXform.h"
#include "src/gpu/ganesh/GrGeometryProcessor.h"
#include "src/gpu/ganesh/GrMeshDrawTarget.h"
#include "src/gpu/ganesh/GrOpsRenderPass.h"
#include "src/gpu/ganesh/GrResourceProvider.h"
#include "src/gpu/ganesh/GrShaderVar.h"
#include "src/gpu/ganesh/geometry/GrQuadUtils.h"
#include "src/gpu/ganesh/glsl/GrGLSLColorSpaceXformHelper.h"
#include "src/gpu/ganesh/glsl/GrGLSLFragmentShaderBuilder.h"
#include "src/gpu/ganesh/glsl/GrGLSLVarying.h"
#include "src/gpu/ganesh/glsl/GrGLSLVertexGeoBuilder.h"

#include <cstdint>
#include <memory>
#include <utility>

class GrBackendFormat;
class GrGLSLProgramDataManager;
namespace skgpu {
class Swizzle;
}

static_assert((int)GrQuadAAFlags::kLeft   == SkCanvas::kLeft_QuadAAFlag);
static_assert((int)GrQuadAAFlags::kTop    == SkCanvas::kTop_QuadAAFlag);
static_assert((int)GrQuadAAFlags::kRight  == SkCanvas::kRight_QuadAAFlag);
static_assert((int)GrQuadAAFlags::kBottom == SkCanvas::kBottom_QuadAAFlag);
static_assert((int)GrQuadAAFlags::kNone   == SkCanvas::kNone_QuadAAFlags);
static_assert((int)GrQuadAAFlags::kAll    == SkCanvas::kAll_QuadAAFlags);

namespace skgpu::ganesh::QuadPerEdgeAA {

namespace {

using VertexSpec = skgpu::ganesh::QuadPerEdgeAA::VertexSpec;
using CoverageMode = skgpu::ganesh::QuadPerEdgeAA::CoverageMode;
using ColorType = skgpu::ganesh::QuadPerEdgeAA::ColorType;

// Generic WriteQuadProc that can handle any VertexSpec. It writes the 4 vertices in triangle strip
// order, although the data per-vertex is dependent on the VertexSpec.
void write_quad_generic(VertexWriter* vb,
                        const VertexSpec& spec,
                        const GrQuad* deviceQuad,
                        const GrQuad* localQuad,
                        const float coverage[4],
                        const SkPMColor4f& color,
                        const SkRect& geomSubset,
                        const SkRect& texSubset,
                    const SkMatrix& deviceToLocal,
                    const SkPoint3& textureNormalization) {
    static constexpr auto If = VertexWriter::If<float>;

    SkASSERT(!spec.hasLocalCoords() || localQuad);

    CoverageMode mode = spec.coverageMode();
    for (int i = 0; i < 4; ++i) {
        // save position, this is a float2 or float3 or float4 depending on the combination of
        // perspective and coverage mode.
        *vb << deviceQuad->x(i)
            << deviceQuad->y(i)
            << If(spec.deviceQuadType() == GrQuad::Type::kPerspective, deviceQuad->w(i))
            << If(mode == CoverageMode::kWithPosition, coverage[i]);

        // save color
        if (spec.hasVertexColors()) {
            auto wide = spec.colorStorage();
            *vb << VertexColor(color * (mode == CoverageMode::kWithColor ? coverage[i] : 1), wide);
        }

        // Repeat the original draw matrix, independent of tessellated positions.
        if (spec.hasLocalCoords()) {
            for (int row = 0; row < 3; ++row) {
                *vb << deviceToLocal[3 * row] << deviceToLocal[3 * row + 1]
                    << deviceToLocal[3 * row + 2];
            }
        }

        if (spec.hasTexture()) {
            *vb << textureNormalization.fX << textureNormalization.fY << textureNormalization.fZ;
        }

        // save the geometry subset
        if (spec.requiresGeometrySubset()) {
            *vb << geomSubset;
        }

        // save the texture subset
        if (spec.hasSubset()) {
            *vb << texSubset;
        }
    }
}

// Specialized WriteQuadProcs for particular VertexSpecs that show up frequently (determined
// experimentally through recorded GMs, SKPs, and SVGs, as well as SkiaRenderer's usage patterns):

// 2D (XY), no explicit coverage, vertex color, no locals, no geometry subset, no texture subsetn
// This represents simple, solid color or shader, non-AA (or AA with cov. as alpha) rects.
void write_2d_color(VertexWriter* vb,
                    const VertexSpec& spec,
                    const GrQuad* deviceQuad,
                    const GrQuad* localQuad,
                    const float coverage[4],
                    const SkPMColor4f& color,
                    const SkRect& geomSubset,
                    const SkRect& texSubset,
                    const SkMatrix& deviceToLocal,
                    const SkPoint3& textureNormalization) {
    // Assert assumptions about VertexSpec
    SkASSERT(spec.deviceQuadType() != GrQuad::Type::kPerspective);
    SkASSERT(!spec.hasLocalCoords());
    SkASSERT(spec.coverageMode() == CoverageMode::kNone ||
             spec.coverageMode() == CoverageMode::kWithColor);
    SkASSERT(spec.hasVertexColors());
    SkASSERT(!spec.requiresGeometrySubset());
    SkASSERT(!spec.hasSubset());
    // We don't assert that localQuad == nullptr, since it is possible for FillRectOp to
    // accumulate local coords conservatively (paint not trivial), and then after analysis realize
    // the processors don't need local coordinates.

    auto wide = spec.colorStorage();
    for (int i = 0; i < 4; ++i) {
        // If this is not coverage-with-alpha, make sure coverage == 1 so it doesn't do anything
        SkASSERT(spec.coverageMode() == CoverageMode::kWithColor || coverage[i] == 1.f);
        *vb << deviceQuad->x(i)
            << deviceQuad->y(i)
            << VertexColor(color * coverage[i], wide);
    }
}

} // anonymous namespace

IndexBufferOption CalcIndexBufferOption(GrAAType aa, int numQuads) {
    if (aa == GrAAType::kCoverage) {
        return IndexBufferOption::kPictureFramed;
    } else if (numQuads > 1) {
        return IndexBufferOption::kIndexedRects;
    } else {
        return IndexBufferOption::kTriStrips;
    }
}

ColorType MinColorType(SkPMColor4f color, GrClampType clampType, const GrCaps& caps) {
    if (GrVertexColorIsWide(color, clampType)) {
        return GrVertexColorStorage(color, clampType, caps) == skgpu::VertexColorType::kHalf
                       ? ColorType::kHalf : ColorType::kFloat;
    }
    if (color == SK_PMColor4fWHITE) {
        return ColorType::kNone;
    } else {
        return ColorType::kByte;
    }
}

////////////////// Tessellator Implementation

Tessellator::WriteQuadProc Tessellator::GetWriteQuadProc(const VertexSpec& spec) {
    if (!spec.hasLocalCoords() && spec.hasVertexColors() &&
        spec.deviceQuadType() != GrQuad::Type::kPerspective &&
        !spec.requiresGeometrySubset() && spec.coverageMode() != CoverageMode::kWithPosition) {
        return write_2d_color;
    }
    return write_quad_generic;
}

Tessellator::Tessellator(const VertexSpec& spec, char* vertices)
        : fVertexSpec(spec)
        , fVertexWriter{vertices}
        , fWriteProc(Tessellator::GetWriteQuadProc(spec)) {}

void Tessellator::append(GrQuad* deviceQuad, GrQuad* localQuad,
                         const SkPMColor4f& color, const SkRect& uvSubset, GrQuadAAFlags aaFlags,
                         const SkMatrix& deviceToLocal,
                        const SkPoint3& textureNormalization) {
    // We allow Tessellator to be created with a null vertices pointer for convenience, but it is
    // assumed it will never actually be used in those cases.
    SkASSERT(fVertexWriter);
    SkASSERT(deviceQuad->quadType() <= fVertexSpec.deviceQuadType());
    SkASSERT(localQuad || !fVertexSpec.hasLocalCoords());
    SkASSERT(!fVertexSpec.hasLocalCoords() || localQuad->quadType() <= fVertexSpec.localQuadType());

    static const float kFullCoverage[4] = {1.f, 1.f, 1.f, 1.f};
    static const float kZeroCoverage[4] = {0.f, 0.f, 0.f, 0.f};
    static const SkRect kIgnoredSubset = SkRect::MakeEmpty();

    if (fVertexSpec.usesCoverageAA()) {
        SkASSERT(fVertexSpec.coverageMode() == CoverageMode::kWithColor ||
                 fVertexSpec.coverageMode() == CoverageMode::kWithPosition);
        // Must calculate inner and outer quadrilaterals for the vertex coverage ramps, and possibly
        // a geometry subset if corners are not right angles
        SkRect geomSubset;
        if (fVertexSpec.requiresGeometrySubset()) {
            // Our GP code expects a 0.5 outset rect (coverage is computed as 0 at the values of
            // the uniform). However, if we have quad edges that aren't supposed to be antialiased
            // they may lie close to the bounds. So in that case we outset by an additional 0.5.
            // This is a sort of backup clipping mechanism for cases where quad outsetting of nearly
            // parallel edges produces long thin extrusions from the original geometry.
            float outset = aaFlags == GrQuadAAFlags::kAll ? 0.5f : 1.f;
            geomSubset = deviceQuad->bounds().makeOutset(outset, outset);
        }

        if (aaFlags == GrQuadAAFlags::kNone) {
            // Have to write the coverage AA vertex structure, but there's no math to be done for a
            // non-aa quad batched into a coverage AA op.
            fWriteProc(&fVertexWriter, fVertexSpec, deviceQuad, localQuad, kFullCoverage, color,
                       geomSubset, uvSubset, deviceToLocal, textureNormalization);
            // Since we pass the same corners in, the outer vertex structure will have 0 area and
            // the coverage interpolation from 1 to 0 will not be visible.
            fWriteProc(&fVertexWriter, fVertexSpec, deviceQuad, localQuad, kZeroCoverage, color,
                       geomSubset, uvSubset, deviceToLocal, textureNormalization);
        } else {
            // Reset the tessellation helper to match the current geometry
            fAAHelper.reset(*deviceQuad, localQuad);

            // Edge inset/outset distance ordered LBTR, set to 0.5 for a half pixel if the AA flag
            // is turned on, or 0.0 if the edge is not anti-aliased.
            skvx::Vec<4, float> edgeDistances;
            if (aaFlags == GrQuadAAFlags::kAll) {
                edgeDistances = 0.5f;
            } else {
                edgeDistances = { (aaFlags & GrQuadAAFlags::kLeft)   ? 0.5f : 0.f,
                                  (aaFlags & GrQuadAAFlags::kBottom) ? 0.5f : 0.f,
                                  (aaFlags & GrQuadAAFlags::kTop)    ? 0.5f : 0.f,
                                  (aaFlags & GrQuadAAFlags::kRight)  ? 0.5f : 0.f };
            }

            // Write inner vertices first
            float coverage[4];
            fAAHelper.inset(edgeDistances, deviceQuad, localQuad).store(coverage);
            fWriteProc(&fVertexWriter, fVertexSpec, deviceQuad, localQuad, coverage, color,
                       geomSubset, uvSubset, deviceToLocal, textureNormalization);

            // Then outer vertices, which use 0.f for their coverage. If the inset was degenerate
            // to a line (had all coverages < 1), tweak the outset distance so the outer frame's
            // narrow axis reaches out to 2px, which gives better animation under translation.
            const bool hairline = aaFlags == GrQuadAAFlags::kAll &&
                                  coverage[0] < 1.f &&
                                  coverage[1] < 1.f &&
                                  coverage[2] < 1.f &&
                                  coverage[3] < 1.f;
            if (hairline) {
                skvx::Vec<4, float> len = fAAHelper.getEdgeLengths();
                // Using max guards us against trying to scale a degenerate triangle edge of 0 len
                // up to 2px. The shuffles are so that edge 0's adjustment is based on the lengths
                // of its connecting edges (1 and 2), and so forth.
                skvx::Vec<4, float> maxWH = max(skvx::shuffle<1, 0, 3, 2>(len),
                                                skvx::shuffle<2, 3, 0, 1>(len));
                // wh + 2e' = 2, so e' = (2 - wh) / 2 => e' = e * (2 - wh). But if w or h > 1, then
                // 2 - wh < 1 and represents the non-narrow axis so clamp to 1.
                edgeDistances *= max(1.f, 2.f - maxWH);
            }
            fAAHelper.outset(edgeDistances, deviceQuad, localQuad);
            fWriteProc(&fVertexWriter, fVertexSpec, deviceQuad, localQuad, kZeroCoverage, color,
                       geomSubset, uvSubset, deviceToLocal, textureNormalization);
        }
    } else {
        // No outsetting needed, just write a single quad with full coverage
        SkASSERT(fVertexSpec.coverageMode() == CoverageMode::kNone &&
                 !fVertexSpec.requiresGeometrySubset());
        fWriteProc(&fVertexWriter, fVertexSpec, deviceQuad, localQuad, kFullCoverage, color,
                   kIgnoredSubset, uvSubset, deviceToLocal, textureNormalization);
    }
}

sk_sp<const GrBuffer> GetIndexBuffer(GrMeshDrawTarget* target,
                                     IndexBufferOption indexBufferOption) {
    auto resourceProvider = target->resourceProvider();

    switch (indexBufferOption) {
        case IndexBufferOption::kPictureFramed: return resourceProvider->refAAQuadIndexBuffer();
        case IndexBufferOption::kIndexedRects:  return resourceProvider->refNonAAQuadIndexBuffer();
        case IndexBufferOption::kTriStrips:     // fall through
        default:                                return nullptr;
    }
}

int QuadLimit(IndexBufferOption option) {
    switch (option) {
        case IndexBufferOption::kPictureFramed: return GrResourceProvider::MaxNumAAQuads();
        case IndexBufferOption::kIndexedRects:  return GrResourceProvider::MaxNumNonAAQuads();
        case IndexBufferOption::kTriStrips:     return SK_MaxS32; // not limited by an indexBuffer
    }

    SkUNREACHABLE;
}

void IssueDraw(const GrCaps& caps, GrOpsRenderPass* renderPass, const VertexSpec& spec,
               int runningQuadCount, int quadsInDraw, int maxVerts, int absVertBufferOffset) {
    if (spec.indexBufferOption() == IndexBufferOption::kTriStrips) {
        int offset = absVertBufferOffset +
                                    runningQuadCount * GrResourceProvider::NumVertsPerNonAAQuad();
        renderPass->draw(4, offset);
        return;
    }

    SkASSERT(spec.indexBufferOption() == IndexBufferOption::kPictureFramed ||
             spec.indexBufferOption() == IndexBufferOption::kIndexedRects);

    int maxNumQuads, numIndicesPerQuad, numVertsPerQuad;

    if (spec.indexBufferOption() == IndexBufferOption::kPictureFramed) {
        // AA uses 8 vertices and 30 indices per quad, basically nested rectangles
        maxNumQuads = GrResourceProvider::MaxNumAAQuads();
        numIndicesPerQuad = GrResourceProvider::NumIndicesPerAAQuad();
        numVertsPerQuad = GrResourceProvider::NumVertsPerAAQuad();
    } else {
        // Non-AA uses 4 vertices and 6 indices per quad
        maxNumQuads = GrResourceProvider::MaxNumNonAAQuads();
        numIndicesPerQuad = GrResourceProvider::NumIndicesPerNonAAQuad();
        numVertsPerQuad = GrResourceProvider::NumVertsPerNonAAQuad();
    }

    SkASSERT(runningQuadCount + quadsInDraw <= maxNumQuads);

    if (caps.avoidLargeIndexBufferDraws()) {
        // When we need to avoid large index buffer draws we modify the base vertex of the draw
        // which, in GL, requires rebinding all vertex attrib arrays, so a base index is generally
        // preferred.
        int offset = absVertBufferOffset + runningQuadCount * numVertsPerQuad;

        renderPass->drawIndexPattern(numIndicesPerQuad, quadsInDraw, maxNumQuads, numVertsPerQuad,
                                     offset);
    } else {
        int baseIndex = runningQuadCount * numIndicesPerQuad;
        int numIndicesToDraw = quadsInDraw * numIndicesPerQuad;

        int minVertex = runningQuadCount * numVertsPerQuad;
        int maxVertex = (runningQuadCount + quadsInDraw) * numVertsPerQuad - 1; // inclusive

        renderPass->drawIndexed(numIndicesToDraw, baseIndex, minVertex, maxVertex,
                                absVertBufferOffset);
    }
}

////////////////// VertexSpec Implementation

int VertexSpec::deviceDimensionality() const {
    return this->deviceQuadType() == GrQuad::Type::kPerspective ? 3 : 2;
}

int VertexSpec::localDimensionality() const {
    return fHasLocalCoords ? (this->localQuadType() == GrQuad::Type::kPerspective ? 3 : 2) : 0;
}

CoverageMode VertexSpec::coverageMode() const {
    if (this->usesCoverageAA()) {
        if (this->compatibleWithCoverageAsAlpha() && this->hasVertexColors() &&
            !this->requiresGeometrySubset()) {
            // Using a geometric subset acts as a second source of coverage and folding
            // the original coverage into color makes it impossible to apply the color's
            // alpha to the geometric subset's coverage when the original shape is clipped.
            return CoverageMode::kWithColor;
        } else {
            return CoverageMode::kWithPosition;
        }
    } else {
        return CoverageMode::kNone;
    }
}

// This needs to stay in sync w/ QuadPerEdgeAAGeometryProcessor::initializeAttrs
size_t VertexSpec::vertexSize() const {
    bool needsPerspective = (this->deviceDimensionality() == 3);
    CoverageMode coverageMode = this->coverageMode();

    size_t count = 0;

    if (coverageMode == CoverageMode::kWithPosition) {
        if (needsPerspective) {
            count += GrVertexAttribTypeSize(kFloat4_GrVertexAttribType);
        } else {
            count += GrVertexAttribTypeSize(kFloat2_GrVertexAttribType) +
                     GrVertexAttribTypeSize(kFloat_GrVertexAttribType);
        }
    } else {
        if (needsPerspective) {
            count += GrVertexAttribTypeSize(kFloat3_GrVertexAttribType);
        } else {
            count += GrVertexAttribTypeSize(kFloat2_GrVertexAttribType);
        }
    }

    if (this->requiresGeometrySubset()) {
        count += GrVertexAttribTypeSize(kFloat4_GrVertexAttribType);
    }

    count += this->hasLocalCoords() ? 9 * sizeof(float) : 0;
    count += this->hasTexture() ? 3 * sizeof(float) : 0;

    if (ColorType::kByte == this->colorType()) {
        count += GrVertexAttribTypeSize(kUByte4_norm_GrVertexAttribType);
    } else if (ColorType::kHalf == this->colorType()) {
        count += GrVertexAttribTypeSize(kHalf4_GrVertexAttribType);
    } else if (ColorType::kFloat == this->colorType()) {
        count += GrVertexAttribTypeSize(kFloat4_GrVertexAttribType);
    }

    if (this->hasSubset()) {
        count += GrVertexAttribTypeSize(kFloat4_GrVertexAttribType);
    }

    return count;
}

////////////////// Geometry Processor Implementation

class QuadPerEdgeAAGeometryProcessor : public GrGeometryProcessor {
public:
    static GrGeometryProcessor* Make(SkArenaAlloc* arena, const VertexSpec& spec) {
        return arena->make([&](void* ptr) {
            return new (ptr) QuadPerEdgeAAGeometryProcessor(spec);
        });
    }

    static GrGeometryProcessor* Make(SkArenaAlloc* arena,
                                     const VertexSpec& vertexSpec,
                                     const GrShaderCaps& caps,
                                     const GrBackendFormat& backendFormat,
                                     GrSamplerState samplerState,
                                     const skgpu::Swizzle& swizzle,
                                     sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                                     Saturate saturate) {
        return arena->make([&](void* ptr) {
            return new (ptr) QuadPerEdgeAAGeometryProcessor(
                    vertexSpec, caps, backendFormat, samplerState, swizzle,
                    std::move(textureColorSpaceXform), saturate);
        });
    }

    const char* name() const override { return "QuadPerEdgeAAGeometryProcessor"; }

    void addToKey(const GrShaderCaps&, KeyBuilder* b) const override {
        // texturing, device-dimensions are single bit flags
        b->addBool(fTexSubset.isInitialized(),    "subset");
        b->addBool(fSampler.isInitialized(),      "textured");
        if (fSampler.isInitialized()) {
            b->addBool(fSampler.samplerState().filter() == GrSamplerState::Filter::kNearest &&
                       fSampler.samplerState().mipmapMode() == GrSamplerState::MipmapMode::kNone,
                       "nearestCells");
        }
        b->addBool(fNeedsPerspective,             "perspective");
        b->addBool((fSaturate == Saturate::kYes), "saturate");

        b->addBool(fLocalMatrix0.isInitialized(), "hasLocalCoords");
        if (fLocalMatrix0.isInitialized()) {
            b->addBits(1, 1, "fragmentLocalCoords");
        }
        b->addBool(fColor.isInitialized(),        "hasColor");
        if (fColor.isInitialized()) {
            // bytes (0) or floats (1)
            b->addBits(2, (fColor.cpuType() == kFloat4_GrVertexAttribType ? 2 :
                           fColor.cpuType() == kHalf4_GrVertexAttribType ? 1 : 0), "colorType");
        }
        // and coverage mode, 00 for none, 01 for withposition, 10 for withcolor, 11 for
        // position+geomsubset
        uint32_t coverageKey = 0;
        SkASSERT(!fGeomSubset.isInitialized() || fCoverageMode == CoverageMode::kWithPosition);
        if (fCoverageMode != CoverageMode::kNone) {
            coverageKey = fGeomSubset.isInitialized()
                                  ? 0x3
                                  : (CoverageMode::kWithPosition == fCoverageMode ? 0x1 : 0x2);
        }
        b->addBits(2, coverageKey, "coverageMode");

        b->add32(GrColorSpaceXform::XformKey(fTextureColorSpaceXform.get()), "colorSpaceXform");
    }

    std::unique_ptr<ProgramImpl> makeProgramImpl(const GrShaderCaps&) const override {
        class Impl : public ProgramImpl {
        public:
            void setData(const GrGLSLProgramDataManager& pdman,
                         const GrShaderCaps&,
                         const GrGeometryProcessor& geomProc) override {
                const auto& gp = geomProc.cast<QuadPerEdgeAAGeometryProcessor>();
                fTextureColorSpaceXformHelper.setData(pdman, gp.fTextureColorSpaceXform.get());
            }

        private:
            void onEmitCode(EmitArgs& args, GrGPArgs* gpArgs) override {
                using Interpolation = GrGLSLVaryingHandler::Interpolation;

                const auto& gp = args.fGeomProc.cast<QuadPerEdgeAAGeometryProcessor>();
                fTextureColorSpaceXformHelper.emitCode(args.fUniformHandler,
                                                       gp.fTextureColorSpaceXform.get());

                args.fVaryingHandler->emitAttributes(gp);

                if (gp.fCoverageMode == CoverageMode::kWithPosition) {
                    // Strip last channel from the vertex attribute to remove coverage and get the
                    // actual position
                    if (gp.fNeedsPerspective) {
                        args.fVertBuilder->codeAppendf("float3 position = %s.xyz;",
                                                       gp.fPosition.name());
                    } else {
                        args.fVertBuilder->codeAppendf("float2 position = %s.xy;",
                                                       gp.fPosition.name());
                    }
                    gpArgs->fPositionVar = {"position",
                                            gp.fNeedsPerspective ? SkSLType::kFloat3
                                                                 : SkSLType::kFloat2,
                                            GrShaderVar::TypeModifier::None};
                } else {
                    // No coverage to eliminate
                    gpArgs->fPositionVar = gp.fPosition.asShaderVar();
                }

                if (gp.fLocalMatrix0.isInitialized()) {
                    const Attribute* rows[] = {&gp.fLocalMatrix0, &gp.fLocalMatrix1, &gp.fLocalMatrix2};
                    const char* names[] = {"localMatrix0", "localMatrix1", "localMatrix2"};
                    for (int i = 0; i < 3; ++i) {
                        args.fFragBuilder->codeAppendf("float3 %s;", names[i]);
                        args.fVaryingHandler->addPassThroughAttribute(rows[i]->asShaderVar(),
                                names[i], Interpolation::kMustBeFlat);
                    }
                    args.fFragBuilder->codeAppend(
                            "float3 localH = float3(dot(localMatrix0, sk_FragCoord.xy1),"
                            "dot(localMatrix1, sk_FragCoord.xy1),"
                            "dot(localMatrix2, sk_FragCoord.xy1));"
                            "float2 localCoord = localH.xy / localH.z;");
                    gpArgs->fLocalCoordVar = GrShaderVar("localCoord", SkSLType::kFloat2);
                    gpArgs->fLocalCoordShader = kFragment_GrShaderType;
                }

                // Solid color before any texturing gets modulated in
                const char* blendDst;
                if (gp.fColor.isInitialized()) {
                    SkASSERT(gp.fCoverageMode != CoverageMode::kWithColor || !gp.fNeedsPerspective);
                    // The color cannot be flat if the varying coverage has been modulated into it
                    args.fFragBuilder->codeAppendf("half4 %s;", args.fOutputColor);
                    args.fVaryingHandler->addPassThroughAttribute(
                            gp.fColor.asShaderVar(),
                            args.fOutputColor,
                            gp.fCoverageMode == CoverageMode::kWithColor
                                    ? Interpolation::kInterpolated
                                    : Interpolation::kCanBeFlat);
                    blendDst = args.fOutputColor;
                } else {
                    // Output color must be initialized to something
                    args.fFragBuilder->codeAppendf("half4 %s = half4(1);", args.fOutputColor);
                    blendDst = nullptr;
                }

                // If there is a texture, must also handle texture coordinates and reading from
                // the texture in the fragment shader before continuing to fragment processors.
                if (gp.fSampler.isInitialized()) {
                    // Texture coordinates clamped by the subset on the fragment shader; if the GP
                    // has a texture, it's guaranteed to have local coordinates
                    args.fFragBuilder->codeAppend("float2 texCoord;");
                    args.fFragBuilder->codeAppend("float3 textureNormalization;");
                    args.fVaryingHandler->addPassThroughAttribute(gp.fTextureNormalization.asShaderVar(),
                            "textureNormalization", Interpolation::kMustBeFlat);
                    args.fFragBuilder->codeAppendf(
                            "texCoord = %s * textureNormalization.xy + float2(0, textureNormalization.z);",
                            gp.fSampler.samplerState().filter() == GrSamplerState::Filter::kNearest &&
                            gp.fSampler.samplerState().mipmapMode() == GrSamplerState::MipmapMode::kNone
                                    ? "(ceil(localCoord) - 0.5)" : "localCoord");

                    // Clamp the now 2D localCoordName variable by the subset if it is provided
                    if (gp.fTexSubset.isInitialized()) {
                        args.fFragBuilder->codeAppend("float4 subset;");
                        args.fVaryingHandler->addPassThroughAttribute(gp.fTexSubset.asShaderVar(),
                                                                      "subset",
                                                                      Interpolation::kCanBeFlat);
                        args.fFragBuilder->codeAppend(
                                "texCoord = clamp(texCoord, subset.LT, subset.RB);");
                    }

                    // Now modulate the starting output color by the texture lookup
                    args.fFragBuilder->codeAppendf(
                            "%s = %s(",
                            args.fOutputColor,
                            (gp.fSaturate == Saturate::kYes) ? "saturate" : "");
                    args.fFragBuilder->appendTextureLookupAndBlend(
                            blendDst, SkBlendMode::kModulate, args.fTexSamplers[0],
                            "texCoord", &fTextureColorSpaceXformHelper);
                    args.fFragBuilder->codeAppend(");");
                } else {
                    // Saturate is only intended for use with a proxy to account for the fact
                    // that TextureOp skips SkPaint conversion, which normally handles this.
                    SkASSERT(gp.fSaturate == Saturate::kNo);
                }

                // And lastly, output the coverage calculation code
                if (gp.fCoverageMode == CoverageMode::kWithPosition) {
                    GrGLSLVarying coverage(SkSLType::kFloat);
                    args.fVaryingHandler->addVarying("coverage", &coverage);
                    if (gp.fNeedsPerspective) {
                        // Multiply by "W" in the vertex shader, then by 1/w (sk_FragCoord.w) in
                        // the fragment shader to get screen-space linear coverage.
                        args.fVertBuilder->codeAppendf("%s = %s.w * %s.z;",
                                                       coverage.vsOut(), gp.fPosition.name(),
                                                       gp.fPosition.name());
                        args.fFragBuilder->codeAppendf("float coverage = %s * sk_FragCoord.w;",
                                                        coverage.fsIn());
                    } else {
                        args.fVertBuilder->codeAppendf("%s = %s;",
                                                       coverage.vsOut(), gp.fCoverage.name());
                        args.fFragBuilder->codeAppendf("float coverage = %s;", coverage.fsIn());
                    }

                    if (gp.fGeomSubset.isInitialized()) {
                        // Calculate distance from sk_FragCoord to the 4 edges of the subset
                        // and clamp them to (0, 1). Use the minimum of these and the original
                        // coverage. This only has to be done in the exterior triangles, the
                        // interior of the quad geometry can never be clipped by the subset box.
                        args.fFragBuilder->codeAppend("float4 geoSubset;");
                        args.fVaryingHandler->addPassThroughAttribute(gp.fGeomSubset.asShaderVar(),
                                                                      "geoSubset",
                                                                      Interpolation::kCanBeFlat);
                        args.fFragBuilder->codeAppend(
                                // This is lifted from GrFragmentProcessor::Rect.
                                "float4 dists4 = saturate(float4(1, 1, -1, -1) * "
                                                         "(sk_FragCoord.xyxy - geoSubset));"
                                "float2 dists2 = dists4.xy + dists4.zw - 1;"
                                "coverage = min(coverage, dists2.x * dists2.y);");
                    }

                    args.fFragBuilder->codeAppendf("half4 %s = half4(coverage);",
                                                   args.fOutputCoverage);
                } else {
                    // Set coverage to 1, since it's either non-AA or the coverage was already
                    // folded into the output color
                    SkASSERT(!gp.fGeomSubset.isInitialized());
                    args.fFragBuilder->codeAppendf("const half4 %s = half4(1);",
                                                   args.fOutputCoverage);
                }
            }

            GrGLSLColorSpaceXformHelper fTextureColorSpaceXformHelper;
        };

        return std::make_unique<Impl>();
    }

private:
    using Saturate = skgpu::ganesh::TextureOp::Saturate;

    QuadPerEdgeAAGeometryProcessor(const VertexSpec& spec)
            : INHERITED(kQuadPerEdgeAAGeometryProcessor_ClassID)
            , fTextureColorSpaceXform(nullptr) {
        SkASSERT(!spec.hasSubset());
        this->initializeAttrs(spec);
        this->setTextureSamplerCnt(0);
    }

    QuadPerEdgeAAGeometryProcessor(const VertexSpec& spec,
                                   const GrShaderCaps& caps,
                                   const GrBackendFormat& backendFormat,
                                   GrSamplerState samplerState,
                                   const skgpu::Swizzle& swizzle,
                                   sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                                   Saturate saturate)
            : INHERITED(kQuadPerEdgeAAGeometryProcessor_ClassID)
            , fSaturate(saturate)
            , fTextureColorSpaceXform(std::move(textureColorSpaceXform))
            , fSampler(samplerState, backendFormat, swizzle) {
        SkASSERT(spec.hasLocalCoords());
        this->initializeAttrs(spec);
        this->setTextureSamplerCnt(1);
    }

    // This needs to stay in sync w/ VertexSpec::vertexSize
    void initializeAttrs(const VertexSpec& spec) {
        fNeedsPerspective = spec.deviceDimensionality() == 3;
        fCoverageMode = spec.coverageMode();

        if (fCoverageMode == CoverageMode::kWithPosition) {
            if (fNeedsPerspective) {
                fPosition = {"positionWithCoverage", kFloat4_GrVertexAttribType, SkSLType::kFloat4};
            } else {
                fPosition = {"position", kFloat2_GrVertexAttribType, SkSLType::kFloat2};
                fCoverage = {"coverage", kFloat_GrVertexAttribType, SkSLType::kFloat};
            }
        } else {
            if (fNeedsPerspective) {
                fPosition = {"position", kFloat3_GrVertexAttribType, SkSLType::kFloat3};
            } else {
                fPosition = {"position", kFloat2_GrVertexAttribType, SkSLType::kFloat2};
            }
        }

        // Need a geometry subset when the quads are AA and not rectilinear, since their AA
        // outsetting can go beyond a half pixel.
        if (spec.requiresGeometrySubset()) {
            fGeomSubset = {"geomSubset", kFloat4_GrVertexAttribType, SkSLType::kFloat4};
        }

        if (spec.hasLocalCoords()) {
            fLocalMatrix0 = {"localMatrix0", kFloat3_GrVertexAttribType, SkSLType::kFloat3};
            fLocalMatrix1 = {"localMatrix1", kFloat3_GrVertexAttribType, SkSLType::kFloat3};
            fLocalMatrix2 = {"localMatrix2", kFloat3_GrVertexAttribType, SkSLType::kFloat3};
        }

        if (spec.hasTexture()) {
            fTextureNormalization = {"textureNormalization", kFloat3_GrVertexAttribType, SkSLType::kFloat3};
        }

        if (spec.hasVertexColors()) {
            fColor = MakeColorAttribute("color", spec.colorStorage());
        }

        if (spec.hasSubset()) {
            fTexSubset = {"texSubset", kFloat4_GrVertexAttribType, SkSLType::kFloat4};
        }

        this->setVertexAttributesWithImplicitOffsets(&fPosition, 9);
    }

    const TextureSampler& onTextureSampler(int) const override { return fSampler; }

    Attribute fPosition; // May contain coverage as last channel
    Attribute fCoverage; // Used for non-perspective position to avoid Intel Metal issues
    Attribute fColor; // May have coverage modulated in if the FPs support it
    Attribute fLocalMatrix0;
    Attribute fLocalMatrix1;
    Attribute fLocalMatrix2;
    Attribute fTextureNormalization;
    Attribute fGeomSubset; // Screen-space bounding box on geometry+aa outset
    Attribute fTexSubset; // Texture-space bounding box on local coords

    // The positions attribute may have coverage built into it, so float3 is an ambiguous type
    // and may mean 2d with coverage, or 3d with no coverage
    bool fNeedsPerspective;
    // Should saturate() be called on the color? Only relevant when created with a texture.
    Saturate fSaturate = Saturate::kNo;
    CoverageMode fCoverageMode;

    // Color space will be null and fSampler.isInitialized() returns false when the GP is configured
    // to skip texturing.
    sk_sp<GrColorSpaceXform> fTextureColorSpaceXform;
    TextureSampler fSampler;

    using INHERITED = GrGeometryProcessor;
};

GrGeometryProcessor* MakeProcessor(SkArenaAlloc* arena, const VertexSpec& spec) {
    return QuadPerEdgeAAGeometryProcessor::Make(arena, spec);
}

GrGeometryProcessor* MakeTexturedProcessor(SkArenaAlloc* arena,
                                           const VertexSpec& spec,
                                           const GrShaderCaps& caps,
                                           const GrBackendFormat& backendFormat,
                                           GrSamplerState samplerState,
                                           const skgpu::Swizzle& swizzle,
                                           sk_sp<GrColorSpaceXform> textureColorSpaceXform,
                                           Saturate saturate) {
    return QuadPerEdgeAAGeometryProcessor::Make(arena, spec, caps, backendFormat, samplerState,
                                                swizzle, std::move(textureColorSpaceXform),
                                                saturate);
}

}  // namespace skgpu::ganesh::QuadPerEdgeAA
