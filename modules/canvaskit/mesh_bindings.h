#include "include/core/SkMesh.h"
#include "include/gpu/ganesh/SkMeshGanesh.h"

namespace ck_mesh {
using namespace emscripten;

template <typename Buffer> struct RetainedBuffer {
    sk_sp<Buffer> buffer;
    sk_sp<GrDirectContext> context;
    bool update(uintptr_t data, size_t offset, size_t size) {
        return buffer && buffer->update(context.get(), reinterpret_cast<void*>(data), offset, size);
    }
    size_t size() const { return buffer->size(); }
    bool gpuBacked() const { return context != nullptr; }
};
using VertexBuffer = RetainedBuffer<SkMesh::VertexBuffer>;
using IndexBuffer = RetainedBuffer<SkMesh::IndexBuffer>;
struct Mesh { SkMesh mesh; };

inline val specification(val attrs, size_t stride, val vars, std::string vs, std::string fs,
                         sk_sp<SkColorSpace> cs, SkAlphaType alpha, size_t instanceStride) {
    using A = SkMeshSpecification::Attribute;
    using V = SkMeshSpecification::Varying;
    std::vector<A> attributes;
    std::vector<V> varyings;
    for (unsigned i = 0; i < attrs["length"].as<unsigned>(); ++i) {
        val a = attrs[i];
        attributes.push_back({static_cast<A::Type>(a["type"].as<unsigned>()),
                              a["offset"].as<size_t>(), SkString(a["name"].as<std::string>()),
                              static_cast<A::Rate>(a["rate"].as<unsigned>())});
    }
    for (unsigned i = 0; i < vars["length"].as<unsigned>(); ++i) {
        val v = vars[i];
        varyings.push_back({static_cast<V::Type>(v["type"].as<unsigned>()),
                           SkString(v["name"].as<std::string>())});
    }
    auto result = SkMeshSpecification::Make(SkSpan(attributes), stride, SkSpan(varyings),
                                            SkString(vs), SkString(fs), std::move(cs), alpha, instanceStride);
    val output = val::object();
    if (result.specification) output.set("specification", result.specification);
    else output.set("error", std::string(result.error.c_str()));
    return output;
}

inline std::shared_ptr<VertexBuffer> vertexBuffer(sk_sp<GrDirectContext> context,
                                                 uintptr_t data, size_t size) {
    auto buffer = SkMeshes::MakeVertexBuffer(context.get(), reinterpret_cast<void*>(data), size);
    if (!buffer) return nullptr;
    return std::make_shared<VertexBuffer>(VertexBuffer{std::move(buffer), std::move(context)});
}
inline std::shared_ptr<IndexBuffer> indexBuffer(sk_sp<GrDirectContext> context,
                                               uintptr_t data, size_t size) {
    auto buffer = SkMeshes::MakeIndexBuffer(context.get(), reinterpret_cast<void*>(data), size);
    if (!buffer) return nullptr;
    return std::make_shared<IndexBuffer>(IndexBuffer{std::move(buffer), std::move(context)});
}
inline std::shared_ptr<Mesh> makeMesh(sk_sp<SkMeshSpecification> spec, int mode,
                                    std::shared_ptr<VertexBuffer> vb, size_t vertexCount,
                                    size_t vertexOffset, std::shared_ptr<IndexBuffer> ib,
                                    size_t indexCount, size_t indexOffset, uintptr_t uniformPtr,
                                    size_t uniformSize, uintptr_t boundsPtr, std::shared_ptr<VertexBuffer> instances,
                                    size_t instanceOffset, size_t instanceCount) {
    if (!spec || !vb || mode < 0 || mode > 1 || uniformSize != spec->uniformSize()) return nullptr;
    if ((ib && vb->context != ib->context) || (instances && vb->context != instances->context)) {
        return nullptr;
    }
    SkMesh::Instances instanceData{instances ? instances->buffer : nullptr, instanceOffset, instanceCount};
    const auto* descriptor = instances ? &instanceData : nullptr;
    auto uniforms = SkData::MakeWithCopy(reinterpret_cast<void*>(uniformPtr), uniformSize);
    const auto bounds = *reinterpret_cast<SkRect*>(boundsPtr);
    if (!bounds.isFinite() || bounds.isEmpty()) return nullptr;
    auto result = ib ? SkMesh::MakeIndexed(spec, static_cast<SkMesh::Mode>(mode), vb->buffer,
                           vertexCount, vertexOffset, ib->buffer, indexCount, indexOffset,
                           uniforms, {}, bounds, descriptor)
                     : SkMesh::Make(spec, static_cast<SkMesh::Mode>(mode), vb->buffer,
                           vertexCount, vertexOffset, uniforms, {}, bounds, descriptor);
    if (!result.mesh.isValid()) return nullptr;
    return std::make_shared<Mesh>(Mesh{std::move(result.mesh)});
}
inline sk_sp<GrDirectContext> context(SkCanvas& canvas) {
    return sk_ref_sp(GrAsDirectContext(canvas.recordingContext()));
}
inline void draw(SkCanvas& canvas, const Mesh& mesh, SkBlendMode blender, const SkPaint& paint) {
    if (mesh.mesh.instanceBuffer() && !mesh.mesh.instanceCount()) return;
    canvas.drawMesh(mesh.mesh, SkBlender::Mode(blender), paint);
}
}

EMSCRIPTEN_BINDINGS(CanvasKitMesh) {
    using namespace emscripten;
    class_<SkMeshSpecification>("MeshSpecification")
        .smart_ptr<sk_sp<SkMeshSpecification>>("sk_sp<SkMeshSpecification>")
        .class_function("_Make", &ck_mesh::specification)
        .function("uniformSize", &SkMeshSpecification::uniformSize);
    class_<ck_mesh::VertexBuffer>("MeshVertexBuffer")
        .smart_ptr<std::shared_ptr<ck_mesh::VertexBuffer>>("shared_ptr<MeshVertexBuffer>")
        .function("_update", &ck_mesh::VertexBuffer::update)
        .function("size", &ck_mesh::VertexBuffer::size)
        .function("gpuBacked", &ck_mesh::VertexBuffer::gpuBacked);
    class_<ck_mesh::IndexBuffer>("MeshIndexBuffer")
        .smart_ptr<std::shared_ptr<ck_mesh::IndexBuffer>>("shared_ptr<MeshIndexBuffer>")
        .function("_update", &ck_mesh::IndexBuffer::update)
        .function("size", &ck_mesh::IndexBuffer::size)
        .function("gpuBacked", &ck_mesh::IndexBuffer::gpuBacked);
    class_<ck_mesh::Mesh>("Mesh").smart_ptr<std::shared_ptr<ck_mesh::Mesh>>("shared_ptr<Mesh>");
    function("_MakeMeshVertexBuffer", &ck_mesh::vertexBuffer);
    function("_MakeMeshIndexBuffer", &ck_mesh::indexBuffer);
    function("_MakeMesh", &ck_mesh::makeMesh);
}
