/* Evaluate the compiled mesh stages with runtime effects; SkDraw owns triangle coverage and clipping. */
#include "src/core/SkMeshRasterPriv.h"
#include "src/core/SkMeshPriv.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/core/SkBitmapDevice.h"
#include "src/core/SkVertState.h"
#include "src/effects/colorfilters/SkColorSpaceXformColorFilter.h"
#include "src/sksl/ir/SkSLProgram.h"
#include "include/core/SkBlender.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkShader.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkSurface.h"
#include "include/core/SkVertices.h"
#include <cstring>

namespace {
int components(SkMeshSpecification::Varying::Type type) { return (static_cast<int>(type) % 4) + 1; }
std::string stage_source(const SkSL::Program& program, const std::string& prefix) {
    std::string source = program.description();
    size_t pos = 0;
    while ((pos = source.find(" main(", pos)) != std::string::npos) {
        source.replace(pos + 1, 4, prefix + "stage");
        pos += prefix.size() + 7;
    }
    const std::string color_layout = "layout (color) ";
    while ((pos = source.find(color_layout)) != std::string::npos) source.erase(pos, color_layout.size());
    return source;
}
void bind_mesh(SkRuntimeShaderBuilder& builder, const SkMesh& mesh) {
    auto uniforms = SkRuntimeEffectPriv::TransformUniforms(
        mesh.spec()->uniforms(), mesh.refUniforms(), mesh.spec()->colorSpace());
    for (const auto& u : builder.effect()->uniforms()) {
        if (const auto* source = mesh.spec()->findUniform(u.name)) {
            builder.uniform(u.name).set(uniforms->bytes() + source->offset,
                                       static_cast<int>(source->sizeInBytes()));
        }
    }
    for (const auto& c : builder.effect()->children()) {
        if (const auto* source = mesh.spec()->findChild(c.name)) {
            const auto& child = mesh.children()[source->index];
            switch (source->type) {
                case SkRuntimeEffect::ChildType::kShader: builder.child(c.name) = sk_ref_sp(child.shader()); break;
                case SkRuntimeEffect::ChildType::kColorFilter: builder.child(c.name) = sk_ref_sp(child.colorFilter()); break;
                case SkRuntimeEffect::ChildType::kBlender: builder.child(c.name) = sk_ref_sp(child.blender()); break;
            }
        }
    }
}
}

const SkMeshRasterProgram& SkMeshRasterPriv::Get(const SkMeshSpecification& spec) {
    spec.fRasterOnce([&] { spec.fRasterProgram = Make(spec); });
    return *spec.fRasterProgram;
}

