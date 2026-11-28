/*
 * Copyright 2025 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/graphite/SerializationUtils.h"

#include "include/core/SkColorSpace.h"
#include "include/core/SkFourByteTag.h"
#include "include/core/SkMesh.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/core/SkStream.h"
#include "include/private/SkAttributes.h"
#include "src/core/SkKnownRuntimeEffects.h"
#include "src/core/SkMeshPriv.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/gpu/SwizzlePriv.h"
#include "src/gpu/graphite/Caps.h"
#include "src/gpu/graphite/GraphicsPipelineDesc.h"
#include "src/gpu/graphite/PaintParamsKey.h"
#include "src/gpu/graphite/RenderPassDesc.h"
#include "src/gpu/graphite/Renderer.h"
#include "src/gpu/graphite/RuntimeEffectDictionary.h"
#include "src/gpu/graphite/ShaderCodeDictionary.h"
#include "src/sksl/SkSLProgramSettings.h"
#include "src/sksl/ir/SkSLProgram.h"

#include <vector>

namespace skgpu::graphite {

// This is the main control to version the serialized Pipelines (c.f. stream_is_blob)
static constexpr uint32_t kCurrent_Version = 4;

namespace {

static const char kMagic[] = { 's', 'k', 'i', 'a', 'p', 'i', 'p', 'e' };

constexpr bool is_valid_samplecount(uint32_t sampleCount) {
    return SkIsPow2(sampleCount) && sampleCount >= 1 && sampleCount <= 16;
}

[[nodiscard]] bool stream_is_pipeline(SkStream* stream, uint32_t* version) {
    char magic[8];
    static_assert(sizeof(kMagic) == sizeof(magic), "");

    if (stream->read(magic, sizeof(kMagic)) != sizeof(kMagic)) {
        return false;
    }

    if (0 != SK_UNSAFE_TODO(memcmp(magic, kMagic, sizeof(kMagic)))) {
        return false;
    }

    return stream->readU32(version) && (*version == 3 || *version == kCurrent_Version);
}

constexpr uint32_t kMaxShaderBytes = 1 << 20;
constexpr uint32_t kMaxKeyWords = 1 << 16;
constexpr int kMaxKeyDepth = 128;
constexpr int32_t kInlineEffect = -1;
constexpr int32_t kInlineMesh = -2;

bool stable_snippet(const ShaderCodeDictionary* dict, int32_t id) {
    return id >= 0 && (id < kBuiltInCodeSnippetIDCount ||
                      SkKnownRuntimeEffects::IsSkiaKnownRuntimeEffect(id) ||
                      dict->isUserDefinedKnownRuntimeEffect(id));
}

bool write_bytes(SkWStream* stream, const void* data, size_t size) {
    return size <= kMaxShaderBytes && stream->write32(SkToU32(size)) &&
           (!size || stream->write(data, size));
}

bool write_string(SkWStream* stream, std::string_view value) {
    return write_bytes(stream, value.data(), value.size());
}

sk_sp<SkData> read_bytes(SkStream* stream) {
    uint32_t size;
    if (!stream->readU32(&size) || size > kMaxShaderBytes ||
        size > stream->getLength() - stream->getPosition()) {
        return nullptr;
    }
    auto data = SkData::MakeUninitialized(size);
    if (size && stream->read(data->writable_data(), size) != size) return nullptr;
    return data;
}

bool read_string(SkStream* stream, SkString* value) {
    auto data = read_bytes(stream);
    if (!data || (data->size() && memchr(data->data(), 0, data->size()))) return false;
    if (data->size()) value->set(static_cast<const char*>(data->data()), data->size());
    return true;
}

bool write_mesh(SkWStream* stream, const SkMeshSpecification& mesh) {
    using Priv = SkMeshSpecificationPriv;
    // SkMeshSpecification::Make prefixes these declarations. Save the authored
    // bodies and let Make regenerate and validate the declarations on import.
    auto body = [](const SkSL::Program* program, int structs) -> std::string_view {
        std::string_view source = *program->fSource;
        while (structs--) {
            auto end = source.find("};\n");
            if (end == std::string_view::npos) return {};
            source.remove_prefix(end + 3);
        }
        return source;
    };
    auto vs = body(Priv::VS(mesh), 2);
    auto fs = body(Priv::FS(mesh), 1);
    if (vs.empty() || fs.empty() || !write_string(stream, vs) || !write_string(stream, fs) ||
        !stream->write32(SkToU32(mesh.stride())) || !stream->write32(SkToU32(mesh.instanceStride())) ||
        !stream->write32(Priv::AlphaType(mesh))) {
        return false;
    }
    auto cs = mesh.colorSpace() ? mesh.colorSpace()->serialize() : nullptr;
    if (!write_bytes(stream, cs ? cs->data() : nullptr, cs ? cs->size() : 0) ||
        !stream->write32(SkToU32(mesh.attributes().size()))) return false;
    for (const auto& a : mesh.attributes()) {
        if (!stream->write32(static_cast<uint32_t>(a.type)) ||
            !stream->write32(static_cast<uint32_t>(a.rate)) ||
            !stream->write32(SkToU32(a.offset)) ||
            !write_string(stream, {a.name.c_str(), a.name.size()})) return false;
    }
    auto varyings = Priv::Varyings(mesh);
    if (!stream->write32(SkToU32(varyings.size()))) return false;
    for (const auto& v : varyings) {
        if (!stream->write32(static_cast<uint32_t>(v.type)) ||
            !write_string(stream, {v.name.c_str(), v.name.size()})) return false;
    }
    return true;
}

sk_sp<SkMeshSpecification> read_mesh(SkStream* stream) {
    using Mesh = SkMeshSpecification;
    using Attr = Mesh::Attribute;
    using Varying = Mesh::Varying;
    SkString vs, fs;
    uint32_t stride, instanceStride, alpha, count;
    if (!read_string(stream, &vs) || !read_string(stream, &fs) ||
        !stream->readU32(&stride) || stride > Mesh::kMaxStride ||
        !stream->readU32(&instanceStride) || instanceStride > Mesh::kMaxStride ||
        !stream->readU32(&alpha) || alpha > kLastEnum_SkAlphaType) return nullptr;
    auto bytes = read_bytes(stream);
    if (!bytes) return nullptr;
    auto cs = bytes->size() ? SkColorSpace::Deserialize(bytes->data(), bytes->size()) : nullptr;
    if (bytes->size() && !cs) return nullptr;

    if (!stream->readU32(&count) || count > Mesh::kMaxAttributes) return nullptr;
    std::vector<Attr> attributes(count);
    for (auto& a : attributes) {
        uint32_t type, rate, offset;
        if (!stream->readU32(&type) || type > static_cast<uint32_t>(Attr::Type::kLast) ||
            !stream->readU32(&rate) || rate > static_cast<uint32_t>(Attr::Rate::kInstance) ||
            !stream->readU32(&offset) || offset > Mesh::kMaxStride ||
            !read_string(stream, &a.name)) return nullptr;
        a.type = static_cast<Attr::Type>(type);
        a.rate = static_cast<Attr::Rate>(rate);
        a.offset = offset;
    }
    if (!stream->readU32(&count) || count > Mesh::kMaxVaryings) return nullptr;
    std::vector<Varying> varyings(count);
    for (auto& v : varyings) {
        uint32_t type;
        if (!stream->readU32(&type) || type > static_cast<uint32_t>(Varying::Type::kLast) ||
            !read_string(stream, &v.name)) return nullptr;
        v.type = static_cast<Varying::Type>(type);
    }
    return Mesh::Make(SkSpan(attributes), stride, SkSpan(varyings), vs, fs,
                      std::move(cs), static_cast<SkAlphaType>(alpha), instanceStride).specification;
}

bool write_snippet(SkWStream* stream, const ShaderCodeDictionary* dict,
                   const RuntimeEffectDictionary* effects, int32_t id) {
    if (stable_snippet(dict, id)) return stream->write32(id);
    if (effects) {
        if (auto effect = effects->find(id)) {
            uint8_t kind = effect->allowShader() ? 0 : effect->allowColorFilter() ? 1 : 2;
            const auto& config = *SkRuntimeEffectPriv::Program(*effect).fConfig;
            using Kind = SkSL::ProgramKind;
            bool privateAccess = config.fKind == Kind::kPrivateRuntimeShader ||
                                 config.fKind == Kind::kPrivateRuntimeColorFilter ||
                                 config.fKind == Kind::kPrivateRuntimeBlender;
            uint8_t flags = (config.fSettings.fMaxVersionAllowed == SkSL::Version::k300 ? 1 : 0) |
                            (config.fSettings.fForceNoInline ? 2 : 0) | (privateAccess ? 4 : 0);
            return stream->write32(kInlineEffect) && stream->write8(kind) &&
                   stream->write8(flags) && write_string(stream, effect->source());
        }
        if (auto mesh = effects->findMeshSpec(id)) {
            return stream->write32(kInlineMesh) && write_mesh(stream, *mesh);
        }
    }
    return false;
}

int32_t read_snippet(SkStream* stream, ShaderCodeDictionary* dict,
                     RuntimeEffectDictionary* effects, uint32_t version, int32_t id) {
    if (stable_snippet(dict, id)) return id;
    if (version < 4) return -1;
    if (id == kInlineEffect) {
        uint8_t kind, flags;
        SkString source;
        if (!stream->readU8(&kind) || kind > 2 || !stream->readU8(&flags) || flags > 7 ||
            !read_string(stream, &source)) return -1;
        auto options = (flags & 1) ? SkRuntimeEffectPriv::ES3Options() : SkRuntimeEffect::Options{};
        options.forceUnoptimized = (flags & 2) != 0;
        if (flags & 4) SkRuntimeEffectPriv::AllowPrivateAccess(&options);
        auto result = kind == 0 ? SkRuntimeEffect::MakeForShader(std::move(source), options) :
                      kind == 1 ? SkRuntimeEffect::MakeForColorFilter(std::move(source), options) :
                                  SkRuntimeEffect::MakeForBlender(std::move(source), options);
        if (!result.effect) return -1;
        int snippet = dict->findOrCreateRuntimeEffectSnippet(result.effect.get());
        if (snippet >= 0) effects->set(snippet, std::move(result.effect));
        return snippet;
    }
    if (id == kInlineMesh) {
        auto mesh = read_mesh(stream);
        if (!mesh) return -1;
        int snippet = dict->findOrCreateMeshSnippet(mesh.get());
        effects->set(snippet, std::move(mesh));
        return snippet;
    }
    return -1;
}

// Snippet IDs appear alongside sampler words whose values may resemble IDs. Walk the
// structural tree and only rewrite the ID at the beginning of each block.
template <typename Word, typename Snippet>
bool walk_block(const ShaderCodeDictionary* dict, Word& word, Snippet& snippet,
                int depth, bool* external) {
    int32_t id;
    if (!depth || !snippet(&id)) return false;
    auto entry = dict->getEntry(id);
    if (!entry) return false;

    if (entry->storesSamplerDescData()) {
        int32_t encoded;
        if (!word(&encoded) || encoded >= 0 ||
            encoded < PaintParamsKey::EncodeDataSize(PaintParamsKey::kEmbeddedDataSizeLimit)) {
            return false;
        }
        int words = PaintParamsKey::EncodeDataSize(encoded);
        if (external && words == 3) *external = true;
        for (int i = 0; i < words; ++i) {
            int32_t payload;
            if (!word(&payload)) return false;
        }
    }
    for (int i = 0; i < entry->fNumChildren; ++i) {
        if (!walk_block(dict, word, snippet, depth - 1, external)) return false;
    }
    return true;
}

bool serialize_graphics_pipeline_desc(ShaderCodeDictionary* dict,
                                      const RuntimeEffectDictionary* effects,
                                      SkWStream* stream,
                                      const GraphicsPipelineDesc& pipelineDesc) {
    auto words = dict->lookup(pipelineDesc.paintParamsID()).data();
    if (words.size() > kMaxKeyWords ||
        !stream->write32(static_cast<uint32_t>(pipelineDesc.renderStepID())) ||
        !stream->write32(SkToU32(words.size()))) return false;

    size_t next = 0;
    auto word = [&](int32_t* value) {
        if (next == words.size()) return false;
        *value = words[next++];
        return stream->write32(*value);
    };
    auto snippet = [&](int32_t* id) {
        if (next == words.size()) return false;
        *id = words[next++];
        return write_snippet(stream, dict, effects, *id);
    };
    while (next < words.size()) {
        int32_t header;
        if (!word(&header) || header >= 0 ||
            !walk_block(dict, word, snippet, kMaxKeyDepth, nullptr)) return false;
    }
    return true;
}

bool deserialize_graphics_pipeline_desc(ShaderCodeDictionary* dict,
                                        RuntimeEffectDictionary* effects,
                                        SkStream* stream,
                                        uint32_t version,
                                        GraphicsPipelineDesc* desc,
                                        bool* external) {
    uint32_t step, wordCount;
    if (!stream->readU32(&step) || step >= RenderStep::kNumRenderSteps ||
        !stream->readU32(&wordCount) || wordCount > kMaxKeyWords ||
        wordCount > (stream->getLength() - stream->getPosition()) / 4) return false;

    std::vector<int32_t> words;
    words.reserve(wordCount);
    auto word = [&](int32_t* value) {
        if (words.size() == wordCount || !stream->readS32(value)) return false;
        words.push_back(*value);
        return true;
    };
    auto snippet = [&](int32_t* id) {
        if (!word(id)) return false;
        *id = read_snippet(stream, dict, effects, version, *id);
        words.back() = *id;
        return *id >= 0;
    };
    while (words.size() < wordCount) {
        int32_t header;
        if (!word(&header) || header >= 0 ||
            !walk_block(dict, word, snippet, kMaxKeyDepth, external)) return false;
    }
    auto paint = words.empty() ? UniquePaintParamsID::Invalid() :
                                 dict->findOrCreate(PaintParamsKey(SkSpan(words)));
    *desc = GraphicsPipelineDesc(static_cast<RenderStep::RenderStepID>(step), paint);
    return true;
}

[[nodiscard]] bool serialize_attachment_desc(SkWStream* stream,
                                             const AttachmentDesc& attachmentDesc) {
    uint32_t tag = attachmentDesc.fFormat == TextureFormat::kUnsupported
            ? SkSetFourByteTag(static_cast<uint8_t>(TextureFormat::kUnsupported), 0, 0, 1)
            : SkSetFourByteTag(static_cast<uint8_t>(attachmentDesc.fFormat),
                               static_cast<uint8_t>(attachmentDesc.fLoadOp),
                               static_cast<uint8_t>(attachmentDesc.fStoreOp),
                               static_cast<uint8_t>(attachmentDesc.fSampleCount));
    return stream->write32(tag);
}

[[nodiscard]] bool deserialize_attachment_desc(SkStream* stream,
                                               AttachmentDesc* attachmentDesc) {
    uint32_t tag;
    if (!stream->readU32(&tag)) {
        return false;
    }

    uint8_t format      = static_cast<uint8_t>((tag >> 24) & 0xFF);
    uint8_t loadOp      = static_cast<uint8_t>((tag >> 16) & 0xFF);
    uint8_t storeOp     = static_cast<uint8_t>((tag >>  8) & 0xFF);
    uint8_t sampleCount = static_cast<uint8_t>((tag >>  0) & 0xFF);

    if (format >= kTextureFormatCount) {
        return false;
    }
    if (loadOp >= kLoadOpCount) {
        return false;
    }
    if (storeOp >= kStoreOpCount) {
        return false;
    }
    if (!is_valid_samplecount(sampleCount)) {
        return false;
    }

    *attachmentDesc = {static_cast<TextureFormat>(format),
                       static_cast<LoadOp>(loadOp),
                       static_cast<StoreOp>(storeOp),
                       static_cast<SampleCount>(sampleCount)};

    return true;
}

[[nodiscard]] bool serialize_render_pass_desc(SkWStream* stream,
                                              const RenderPassDesc& renderPassDesc) {
    if (!serialize_attachment_desc(stream, renderPassDesc.fColorAttachment)) {
        return false;
    }
    if (!serialize_attachment_desc(stream, renderPassDesc.fColorResolveAttachment)) {
        return false;
    }
    if (!serialize_attachment_desc(stream, renderPassDesc.fDepthStencilAttachment)) {
        return false;
    }

    if (!stream->write16(renderPassDesc.fWriteSwizzle.asKey())) {
        return false;
    }
    if (!stream->write8(static_cast<uint8_t>(renderPassDesc.fSampleCount))) {
        return false;
    }

    // Omit clear values for the various attachments as they do not effect structure.
    // Omit fDstReadStrategy from the serialization because it is not a part of RenderPassDesc
    // keys and does not impact pipeline creation. When deserializing, the strategy can be
    // obtained via caps->getDstReadStrategy().

    return true;
}

[[nodiscard]] bool deserialize_render_pass_desc(const Caps* caps,
                                                SkStream* stream,
                                                RenderPassDesc* renderPassDesc) {
    if (!deserialize_attachment_desc(stream, &renderPassDesc->fColorAttachment)) {
        return false;
    }
    if (!deserialize_attachment_desc(stream, &renderPassDesc->fColorResolveAttachment)) {
        return false;
    }
    if (!deserialize_attachment_desc(stream, &renderPassDesc->fDepthStencilAttachment)) {
        return false;
    }

    uint16_t swizzle;
    if (!stream->readU16(&swizzle)) {
        return false;
    }
    renderPassDesc->fWriteSwizzle = SwizzleCtorAccessor::Make(swizzle);

    uint8_t sampleCount;
    if (!stream->readU8(&sampleCount) || !is_valid_samplecount(sampleCount)) {
        return false;
    }
    renderPassDesc->fSampleCount = static_cast<SampleCount>(sampleCount);

    // RenderPassDesc dst read strategy is not serialized as it is not something we key on and does
    // not impact pipeline creation. When deserializing, simply query Caps again for a
    // DstReadStrategy. Leave clear color/depth/stencil as their default values.
    renderPassDesc->fDstReadStrategy = caps->getDstReadStrategy();

    return true;
}

#define SK_BLOB_END_TAG SkSetFourByteTag('e', 'n', 'd', ' ')

bool deserialize_pipeline(const Caps* caps, ShaderCodeDictionary* dict,
                          RuntimeEffectDictionary* effects, SkStream* stream,
                          GraphicsPipelineDesc* pipeline, RenderPassDesc* renderPass,
                          bool* external = nullptr) {
    uint32_t version, tag;
    if (!stream_is_pipeline(stream, &version) ||
        !deserialize_graphics_pipeline_desc(dict, effects, stream, version, pipeline, external) ||
        !deserialize_render_pass_desc(caps, stream, renderPass) ||
        !stream->readU32(&tag) || tag != SK_BLOB_END_TAG || !stream->isAtEnd()) {
        return false;
    }
    if (external) {
        *external |= renderPass->fColorAttachment.fFormat == TextureFormat::kExternal ||
                     renderPass->fColorResolveAttachment.fFormat == TextureFormat::kExternal ||
                     renderPass->fDepthStencilAttachment.fFormat == TextureFormat::kExternal;
    }
    return true;
}

} // anonymous namespace

sk_sp<SkData> PipelineDescToData(const Caps*,
                                ShaderCodeDictionary* dict,
                                const RuntimeEffectDictionary* effects,
                                const GraphicsPipelineDesc& pipeline,
                                const RenderPassDesc& renderPass) {
    SkDynamicMemoryWStream stream;
    if (!stream.write(kMagic, sizeof(kMagic)) || !stream.write32(kCurrent_Version) ||
        !serialize_graphics_pipeline_desc(dict, effects, &stream, pipeline) ||
        !serialize_render_pass_desc(&stream, renderPass) || !stream.write32(SK_BLOB_END_TAG)) {
        return nullptr;
    }
    return stream.detachAsData();
}

bool DataToPipelineDesc(const Caps* caps, ShaderCodeDictionary* dict,
                        RuntimeEffectDictionary* effects, const SkData* data,
                        GraphicsPipelineDesc* pipeline, RenderPassDesc* renderPass) {
    if (!data) return false;
    SkMemoryStream stream(data->data(), data->size());
    return deserialize_pipeline(caps, dict, effects, &stream, pipeline, renderPass);
}

bool DataContainsExternalFormat(const Caps* caps, ShaderCodeDictionary* dict,
                                const SkData* data, bool* external) {
    if (!data) return false;
    SkMemoryStream stream(data->data(), data->size());
    auto effects = sk_make_sp<RuntimeEffectDictionary>();
    GraphicsPipelineDesc pipeline;
    RenderPassDesc renderPass;
    *external = false;
    return deserialize_pipeline(caps, dict, effects.get(), &stream, &pipeline, &renderPass, external);
}

#if defined(GPU_TEST_UTILS)
void DumpPipelineDesc(const Caps* caps,
                      const char* label,
                      ShaderCodeDictionary* shaderCodeDictionary,
                      const GraphicsPipelineDesc& pipelineDesc,
                      const RenderPassDesc& renderPassDesc) {
    SkString pipelineStr = pipelineDesc.toString(caps, shaderCodeDictionary);
    SkString renderPassStr = renderPassDesc.toPipelineLabel();
    SkDebugf("%s: %s - %s\n", label, pipelineStr.c_str(), renderPassStr.c_str());
}

bool ComparePipelineDescs(const GraphicsPipelineDesc& a1, const RenderPassDesc& b1,
                          const GraphicsPipelineDesc& a2, const RenderPassDesc& b2) {
    return (a1 == a2) && (b1 == b2);
}
#endif

} // namespace skgpu::graphite
