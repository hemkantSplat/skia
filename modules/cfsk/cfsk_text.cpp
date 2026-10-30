/* C ABI validation and ownership stay at the boundary; Skia owns rendering. */
#include "cfsk_internal.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkRSXform.h"
#include "include/core/SkStream.h"
#include "src/ports/SkTypeface_FreeType.h"  // nogncheck: explicit CPU font decoder at the pinned fork.

cfsk_status cfsk_typeface_from_data(cfsk_bytes bytes, int32_t collection_index, cfsk_typeface** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(bytes));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(collection_index >= 0);
    SkFontArguments args; args.setCollectionIndex(collection_index);
    return cfsk::resource(output, SkTypeface_FreeType::MakeFromStream(SkMemoryStream::Make(SkData::MakeWithCopy(bytes.data, bytes.length)), args));
}

cfsk_status cfsk_typeface_variation_axes(const cfsk_typeface* face, cfsk_axis* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(face && required && (!capacity || output));
    int count = face->value->getVariationDesignParameters({});
    if (count < 0) return cfsk::fail(CFSK_FAILED, "variation axes unavailable");
    *required = static_cast<size_t>(count);
    if (!output && !capacity) return CFSK_OK;
    if (capacity < *required) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "axis buffer too small");
    std::vector<SkFontParameters::Variation::Axis> axes(count);
    if (face->value->getVariationDesignParameters(SkSpan<SkFontParameters::Variation::Axis>(axes)) != count) return cfsk::fail(CFSK_FAILED, "axis read failed");
    for (int i = 0; i < count; ++i) output[i] = {axes[i].tag, axes[i].min, axes[i].max, axes[i].def, static_cast<uint8_t>(axes[i].isHidden())};
    return CFSK_OK;
}

cfsk_status cfsk_typeface_make_variation(const cfsk_typeface* face, const cfsk_axis_coordinate* coordinates, size_t count, cfsk_typeface** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(face && count <= INT_MAX && (!count || coordinates));
    std::vector<SkFontArguments::VariationPosition::Coordinate> values;
    for (size_t i = 0; i < count; ++i) { CFSK_REQUIRE(cfsk::finite(coordinates[i].value)); values.push_back({coordinates[i].tag, coordinates[i].value}); }
    SkFontArguments args; args.setVariationDesignPosition({values.data(), static_cast<int>(count)});
    return cfsk::resource(output, face->value->makeClone(args));
}

cfsk_status cfsk_font_make(const cfsk_typeface* face, float size, float scale_x, float skew_x, cfsk_font** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(size));
    CFSK_REQUIRE(cfsk::finite(scale_x));
    CFSK_REQUIRE(cfsk::finite(skew_x));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(size >= 0);
    return cfsk::owned(output, SkFont(cfsk::ref(face), size, scale_x, skew_x));
}

cfsk_status cfsk_font_set_size(cfsk_font* font, float value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(value));
    CFSK_REQUIRE(font && (value >= 0));
    font->value.setSize(value);
    return CFSK_OK;
}

cfsk_status cfsk_font_set_skew_x(cfsk_font* font, float value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(value));
    CFSK_REQUIRE(font && (true));
    font->value.setSkewX(value);
    return CFSK_OK;
}

cfsk_status cfsk_font_set_edging(cfsk_font* font, uint32_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(font && (value <= 2));
    font->value.setEdging(static_cast<SkFont::Edging>(value));
    return CFSK_OK;
}

cfsk_status cfsk_font_set_hinting(cfsk_font* font, uint32_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(font && (value <= 3));
    font->value.setHinting(static_cast<SkFontHinting>(value));
    return CFSK_OK;
}

cfsk_status cfsk_font_set_embolden(cfsk_font* font, uint8_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(value <= 1);
    CFSK_REQUIRE(font && (true));
    font->value.setEmbolden(value);
    return CFSK_OK;
}

cfsk_status cfsk_font_set_linear_metrics(cfsk_font* font, uint8_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(value <= 1);
    CFSK_REQUIRE(font && (true));
    font->value.setLinearMetrics(value);
    return CFSK_OK;
}

cfsk_status cfsk_font_set_subpixel(cfsk_font* font, uint8_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(value <= 1);
    CFSK_REQUIRE(font && (true));
    font->value.setSubpixel(value);
    return CFSK_OK;
}

cfsk_status cfsk_font_glyph_ids(const cfsk_font* font, cfsk_string utf8, uint16_t* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(utf8));
    CFSK_REQUIRE(font && required && (!capacity || output) && utf8.length <= INT_MAX);
    int count = font->value.countText(utf8.data, utf8.length, SkTextEncoding::kUTF8);
    CFSK_REQUIRE(count >= 0);
    *required = static_cast<size_t>(count);
    if (!output && !capacity) return CFSK_OK;
    if (capacity < *required) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "glyph buffer too small");
    font->value.textToGlyphs(utf8.data, utf8.length, SkTextEncoding::kUTF8, SkSpan<SkGlyphID>(output, count));
    return CFSK_OK;
}