std::unique_ptr<const SkMeshRasterProgram> SkMeshRasterPriv::Make(const SkMeshSpecification& spec) {
    auto result = std::make_unique<SkMeshRasterProgram>();
    const auto& vs = *SkMeshSpecificationPriv::VS(spec);
    const auto& fs = *SkMeshSpecificationPriv::FS(spec);
    const std::string originals = vs.description() + fs.description();
    for (int suffix = 0;; ++suffix) {
        result->prefix = "cfMesh" + std::to_string(suffix) + "_";
        if (originals.find(result->prefix) == std::string::npos) break;
    }
    const auto& p = result->prefix;
    const auto varyings = SkMeshSpecificationPriv::Varyings(spec);
    for (const auto& v : varyings) result->slots += components(v.type);
    const int groups = (result->slots + 3) / 4;
    std::string source = stage_source(vs, p);
    source += "uniform float4 " + p + "attrs[" + std::to_string(spec.attributes().size()) + "];\n";
    source += "float4 main(float2 xy) { Attributes a;\n";
    size_t attribute = 0;
    for (const auto& a : spec.attributes()) {
        const int count = static_cast<int>(a.type) == 4 ? 4 : static_cast<int>(a.type) + 1;
        source += "a." + std::string(a.name.c_str()) + "=" + p + "attrs[" + std::to_string(attribute++) + "].";
        source += std::string("xyzw", count) + ";\n";
    }
    source += "Varyings v=" + p + "stage(a); float " + p + "slots[" + std::to_string(groups * 4) + "];\n";
    int slot = 0;
    for (const auto& v : varyings) {
        for (int c = 0; c < components(v.type); ++c) {
            source += p + "slots[" + std::to_string(slot++) + "]=float(v." + v.name.c_str();
            if (components(v.type) > 1) source += std::string(".") + "xyzw"[c];
            source += ");\n";
        }
    }
    while (slot < groups * 4) source += p + "slots[" + std::to_string(slot++) + "]=0;\n";
    for (int group = 0; group < groups; ++group) {
        source += "if (xy.x<" + std::to_string(group + 1) + ") return float4(";
        for (int c = 0; c < 4; ++c) {
            if (c) source += ",";
            source += p + "slots[" + std::to_string(group * 4 + c) + "]";
        }
        source += ");\n";
    }
    source += "return float4(0); }";
    auto vertex = SkRuntimeEffect::MakeForShader(SkString(source));
    result->vertex = vertex.effect;
    if (!vertex.effect) { result->error = "raster mesh VS: " + std::string(vertex.errorText.c_str()); return result; }

    source = stage_source(fs, p);
    source += "uniform float3x3 " + p + "bary; uniform float4 " + p + "values[" + std::to_string(groups * 3) + "];\n";
    source += "uniform shader " + p + "paint;\n";
    const bool colors = SkMeshSpecificationPriv::HasColors(spec);
    if (colors) source += "uniform blender " + p + "blend; uniform colorFilter " + p + "xform;\n";
    source += "float4 main(float2 xy) { float3 b=" + p + "bary*float3(xy,1); b.z=1-b.x-b.y; Varyings v;\n";
    slot = 0;
    const char* types[] = {"float", "float2", "float3", "float4", "half", "half2", "half3", "half4"};
    for (const auto& v : varyings) {
        source += "v." + std::string(v.name.c_str()) + "=" + types[static_cast<int>(v.type)] + "(";
        for (int c = 0; c < components(v.type); ++c, ++slot) {
            if (c) source += ",";
            source += "(";
            for (int vertex_index = 0; vertex_index < 3; ++vertex_index) {
                if (vertex_index) source += "+";
                source += p + "values[" + std::to_string(vertex_index * groups + slot / 4) + "].";
                source += "xyzw"[slot % 4];
                source += std::string("*b.") + "xyz"[vertex_index];
            }
            source += ")";
        }
        source += ");\n";
    }
    if (colors) {
        const bool half = SkMeshSpecificationPriv::GetColorType(spec) == SkMeshSpecificationPriv::ColorType::kHalf4;
        source += std::string(half ? "half4" : "float4") + " color; float2 local=" + p + "stage(v,color);\n";
        if (SkMeshSpecificationPriv::AlphaType(spec) == kOpaque_SkAlphaType) source += "color.a=1;\n";
        if (SkMeshSpecificationPriv::AlphaType(spec) == kUnpremul_SkAlphaType) source += "color.rgb*=color.a;\n";
        source += "return " + p + "blend.eval(" + p + "paint.eval(local)," + p + "xform.eval(half4(color))); }";
    } else {
        source += "return " + p + "paint.eval(" + p + "stage(v)); }";
    }
    auto fragment = SkRuntimeEffect::MakeForShader(SkString(source));
    result->fragment = fragment.effect;
    if (!fragment.effect) result->error = "raster mesh FS: " + std::string(fragment.errorText.c_str());
    return result;
}

