/* Continuous coverage stays in float texcoords; the caller supplies the layer and SrcIn fill. */
#include "cfsk_internal.h"

cfsk_status cfsk_canvas_draw_coverage_mesh(cfsk_canvas* canvas, const float* positions,
        const float* coverage, size_t vertex_count, const uint16_t* indices, size_t index_count) {
    CFSK_BEGIN;
    CFSK_REQUIRE(canvas && canvas->value);
    if (canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(vertex_count <= 65532 && index_count <= INT_MAX && index_count % 3 == 0);
    if (!index_count) return CFSK_OK;
    CFSK_REQUIRE(vertex_count && indices && cfsk::finite(positions, vertex_count * 2) && cfsk::finite(coverage, vertex_count * 2));
    for (size_t i = 0; i < index_count; ++i) CFSK_REQUIRE(indices[i] < vertex_count);
    std::vector<SkPoint> points(vertex_count), texcoords(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        points[i] = {positions[i * 2], positions[i * 2 + 1]};
        texcoords[i] = {coverage[i * 2], coverage[i * 2 + 1]};
    }
    static const auto effect = SkRuntimeEffect::MakeForShader(
        SkString("half4 main(float2 coverage) { return half4(coverage.x); }")).effect;
    if (!effect) return cfsk::fail(CFSK_FAILED, "coverage shader unavailable");
    SkPaint paint;
    paint.setShader(effect->makeShader(SkData::MakeEmpty(), nullptr, 0));
    if (!paint.getShader()) return cfsk::fail(CFSK_FAILED, "coverage shader creation failed");
    auto vertices = SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode,
        static_cast<int>(vertex_count), points.data(), texcoords.data(), nullptr,
        static_cast<int>(index_count), indices);
    if (!vertices) return cfsk::fail(CFSK_FAILED, "coverage vertices creation failed");
    canvas->value->drawVertices(vertices, SkBlendMode::kSrc, paint);
    return CFSK_OK;
}
