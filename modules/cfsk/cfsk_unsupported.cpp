/* C ABI validation and ownership stay at the boundary; Skia owns rendering. */
#include "cfsk_internal.h"
struct cfsk_font_collection {};
struct cfsk_font_provider {};
struct cfsk_mesh {};
struct cfsk_mesh_index_buffer {};
struct cfsk_mesh_spec {};
struct cfsk_mesh_vertex_buffer {};
struct cfsk_paragraph {};
struct cfsk_paragraph_builder {};
struct cfsk_skottie {};

cfsk_status cfsk_font_provider_make(cfsk_font_provider** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_font_provider_make: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_font_provider_register_bytes(cfsk_font_provider* provider, cfsk_bytes bytes, cfsk_string alias) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(bytes));
    CFSK_REQUIRE(cfsk::valid(alias));
    return cfsk::unsupported("cfsk_font_provider_register_bytes: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_font_provider_register_typeface(cfsk_font_provider* provider, const cfsk_typeface* face, cfsk_string alias) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(alias));
    return cfsk::unsupported("cfsk_font_provider_register_typeface: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_font_collection_make(cfsk_font_collection** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_font_collection_make: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_font_collection_set_provider(cfsk_font_collection* collection, cfsk_font_provider* provider) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_font_collection_set_provider: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_builder_make(const cfsk_paragraph_style* style, cfsk_font_collection* fonts, cfsk_paragraph_builder** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_paragraph_builder_make: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_builder_add_text(cfsk_paragraph_builder* builder, cfsk_string text) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(text));
    return cfsk::unsupported("cfsk_paragraph_builder_add_text: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_builder_build(cfsk_paragraph_builder* builder, cfsk_paragraph** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_paragraph_builder_build: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_layout(cfsk_paragraph* paragraph, float width) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(width));
    return cfsk::unsupported("cfsk_paragraph_layout: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_longest_line(const cfsk_paragraph* paragraph, float* output) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_paragraph_longest_line: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_max_intrinsic_width(const cfsk_paragraph* paragraph, float* output) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_paragraph_max_intrinsic_width: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_line_metrics(const cfsk_paragraph* paragraph, cfsk_line_metrics* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_paragraph_line_metrics: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_shaped_runs(const cfsk_paragraph* paragraph, cfsk_shaped_run* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_paragraph_shaped_runs: SkParagraph/HarfBuzz/Unicode backend is not built");
}

cfsk_status cfsk_paragraph_run_glyphs(const cfsk_paragraph* paragraph, size_t run_index, uint16_t* glyphs, cfsk_point* positions, uint32_t* clusters, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    return cfsk::unsupported("cfsk_paragraph_run_glyphs: SkParagraph/HarfBuzz/Unicode backend is not built");
}

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

void cfsk_font_provider_destroy(cfsk_font_provider* value) {
    delete value;
}

void cfsk_font_collection_destroy(cfsk_font_collection* value) {
    delete value;
}

void cfsk_paragraph_builder_destroy(cfsk_paragraph_builder* value) {
    delete value;
}

void cfsk_paragraph_destroy(cfsk_paragraph* value) {
    delete value;
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