void SkMeshRasterPriv::Draw(SkBitmapDevice* device, const SkMesh& mesh,
                            sk_sp<SkBlender> blender, const SkPaint& paint) {
    const auto& program = Get(*mesh.spec());
    if (!program.error.empty()) return;
    const int groups = (program.slots + 3) / 4;
    const auto& p = program.prefix;
    SkRuntimeShaderBuilder vertex(program.vertex), fragment(program.fragment);
    bind_mesh(vertex, mesh);
    bind_mesh(fragment, mesh);
    auto destination = device->imageInfo().refColorSpace();
    if (!destination) destination = SkColorSpace::MakeSRGB();
    auto working = sk_ref_sp(mesh.spec()->colorSpace());
    if (!working) working = destination;
    auto paintShader = paint.refShader() ? paint.refShader() :
        SkShaders::Color(paint.getColor4f().makeOpaque(), nullptr);
    // Mesh stages use the specification space; paint enters primitive blending in destination space.
    fragment.child(p + "paint") = paintShader->makeWithWorkingColorSpace(destination, working);
    if (SkMeshSpecificationPriv::HasColors(*mesh.spec())) {
        fragment.child(p + "blend") = blender ? std::move(blender) : SkBlender::Mode(SkBlendMode::kModulate);
        sk_sp<SkColorFilter> xform = sk_make_sp<SkColorSpaceXformColorFilter>(
            sk_ref_sp(mesh.spec()->colorSpace()), destination);
        fragment.child(p + "xform") = std::move(xform);
    }
    auto evaluation = SkSurfaces::Raster(SkImageInfo::Make(groups, 1, kRGBA_F32_SkColorType,
        kPremul_SkAlphaType, sk_ref_sp(mesh.spec()->colorSpace())));
    if (!evaluation) return;
    const size_t count = mesh.vertexCount();
    std::vector<float> values(count * groups * 4);
    std::vector<float> attrs(mesh.spec()->attributes().size() * 4);
    std::vector<SkPoint> positions(count);
    const auto* vertices = static_cast<const uint8_t*>(
        static_cast<const SkMeshPriv::VB*>(mesh.vertexBuffer())->peek()) + mesh.vertexOffset();
    const auto* instances = mesh.instanceBuffer() ? static_cast<const uint8_t*>(
        static_cast<const SkMeshPriv::VB*>(mesh.instanceBuffer())->peek()) + mesh.instanceOffset() : nullptr;
    const uint16_t* indices = mesh.indexBuffer() ? reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(
        static_cast<const SkMeshPriv::IB*>(mesh.indexBuffer())->peek()) + mesh.indexOffset()) : nullptr;
    size_t position_slot = 0;
    for (const auto& v : SkMeshSpecificationPriv::Varyings(*mesh.spec())) {
        if (v.name.equals("position")) break;
        position_slot += components(v.type);
    }
    for (size_t instance = 0; instance < (instances ? mesh.instanceCount() : 1); ++instance) {
        for (size_t v = 0; v < count; ++v) {
            size_t a_index = 0;
            for (const auto& a : mesh.spec()->attributes()) {
                const auto* data = a.rate == SkMeshSpecification::Attribute::Rate::kInstance ?
                    instances + instance * mesh.spec()->instanceStride() : vertices + v * mesh.spec()->stride();
                float* out = attrs.data() + a_index++ * 4;
                if (a.type == SkMeshSpecification::Attribute::Type::kUByte4_unorm) {
                    for (int c = 0; c < 4; ++c) out[c] = data[a.offset + c] * (1.f / 255.f);
                } else { std::memcpy(out, data + a.offset, SkMeshSpecificationPriv::AttrTypeByteSize(a.type)); }
            }
            vertex.uniform(p + "attrs").set(attrs.data(), static_cast<int>(attrs.size()));
            SkPaint evaluate;
            evaluate.setBlendMode(SkBlendMode::kSrc);
            evaluate.setShader(vertex.makeShader());
            evaluation->getCanvas()->drawPaint(evaluate);
            SkPixmap pixmap;
            evaluation->peekPixels(&pixmap);
            float* output = values.data() + v * groups * 4;
            std::memcpy(output, pixmap.addr(), groups * 4 * sizeof(float));
            positions[v] = {output[position_slot], output[position_slot + 1]};
        }
        VertState state(count, indices, mesh.indexCount());
        auto proc = state.chooseProc(mesh.mode() == SkMesh::Mode::kTriangles ?
            SkVertices::kTriangles_VertexMode : SkVertices::kTriangleStrip_VertexMode);
        while (proc(&state)) {
            const int ids[] = {state.f0, state.f1, state.f2};
            const SkPoint triangle[] = {positions[ids[0]], positions[ids[1]], positions[ids[2]]};
            const SkPoint weights[] = {{1, 0}, {0, 1}, {0, 0}};
            SkMatrix bary;
            if (!bary.setPolyToPoly(triangle, weights)) continue;
            fragment.uniform(p + "bary") = bary;
            std::vector<float> triangle_values(groups * 12);
            for (int v = 0; v < 3; ++v) {
                std::memcpy(triangle_values.data() + v * groups * 4, values.data() + ids[v] * groups * 4,
                            groups * 4 * sizeof(float));
            }
            fragment.uniform(p + "values").set(triangle_values.data(), static_cast<int>(triangle_values.size()));
            SkPaint shaded = paint;
            shaded.setShader(fragment.makeShader()->makeWithWorkingColorSpace(working, destination));
            auto geometry = SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode, 3, triangle, nullptr, nullptr);
            device->drawVertices(geometry.get(), SkBlender::Mode(SkBlendMode::kSrc), shaded, false);
        }
    }
}
