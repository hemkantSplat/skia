/* Mesh handles retain inputs; Graphite snapshots CPU staging at each draw before later updates. */
#include "cfsk_internal.h"
#include "src/core/SkMeshPriv.h"
#include "src/core/SkMeshRasterPriv.h"

namespace {
using Attribute = SkMeshSpecification::Attribute;
using Varying = SkMeshSpecification::Varying;
static_assert(static_cast<uint32_t>(Attribute::Type::kFloat) == 0);
static_assert(static_cast<uint32_t>(Attribute::Type::kFloat2) == 1);
static_assert(static_cast<uint32_t>(Attribute::Type::kFloat3) == 2);
static_assert(static_cast<uint32_t>(Attribute::Type::kFloat4) == 3);
static_assert(static_cast<uint32_t>(Attribute::Type::kUByte4_unorm) == 4);
static_assert(static_cast<uint32_t>(Attribute::Rate::kVertex) == 0);
static_assert(static_cast<uint32_t>(Attribute::Rate::kInstance) == 1);
static_assert(static_cast<uint32_t>(Varying::Type::kFloat) == 0);
static_assert(static_cast<uint32_t>(Varying::Type::kFloat2) == 1);
static_assert(static_cast<uint32_t>(Varying::Type::kFloat3) == 2);
static_assert(static_cast<uint32_t>(Varying::Type::kFloat4) == 3);
static_assert(static_cast<uint32_t>(Varying::Type::kHalf) == 4);
static_assert(static_cast<uint32_t>(Varying::Type::kHalf2) == 5);
static_assert(static_cast<uint32_t>(Varying::Type::kHalf3) == 6);
static_assert(static_cast<uint32_t>(Varying::Type::kHalf4) == 7);
static_assert(static_cast<uint32_t>(SkMesh::Mode::kTriangles) == 0);
static_assert(static_cast<uint32_t>(SkMesh::Mode::kTriangleStrip) == 1);

template <typename T> cfsk_status ready(const T* buffer) {
    CFSK_REQUIRE(buffer && buffer->value);
    if (buffer->owner != std::this_thread::get_id())
        return cfsk::fail(CFSK_WRONG_THREAD, "mesh buffer owner mismatch");
    return CFSK_OK;
}
template <typename T> cfsk_status update(cfsk_recorder* recorder, T* buffer,
                                        size_t offset, cfsk_bytes bytes) {
    if (auto status = ready(buffer); status != CFSK_OK) return status;
    const void* recorder_id = nullptr;
    if (auto status = cfsk::mesh_recorder_identity(recorder, &recorder_id); status != CFSK_OK) return status;
    CFSK_REQUIRE(recorder_id == buffer->recorder_id && cfsk::valid(bytes));
    CFSK_REQUIRE(bytes.length && !(offset % 4) && !(bytes.length % 4));
    CFSK_REQUIRE(offset <= buffer->value->size() && bytes.length <= buffer->value->size() - offset);
    return buffer->value->update(nullptr, bytes.data, offset, bytes.length) ? CFSK_OK :
        cfsk::fail(CFSK_FAILED, "mesh buffer update failed");
}
bool valid_indices(const SkMesh& mesh) {
    if (!mesh.indexBuffer()) return true;
    const auto* buffer = static_cast<const SkMeshPriv::IB*>(mesh.indexBuffer());
    const auto* bytes = static_cast<const uint8_t*>(buffer->peek()) + mesh.indexOffset();
    for (size_t i = 0; i < mesh.indexCount(); ++i) {
        uint16_t index;
        std::memcpy(&index, bytes + i * sizeof(index), sizeof(index));
        if (index >= mesh.vertexCount()) return false;
    }
    return true;
}
}

