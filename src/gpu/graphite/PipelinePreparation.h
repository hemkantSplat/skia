/*
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef skgpu_graphite_PipelinePreparation_DEFINED
#define skgpu_graphite_PipelinePreparation_DEFINED

#include "include/core/SkImage.h"
#include "include/gpu/graphite/TextureInfo.h"
#include "src/core/SkStrike.h"
#include "src/gpu/graphite/GraphicsPipelineDesc.h"
#include "src/text/gpu/GlyphVector.h"
#include "src/gpu/graphite/RenderPassDesc.h"
#include "src/gpu/graphite/RuntimeEffectDictionary.h"

#include <string>
#include <vector>

namespace skgpu::graphite {

class ResourceProvider;

struct PipelineRequest {
    GraphicsPipelineDesc fGraphics;
    RenderPassDesc fRenderPass;
    sk_sp<const RuntimeEffectDictionary> fEffects;
};

struct GlyphRunRequest {
    sk_sp<SkStrike> fStrike;
    sktext::gpu::RendererData fRenderData;
    std::vector<SkPackedGlyphID> fIDs;
};

struct BitmapRequest {
    sk_sp<const SkImage> fImage;
    SkImage::RequiredProperties fProperties;
};

enum class PreparationTarget { kScratch, kDestinationCopy, kAttachment };

struct TargetRequest {
    SkISize fSize;
    TextureInfo fInfo;
    std::string fLabel;
    PreparationTarget fKind;
};

struct PipelineDiscovery {
    ResourceProvider* fProvider;
    SkISize fTargetSize;
    std::vector<PipelineRequest>& fRequests;
    std::vector<GlyphRunRequest>* fGlyphs;
    std::vector<BitmapRequest>* fBitmaps;
    std::vector<TargetRequest>* fTargets;
};

// Only set while snapping a private recorder whose commands will be discarded.
inline thread_local PipelineDiscovery* gPipelineDiscovery = nullptr;
inline thread_local ResourceProvider* gPrepareBitmapUploads = nullptr;

}  // namespace skgpu::graphite

#endif