cfsk_status cfsk_font_glyph_widths(const cfsk_font* font, const uint16_t* glyphs, size_t count, const cfsk_paint* paint, float* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(font && count <= INT_MAX && (!count || (glyphs && output)));
    font->value.getWidthsBounds(SkSpan<const SkGlyphID>(glyphs, count), SkSpan<SkScalar>(output, count), {}, paint ? &paint->value : nullptr);
    return CFSK_OK;
}

cfsk_status cfsk_font_glyph_bounds(const cfsk_font* font, const uint16_t* glyphs, size_t count, const cfsk_paint* paint, cfsk_rect* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(font && count <= INT_MAX && (!count || (glyphs && output)));
    std::vector<SkRect> bounds(count);
    font->value.getBounds(SkSpan<const SkGlyphID>(glyphs, count), SkSpan<SkRect>(bounds), paint ? &paint->value : nullptr);
    for (size_t i = 0; i < count; ++i) output[i] = cfsk::rect(bounds[i]);
    return CFSK_OK;
}

cfsk_status cfsk_font_get_metrics(const cfsk_font* font, cfsk_font_metrics* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(font && output);
    SkFontMetrics m;
    font->value.getMetrics(&m);
    *output = {m.fTop, m.fAscent, m.fDescent, m.fBottom, m.fLeading, m.fAvgCharWidth, m.fMaxCharWidth, m.fXMin, m.fXMax, m.fXHeight, m.fCapHeight, m.fUnderlineThickness, m.fUnderlinePosition, m.fStrikeoutThickness, m.fStrikeoutPosition, m.fFlags};
    return CFSK_OK;
}

cfsk_status cfsk_font_paths(const cfsk_font* font, const uint16_t* glyphs, size_t count, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(font && count <= INT_MAX && (!count || (glyphs && output)));
    for (size_t i = 0; i < count; ++i) output[i] = nullptr;
    for (size_t i = 0; i < count; ++i) {
        auto path = font->value.getPath(glyphs[i]);
        if (path) {
            output[i] = new (std::nothrow) cfsk_path{*path};
            if (!output[i]) {
                for (size_t j = 0; j < i; ++j) { delete output[j]; output[j] = nullptr; }
                return cfsk::fail(CFSK_OUT_OF_MEMORY, "glyph path allocation failed");
            }
        }
    }
    return CFSK_OK;
}

cfsk_status cfsk_text_blob_from_rsxform_glyphs(const uint16_t* glyphs, const cfsk_rsxform* transforms, size_t count, const cfsk_font* font, cfsk_text_blob** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(font && count <= INT_MAX && (!count || (glyphs && transforms)));
    std::vector<SkRSXform> values;
    for (size_t i = 0; i < count; ++i) {
        const auto& t = transforms[i];
        CFSK_REQUIRE(cfsk::finite(t.scos) && cfsk::finite(t.ssin) && cfsk::finite(t.translate_x) && cfsk::finite(t.translate_y));
        values.push_back(SkRSXform::Make(t.scos, t.ssin, t.translate_x, t.translate_y));
    }
    return cfsk::resource(output, SkTextBlob::MakeFromRSXform(glyphs, count * sizeof(uint16_t), SkSpan<const SkRSXform>(values), font->value, SkTextEncoding::kGlyphID));
}

cfsk_status cfsk_picture_recorder_make(cfsk_picture_recorder** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    *output = new (std::nothrow) cfsk_picture_recorder;
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "recorder allocation failed");
}

cfsk_status cfsk_picture_recorder_begin(cfsk_picture_recorder* recorder, cfsk_rect bounds, cfsk_canvas** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(bounds));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(recorder && output && !recorder->active);
    recorder->canvas.value = recorder->value.beginRecording(cfsk::rect(bounds));
    recorder->active = true;
    *output = &recorder->canvas;
    return CFSK_OK;
}

cfsk_status cfsk_picture_recorder_finish(cfsk_picture_recorder* recorder, cfsk_picture** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(recorder && recorder->active);
    auto result = recorder->value.finishRecordingAsPicture();
    recorder->active = false; recorder->canvas.value = nullptr;
    return cfsk::resource(output, std::move(result));
}

cfsk_status cfsk_picture_bytes_used(const cfsk_picture* picture, size_t* output_bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(picture && output_bytes);
    *output_bytes = picture->value->approximateBytesUsed();
    return CFSK_OK;
}

cfsk_status cfsk_picture_make_shader(const cfsk_picture* picture, uint32_t tile_x, uint32_t tile_y, uint32_t filter_mode, const float* local_matrix, const cfsk_rect* tile, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(picture && tile_x <= 3 && tile_y <= 3 && filter_mode <= 1 && (!local_matrix || cfsk::finite(local_matrix, 9)) && (!tile || cfsk::finite(*tile)));
    SkMatrix matrix = local_matrix ? cfsk::matrix(local_matrix) : SkMatrix::I();
    SkRect rect = tile ? cfsk::rect(*tile) : SkRect::MakeEmpty();
    return cfsk::resource(output, picture->value->makeShader(static_cast<SkTileMode>(tile_x), static_cast<SkTileMode>(tile_y), static_cast<SkFilterMode>(filter_mode), &matrix, tile ? &rect : nullptr));
}