cfsk_status cfsk_mesh_spec_make(const cfsk_mesh_attribute* attributes, size_t attribute_count,
        size_t stride_bytes, const cfsk_mesh_varying* varyings, size_t varying_count,
        cfsk_string vertex_sksl, cfsk_string fragment_sksl, const cfsk_color_space* space,
        uint32_t alpha_type, size_t instance_stride_bytes, cfsk_mesh_spec** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(attributes && attribute_count && attribute_count <= SkMeshSpecification::kMaxAttributes);
    CFSK_REQUIRE((!varying_count || varyings) && varying_count <= SkMeshSpecification::kMaxVaryings);
    CFSK_REQUIRE(cfsk::valid(vertex_sksl) && cfsk::valid(fragment_sksl));
    CFSK_REQUIRE(alpha_type >= 1 && alpha_type <= 3);
    std::vector<Attribute> native_attributes;
    std::vector<Varying> native_varyings;
    for (size_t i = 0; i < attribute_count; ++i) {
        const auto& a = attributes[i];
        CFSK_REQUIRE(cfsk::valid(a.name) && a.type <= 4 && a.rate <= 1);
        native_attributes.push_back({static_cast<Attribute::Type>(a.type), a.offset,
            SkString(a.name.data, a.name.length), static_cast<Attribute::Rate>(a.rate)});
    }
    for (size_t i = 0; i < varying_count; ++i) {
        const auto& v = varyings[i];
        CFSK_REQUIRE(cfsk::valid(v.name) && v.type <= 7);
        native_varyings.push_back({static_cast<Varying::Type>(v.type), SkString(v.name.data, v.name.length)});
    }
    auto result = SkMeshSpecification::Make(SkSpan(native_attributes), stride_bytes, SkSpan(native_varyings),
        SkString(vertex_sksl.data, vertex_sksl.length), SkString(fragment_sksl.data, fragment_sksl.length),
        space ? space->value : SkColorSpace::MakeSRGB(), static_cast<SkAlphaType>(alpha_type), instance_stride_bytes);
    if (!result.specification) return cfsk::fail(CFSK_INVALID_ARGUMENT, result.error.c_str());
    return cfsk::owned(output, std::move(result.specification));
}

cfsk_status cfsk_mesh_spec_uniform_size(const cfsk_mesh_spec* spec, size_t* output_bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(spec && output_bytes);
    *output_bytes = spec->value->uniformSize();
    return CFSK_OK;
}

cfsk_status cfsk_mesh_vertex_buffer_make(cfsk_recorder* recorder, cfsk_bytes bytes,
                                        cfsk_mesh_vertex_buffer** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::valid(bytes) && bytes.length && !(bytes.length % 4));
    const void* recorder_id = nullptr;
    if (auto status = cfsk::mesh_recorder_identity(recorder, &recorder_id); status != CFSK_OK) return status;
    auto value = SkMeshes::MakeVertexBuffer(bytes.data, bytes.length);
    if (!value) return cfsk::fail(CFSK_FAILED, "mesh vertex buffer allocation failed");
    *output = new (std::nothrow) cfsk_mesh_vertex_buffer{std::move(value), recorder_id};
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "wrapper allocation failed");
}

cfsk_status cfsk_mesh_index_buffer_make(cfsk_recorder* recorder, const uint16_t* indices,
                                       size_t count, cfsk_mesh_index_buffer** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(indices && count && count <= SIZE_MAX / sizeof(uint16_t));
    const void* recorder_id = nullptr;
    if (auto status = cfsk::mesh_recorder_identity(recorder, &recorder_id); status != CFSK_OK) return status;
    auto value = SkMeshes::MakeIndexBuffer(indices, count * sizeof(uint16_t));
    if (!value) return cfsk::fail(CFSK_FAILED, "mesh index buffer allocation failed");
    *output = new (std::nothrow) cfsk_mesh_index_buffer{std::move(value), recorder_id};
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "wrapper allocation failed");
}

cfsk_status cfsk_mesh_vertex_buffer_size(const cfsk_mesh_vertex_buffer* buffer, size_t* output_bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(output_bytes);
    if (auto status = ready(buffer); status != CFSK_OK) return status;
    *output_bytes = buffer->value->size();
    return CFSK_OK;
}
cfsk_status cfsk_mesh_index_buffer_size(const cfsk_mesh_index_buffer* buffer, size_t* output_bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(output_bytes);
    if (auto status = ready(buffer); status != CFSK_OK) return status;
    *output_bytes = buffer->value->size();
    return CFSK_OK;
}
cfsk_status cfsk_mesh_vertex_buffer_update(cfsk_recorder* recorder, cfsk_mesh_vertex_buffer* buffer,
                                          size_t offset_bytes, cfsk_bytes bytes) {
    CFSK_BEGIN;
    return update(recorder, buffer, offset_bytes, bytes);
}
cfsk_status cfsk_mesh_index_buffer_update(cfsk_recorder* recorder, cfsk_mesh_index_buffer* buffer,
                                         size_t offset_bytes, cfsk_bytes bytes) {
    CFSK_BEGIN;
    return update(recorder, buffer, offset_bytes, bytes);
}

