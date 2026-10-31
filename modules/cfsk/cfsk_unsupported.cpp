/* C ABI validation and ownership stay at the boundary; Skia owns rendering. */
#include "cfsk_internal.h"
struct cfsk_mesh {};
struct cfsk_mesh_index_buffer {};
struct cfsk_mesh_spec {};
struct cfsk_mesh_vertex_buffer {};
struct cfsk_skottie {};

cfsk_status cfsk_mesh_spec_make(const cfsk_mesh_attribute* attributes, size_t attribute_count, size_t stride_bytes, const cfsk_mesh_varying* varyings, size_t varying_count, cfsk_string vertex_sksl, cfsk_string fragment_sksl, const cfsk_color_space* space, uint32_t alpha_type, size_t instance_stride_bytes, cfsk_mesh_spec** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(vertex_sksl));
    CFSK_REQUIRE(cfsk::valid(fragment_sksl));
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_mesh_spec_make: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_spec_uniform_size(const cfsk_mesh_spec* spec, size_t* output_bytes) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_mesh_spec_uniform_size: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_vertex_buffer_make(cfsk_recorder* recorder, cfsk_bytes bytes, cfsk_mesh_vertex_buffer** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(bytes));
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_mesh_vertex_buffer_make: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_index_buffer_make(cfsk_recorder* recorder, const uint16_t* indices, size_t count, cfsk_mesh_index_buffer** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_mesh_index_buffer_make: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_vertex_buffer_size(const cfsk_mesh_vertex_buffer* buffer, size_t* output_bytes) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_mesh_vertex_buffer_size: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_vertex_buffer_update(cfsk_recorder* recorder, cfsk_mesh_vertex_buffer* buffer, size_t offset_bytes, cfsk_bytes bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(bytes));
    return cfsk::unsupported("cfsk_mesh_vertex_buffer_update: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_index_buffer_size(const cfsk_mesh_index_buffer* buffer, size_t* output_bytes) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_mesh_index_buffer_size: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_index_buffer_update(cfsk_recorder* recorder, cfsk_mesh_index_buffer* buffer, size_t offset_bytes, cfsk_bytes bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(bytes));
    return cfsk::unsupported("cfsk_mesh_index_buffer_update: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_mesh_make(const cfsk_mesh_desc* descriptor, cfsk_mesh** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_mesh_make: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_canvas_draw_mesh(cfsk_canvas* canvas, const cfsk_mesh* mesh, uint32_t blend_mode, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    return cfsk::unsupported("cfsk_canvas_draw_mesh: Graphite mesh RenderStep backend is not built");
}

cfsk_status cfsk_skottie_make(cfsk_string json, const cfsk_asset* assets, size_t asset_count, cfsk_skottie** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(json));
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_skottie_make: Skottie backend is not built");
}

cfsk_status cfsk_skottie_seek_frame(cfsk_skottie* animation, double frame, cfsk_rect* damage) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(frame));
    return cfsk::unsupported("cfsk_skottie_seek_frame: Skottie backend is not built");
}

cfsk_status cfsk_skottie_render(cfsk_skottie* animation, cfsk_canvas* canvas, const cfsk_rect* destination) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    return cfsk::unsupported("cfsk_skottie_render: Skottie backend is not built");
}

void cfsk_mesh_spec_destroy(cfsk_mesh_spec* value) {
    delete value;
}

void cfsk_mesh_vertex_buffer_destroy(cfsk_mesh_vertex_buffer* value) {
    delete value;
}

void cfsk_mesh_index_buffer_destroy(cfsk_mesh_index_buffer* value) {
    delete value;
}

void cfsk_mesh_destroy(cfsk_mesh* value) {
    delete value;
}

void cfsk_skottie_destroy(cfsk_skottie* value) {
    delete value;
}