cfsk_status cfsk_vertices_make(uint32_t mode, const cfsk_point* positions, const cfsk_point* texcoords, const uint32_t* colors, size_t vertex_count, const uint16_t* indices, size_t index_count, cfsk_vertices** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(mode <= 2 && vertex_count <= INT_MAX && index_count <= INT_MAX && (!vertex_count || positions) && (!index_count || indices));
    std::vector<SkPoint> points, uv;
    for (size_t i = 0; i < vertex_count; ++i) {
        CFSK_REQUIRE(cfsk::finite(positions[i]) && (!texcoords || cfsk::finite(texcoords[i])));
        points.push_back(cfsk::point(positions[i]));
        if (texcoords) uv.push_back(cfsk::point(texcoords[i]));
    }
    for (size_t i = 0; i < index_count; ++i) CFSK_REQUIRE(indices[i] < vertex_count);
    return cfsk::resource(output, SkVertices::MakeCopy(static_cast<SkVertices::VertexMode>(mode), static_cast<int>(vertex_count), points.data(), texcoords ? uv.data() : nullptr, colors, static_cast<int>(index_count), indices));
}

cfsk_status cfsk_vertices_unique_id(const cfsk_vertices* vertices, uint32_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(vertices && output);
    *output = vertices->value->uniqueID();
    return CFSK_OK;
}

cfsk_status cfsk_width_stroke_make(const float* rows, size_t float_count, const cfsk_width_stroke_options* options, cfsk_width_stroke** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(options && cfsk::finite(options->envelope) && cfsk::finite(options->path_length) && options->merge_joins <= 1 && options->subdivide <= 1 && options->batches <= 1 && (!float_count || cfsk::finite(rows, float_count)));
    SkWidthStroke::Options opts{options->envelope, options->path_length, static_cast<bool>(options->merge_joins), static_cast<SkWidthStroke::Batches>(options->batches), static_cast<bool>(options->subdivide)};
    return cfsk::resource(output, SkWidthStroke::Make(rows, float_count, opts));
}

cfsk_status cfsk_width_stroke_vertices(const cfsk_width_stroke* stroke, size_t chunk, cfsk_vertices** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(stroke && chunk < stroke->value->chunks().size());
    return cfsk::resource(output, stroke->value->vertices(chunk));
}

cfsk_status cfsk_width_stroke_bounds(const cfsk_width_stroke* stroke, cfsk_rect* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(stroke && output);
    *output = cfsk::rect(stroke->value->bounds());
    return CFSK_OK;
}

cfsk_status cfsk_width_stroke_vertex_count(const cfsk_width_stroke* stroke, size_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(stroke && output);
    *output = stroke->value->vertexCount();
    return CFSK_OK;
}

cfsk_status cfsk_width_stroke_chunks(const cfsk_width_stroke* stroke, cfsk_stroke_chunk* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(stroke);
    std::vector<cfsk_stroke_chunk> chunks;
    for (const auto& c : stroke->value->chunks()) chunks.push_back({c.range.start, c.range.count, c.batch});
    return cfsk::copy(chunks.data(), chunks.size(), sizeof(cfsk_stroke_chunk), output, capacity, required);
}

cfsk_status cfsk_width_stroke_batches(const cfsk_width_stroke* stroke, cfsk_stroke_range* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(stroke);
    std::vector<cfsk_stroke_range> ranges;
    for (const auto& r : stroke->value->batches()) ranges.push_back({r.start, r.count});
    return cfsk::copy(ranges.data(), ranges.size(), sizeof(cfsk_stroke_range), output, capacity, required);
}

cfsk_status cfsk_width_stroke_deposit_cuts(const cfsk_width_stroke* stroke, uint32_t* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(stroke);
    const auto& cuts = stroke->value->depositCuts();
    return cfsk::copy(cuts.data(), cuts.size(), sizeof(uint32_t), output, capacity, required);
}

cfsk_status cfsk_width_stroke_geometry(const cfsk_width_stroke* stroke, cfsk_point* positions, cfsk_point* uv, size_t vertex_capacity, uint32_t* indices, size_t index_capacity, size_t* required_vertices, size_t* required_indices) {
    CFSK_BEGIN;
    CFSK_REQUIRE(stroke && required_vertices && required_indices && (!vertex_capacity || (positions && uv)) && (!index_capacity || indices));
    const auto& p = stroke->value->positions(); const auto& t = stroke->value->texCoords(); const auto& idx = stroke->value->indices();
    *required_vertices = p.size(); *required_indices = idx.size();
    if (!positions && !uv && !indices && !vertex_capacity && !index_capacity) return CFSK_OK;
    if (vertex_capacity < p.size() || index_capacity < idx.size()) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "stroke geometry buffers too small");
    for (size_t i = 0; i < p.size(); ++i) { positions[i] = {p[i].x(), p[i].y()}; uv[i] = {t[i].x(), t[i].y()}; }
    if (!idx.empty()) std::memcpy(indices, idx.data(), idx.size() * sizeof(uint32_t));
    return CFSK_OK;
}