cfsk_status cfsk_mesh_make(const cfsk_mesh_desc* descriptor, cfsk_mesh** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(descriptor && descriptor->spec && descriptor->mode <= 1);
    const auto& d = *descriptor;
    CFSK_REQUIRE(cfsk::valid(d.uniforms) && d.uniforms.length == d.spec->value->uniformSize());
    CFSK_REQUIRE(cfsk::finite(d.bounds) && !cfsk::rect(d.bounds).isEmpty());
    CFSK_REQUIRE((!d.child_count || d.children) && d.child_count == d.spec->value->children().size());
    if (auto status = ready(d.vertices.buffer); status != CFSK_OK) return status;
    const void* recorder_id = d.vertices.buffer->recorder_id;
    if (d.indices.buffer) {
        if (auto status = ready(d.indices.buffer); status != CFSK_OK) return status;
        CFSK_REQUIRE(d.indices.buffer->recorder_id == recorder_id);
    } else { CFSK_REQUIRE(!d.indices.count && !d.indices.offset_bytes); }
    if (d.instances.buffer) {
        if (auto status = ready(d.instances.buffer); status != CFSK_OK) return status;
        CFSK_REQUIRE(d.instances.buffer->recorder_id == recorder_id);
    } else { CFSK_REQUIRE(!d.instances.count && !d.instances.offset_bytes); }
    CFSK_REQUIRE(d.vertices.count <= INT32_MAX && d.indices.count <= INT32_MAX);
    if (d.instances.buffer && d.instances.count) {
        const size_t count = d.indices.buffer ? d.indices.count : d.vertices.count;
        CFSK_REQUIRE(d.instances.count <= INT32_MAX && count <= INT32_MAX / d.instances.count);
    }
    std::vector<SkMesh::ChildPtr> children;
    for (size_t i = 0; i < d.child_count; ++i) {
        const auto& c = d.children[i];
        switch (c.kind) {
            case 0: children.emplace_back(); break;
            case 1: CFSK_REQUIRE(c.value.shader); children.emplace_back(c.value.shader->value); break;
            case 2: CFSK_REQUIRE(c.value.color_filter); children.emplace_back(c.value.color_filter->value); break;
            case 3: CFSK_REQUIRE(c.value.blender); children.emplace_back(c.value.blender->value); break;
            default: return cfsk::fail(CFSK_INVALID_ARGUMENT, "invalid mesh child kind");
        }
    }
    auto uniforms = SkData::MakeWithCopy(d.uniforms.data, d.uniforms.length);
    SkMesh::Instances instances{cfsk::ref(d.instances.buffer), d.instances.offset_bytes, d.instances.count};
    const auto* instance_data = d.instances.buffer ? &instances : nullptr;
    auto result = d.indices.buffer ? SkMesh::MakeIndexed(d.spec->value, static_cast<SkMesh::Mode>(d.mode),
        d.vertices.buffer->value, d.vertices.count, d.vertices.offset_bytes, d.indices.buffer->value,
        d.indices.count, d.indices.offset_bytes, uniforms, SkSpan(children), cfsk::rect(d.bounds), instance_data) :
        SkMesh::Make(d.spec->value, static_cast<SkMesh::Mode>(d.mode), d.vertices.buffer->value,
        d.vertices.count, d.vertices.offset_bytes, uniforms, SkSpan(children), cfsk::rect(d.bounds), instance_data);
    if (!result.mesh.isValid()) return cfsk::fail(CFSK_INVALID_ARGUMENT, result.error.c_str());
    CFSK_REQUIRE(valid_indices(result.mesh));
    *output = new (std::nothrow) cfsk_mesh{std::move(result.mesh), recorder_id};
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "wrapper allocation failed");
}

cfsk_status cfsk_canvas_draw_mesh(cfsk_canvas* canvas, const cfsk_mesh* mesh,
                                 uint32_t blend_mode, const cfsk_paint* paint) {
    CFSK_BEGIN;
    CFSK_REQUIRE(canvas && canvas->value && mesh && paint && blend_mode <= 28);
    if (canvas->owner != std::this_thread::get_id() || mesh->owner != std::this_thread::get_id())
        return cfsk::fail(CFSK_WRONG_THREAD, "canvas or mesh owner mismatch");
    CFSK_REQUIRE(!mesh->recorder_id || mesh->recorder_id == canvas->value->recorder());
    CFSK_REQUIRE(valid_indices(mesh->value));
    if (!canvas->value->recorder()) {
        const auto& program = SkMeshRasterPriv::Get(*mesh->value.spec());
        if (!program.error.empty()) return cfsk::fail(CFSK_UNSUPPORTED, program.error.c_str());
    }
    canvas->value->drawMesh(mesh->value, SkBlender::Mode(static_cast<SkBlendMode>(blend_mode)), paint->value);
    return CFSK_OK;
}

void cfsk_mesh_spec_destroy(cfsk_mesh_spec* value) { delete value; }
void cfsk_mesh_vertex_buffer_destroy(cfsk_mesh_vertex_buffer* value) { delete value; }
void cfsk_mesh_index_buffer_destroy(cfsk_mesh_index_buffer* value) { delete value; }
void cfsk_mesh_destroy(cfsk_mesh* value) { delete value; }
