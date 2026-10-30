/* C ABI validation and ownership stay at the boundary; Skia owns rendering. */
#include "cfsk_internal.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRSXform.h"
#include "include/codec/SkCodec.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkWebpEncoder.h"
#include "include/gpu/graphite/Surface.h"
namespace cfsk { thread_local std::string error; }

cfsk_status cfsk_abi_version(uint32_t* version) {
    CFSK_BEGIN;
    CFSK_REQUIRE(version);
    *version = CFSK_ABI_VERSION;
    return CFSK_OK;
}

cfsk_status cfsk_error_copy(char* output, size_t capacity, size_t* required) {
    if (!required || (!output && capacity)) return CFSK_INVALID_ARGUMENT;
    *required = cfsk::error.size();
    if (!output && !capacity) return CFSK_OK;
    if (capacity < *required) return CFSK_BUFFER_TOO_SMALL;
    if (*required) std::memcpy(output, cfsk::error.data(), *required);
    return CFSK_OK;
}

cfsk_status cfsk_surface_make_raster(const cfsk_image_info* info, cfsk_surface** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::valid(info));
    return cfsk::resource(output, SkSurfaces::Raster(cfsk::image_info(*info)));
}

cfsk_status cfsk_surface_get_canvas(cfsk_surface* surface, cfsk_canvas** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(surface && output);
    surface->canvas.value = surface->value->getCanvas();
    *output = &surface->canvas;
    return CFSK_OK;
}

cfsk_status cfsk_surface_info(const cfsk_surface* surface, cfsk_image_info* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(surface && output);
    *output = cfsk::image_info(surface->value->imageInfo(), const_cast<cfsk_surface*>(surface)->space);
    return CFSK_OK;
}

cfsk_status cfsk_surface_is_gpu(const cfsk_surface* surface, uint8_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(surface && output);
    *output = surface->value->recorder() != nullptr;
    return CFSK_OK;
}

cfsk_status cfsk_surface_snapshot(cfsk_surface* surface, const cfsk_irect* subset, uint8_t mipmapped, cfsk_image** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(mipmapped <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(surface);
    if (surface->value->recorder()) {
        SkIRect bounds;
        if (subset) bounds = cfsk::irect(*subset);
        auto status = cfsk::resource(output, SkSurfaces::AsImageCopy(surface->value, subset ? &bounds : nullptr, static_cast<skgpu::Mipmapped>(mipmapped)));
        if (status == CFSK_OK) (*output)->gpu_context = surface->gpu_context;
        return status;
    }
    if (mipmapped) return cfsk::unsupported("CPU raster snapshots do not have mipmaps");
    return cfsk::resource(output, subset ? surface->value->makeImageSnapshot(cfsk::irect(*subset)) : surface->value->makeImageSnapshot());
}

cfsk_status cfsk_surface_read_pixels(cfsk_surface* surface, const cfsk_image_info* info, int32_t source_x, int32_t source_y, void* output, size_t output_bytes, size_t row_bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(surface && cfsk::valid(info) && output);
    if (surface->value->recorder()) return cfsk::unsupported("GPU pixels require asynchronous readback");
    auto target = cfsk::image_info(*info);
    CFSK_REQUIRE(cfsk::extent(target, row_bytes, output_bytes));
    if (!surface->value->readPixels(target, output, row_bytes, source_x, source_y))
        return cfsk::fail(CFSK_FAILED, "surface readPixels failed");
    return CFSK_OK;
}

cfsk_status cfsk_canvas_clear(cfsk_canvas* canvas, cfsk_color color, const cfsk_color_space* space) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(color));
    CFSK_REQUIRE(canvas);
    SkPaint paint;
    paint.setColor4f(cfsk::color(color), space ? space->value.get() : nullptr);
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas->value->drawPaint(paint);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_save(cfsk_canvas* canvas, int32_t* save_count) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && save_count);
    *save_count = canvas->value->save();
    return CFSK_OK;
}

cfsk_status cfsk_canvas_get_save_count(cfsk_canvas* canvas, int32_t* save_count) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && save_count);
    *save_count = canvas->value->getSaveCount();
    return CFSK_OK;
}

cfsk_status cfsk_canvas_restore(cfsk_canvas* canvas) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && canvas->value->getSaveCount() > 1);
    canvas->value->restore();
    return CFSK_OK;
}

cfsk_status cfsk_canvas_restore_to_count(cfsk_canvas* canvas, int32_t count) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && count >= 1 && count <= canvas->value->getSaveCount());
    canvas->value->restoreToCount(count);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_concat(cfsk_canvas* canvas, const float matrix[9]) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && cfsk::finite(matrix, 9));
    canvas->value->concat(cfsk::matrix(matrix));
    return CFSK_OK;
}

cfsk_status cfsk_canvas_concat44(cfsk_canvas* canvas, const float matrix[16]) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && cfsk::finite(matrix, 16));
    canvas->value->concat(SkM44::RowMajor(matrix));
    return CFSK_OK;
}

cfsk_status cfsk_canvas_get_matrix(cfsk_canvas* canvas, float matrix[9]) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && matrix);
    canvas->value->getTotalMatrix().get9(matrix);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_get_matrix44(cfsk_canvas* canvas, float matrix[16]) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && matrix);
    canvas->value->getLocalToDevice().getRowMajor(matrix);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_get_device_clip_bounds(cfsk_canvas* canvas, cfsk_irect* bounds) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && bounds);
    auto r = canvas->value->getDeviceClipBounds();
    *bounds = {r.left(), r.top(), r.right(), r.bottom()};
    return CFSK_OK;
}

cfsk_status cfsk_canvas_translate(cfsk_canvas* canvas, float dx, float dy) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(dx));
    CFSK_REQUIRE(cfsk::finite(dy));
    CFSK_REQUIRE(canvas);
    canvas->value->translate(dx, dy);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_scale(cfsk_canvas* canvas, float scale_x, float scale_y) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(scale_x));
    CFSK_REQUIRE(cfsk::finite(scale_y));
    CFSK_REQUIRE(canvas);
    canvas->value->scale(scale_x, scale_y);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_skew(cfsk_canvas* canvas, float skew_x, float skew_y) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(skew_x));
    CFSK_REQUIRE(cfsk::finite(skew_y));
    CFSK_REQUIRE(canvas);
    canvas->value->skew(skew_x, skew_y);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_rotate(cfsk_canvas* canvas, float degrees, float pivot_x, float pivot_y) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(degrees));
    CFSK_REQUIRE(cfsk::finite(pivot_x));
    CFSK_REQUIRE(cfsk::finite(pivot_y));
    CFSK_REQUIRE(canvas);
    canvas->value->rotate(degrees, pivot_x, pivot_y);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_save_layer(cfsk_canvas* canvas, const cfsk_layer* layer, int32_t* save_count) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && layer && save_count && layer->backdrop_tile_mode <= 3);
    CFSK_REQUIRE((layer->flags & ~(CFSK_LAYER_INIT_WITH_PREVIOUS | CFSK_LAYER_F16)) == 0);
    SkRect bounds;
    if (layer->bounds) { CFSK_REQUIRE(cfsk::finite(*layer->bounds)); bounds = cfsk::rect(*layer->bounds); }
    SkCanvas::SaveLayerRec rec(layer->bounds ? &bounds : nullptr, layer->paint ? &layer->paint->value : nullptr,
                             layer->backdrop ? layer->backdrop->value.get() : nullptr, layer->flags);
    rec.fBackdropTileMode = static_cast<SkTileMode>(layer->backdrop_tile_mode);
    rec.fColorSpace = layer->color_space ? layer->color_space->value.get() : nullptr;
    *save_count = canvas->value->saveLayer(rec);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_clip_rect(cfsk_canvas* canvas, cfsk_rect shape, uint32_t op, uint8_t antialias) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(shape));
    CFSK_REQUIRE(antialias <= 1);
    CFSK_REQUIRE(canvas && op <= 1);
    canvas->value->clipRect(cfsk::rect(shape), static_cast<SkClipOp>(op), antialias);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_clip_rrect(cfsk_canvas* canvas, const cfsk_rrect* shape, uint32_t op, uint8_t antialias) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(antialias <= 1);
    CFSK_REQUIRE(canvas && cfsk::valid(shape) && op <= 1);
    canvas->value->clipRRect(cfsk::rrect(*shape), static_cast<SkClipOp>(op), antialias);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_clip_path(cfsk_canvas* canvas, const cfsk_path* shape, uint32_t op, uint8_t antialias) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(antialias <= 1);
    CFSK_REQUIRE(canvas && shape && op <= 1);
    canvas->value->clipPath(shape->value, static_cast<SkClipOp>(op), antialias);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_rect(cfsk_canvas* canvas, cfsk_rect rect, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(rect));
    CFSK_REQUIRE(canvas && paint);
    canvas->value->drawRect(cfsk::rect(rect), paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_oval(cfsk_canvas* canvas, cfsk_rect rect, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(rect));
    CFSK_REQUIRE(canvas && paint);
    canvas->value->drawOval(cfsk::rect(rect), paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_rrect(cfsk_canvas* canvas, const cfsk_rrect* rect, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && paint && cfsk::valid(rect));
    canvas->value->drawRRect(cfsk::rrect(*rect), paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_path(cfsk_canvas* canvas, const cfsk_path* path, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && paint && path);
    canvas->value->drawPath(path->value, paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_line(cfsk_canvas* canvas, cfsk_point start, cfsk_point end, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(start));
    CFSK_REQUIRE(cfsk::finite(end));
    CFSK_REQUIRE(canvas && paint);
    canvas->value->drawLine(cfsk::point(start), cfsk::point(end), paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_paint(cfsk_canvas* canvas, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && paint);
    canvas->value->drawPaint(paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_circle(cfsk_canvas* canvas, float x, float y, float radius, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(cfsk::finite(radius));
    CFSK_REQUIRE(canvas && paint && radius >= 0);
    canvas->value->drawCircle(x, y, radius, paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_text_blob(cfsk_canvas* canvas, const cfsk_text_blob* blob, float x, float y, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(canvas && paint && blob);
    canvas->value->drawTextBlob(blob->value, x, y, paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_text(cfsk_canvas* canvas, cfsk_string text, float x, float y, const cfsk_font* font, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::valid(text));
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(canvas && paint && font);
    canvas->value->drawSimpleText(text.data, text.length, SkTextEncoding::kUTF8, x, y, font->value, paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_vertices(cfsk_canvas* canvas, const cfsk_vertices* vertices, uint32_t blend_mode, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && paint && vertices && blend_mode <= 28);
    canvas->value->drawVertices(vertices->value, static_cast<SkBlendMode>(blend_mode), paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_picture(cfsk_canvas* canvas, const cfsk_picture* picture, const float* matrix, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && picture && (!matrix || cfsk::finite(matrix, 9)));
    SkMatrix m = matrix ? cfsk::matrix(matrix) : SkMatrix::I();
    canvas->value->drawPicture(picture->value, &m, paint ? &paint->value : nullptr);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_image(cfsk_canvas* canvas, const cfsk_image* image, float x, float y, cfsk_sampling sampling, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(canvas && image && cfsk::valid(sampling));
    canvas->value->drawImage(image->value, x, y, cfsk::sampling(sampling), paint ? &paint->value : nullptr);
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_image_rect(cfsk_canvas* canvas, const cfsk_image* image, cfsk_rect source, cfsk_rect destination, cfsk_sampling sampling, const cfsk_paint* paint, uint32_t constraint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(source));
    CFSK_REQUIRE(cfsk::finite(destination));
    CFSK_REQUIRE(canvas && image && cfsk::valid(sampling) && constraint <= 1);
    canvas->value->drawImageRect(image->value, cfsk::rect(source), cfsk::rect(destination), cfsk::sampling(sampling), paint ? &paint->value : nullptr, static_cast<SkCanvas::SrcRectConstraint>(constraint));
    return CFSK_OK;
}

cfsk_status cfsk_canvas_draw_glyphs(cfsk_canvas* canvas, const uint16_t* glyphs, const cfsk_point* positions, size_t count, cfsk_point origin, const cfsk_font* font, const cfsk_paint* paint) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::finite(origin));
    CFSK_REQUIRE(canvas && font && paint && count <= INT_MAX && (!count || (glyphs && positions)));
    std::vector<SkPoint> points;
    for (size_t i = 0; i < count; ++i) { CFSK_REQUIRE(cfsk::finite(positions[i])); points.push_back(cfsk::point(positions[i])); }
    canvas->value->drawGlyphs(SkSpan<const SkGlyphID>(glyphs, count), SkSpan<const SkPoint>(points.data(), count), cfsk::point(origin), font->value, paint->value);
    return CFSK_OK;
}

cfsk_status cfsk_paint_make(cfsk_paint** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::owned(output, SkPaint());
}

cfsk_status cfsk_paint_copy(const cfsk_paint* paint, cfsk_paint** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(paint);
    return cfsk::owned(output, paint->value);
}

cfsk_status cfsk_paint_set_color(cfsk_paint* paint, cfsk_color color, const cfsk_color_space* space) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(color));
    CFSK_REQUIRE(paint);
    paint->value.setColor4f(cfsk::color(color), space ? space->value.get() : nullptr);
    return CFSK_OK;
}

cfsk_status cfsk_paint_get_color(const cfsk_paint* paint, cfsk_color* color) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint && color);
    auto c = paint->value.getColor4f();
    *color = {c.fR, c.fG, c.fB, c.fA};
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_alpha(cfsk_paint* paint, float value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(value));
    CFSK_REQUIRE(paint && (value >= 0 && value <= 1));
    paint->value.setAlphaf(value);
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_antialias(cfsk_paint* paint, uint8_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(value <= 1);
    CFSK_REQUIRE(paint && (true));
    paint->value.setAntiAlias(value);
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_dither(cfsk_paint* paint, uint8_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(value <= 1);
    CFSK_REQUIRE(paint && (true));
    paint->value.setDither(value);
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_blend_mode(cfsk_paint* paint, uint32_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint && (value <= 28));
    paint->value.setBlendMode(static_cast<SkBlendMode>(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_stroke_cap(cfsk_paint* paint, uint32_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint && (value <= 2));
    paint->value.setStrokeCap(static_cast<SkPaint::Cap>(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_stroke_join(cfsk_paint* paint, uint32_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint && (value <= 2));
    paint->value.setStrokeJoin(static_cast<SkPaint::Join>(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_stroke_miter(cfsk_paint* paint, float value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(value));
    CFSK_REQUIRE(paint && (value >= 0));
    paint->value.setStrokeMiter(value);
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_stroke_width(cfsk_paint* paint, float value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(value));
    CFSK_REQUIRE(paint && (value >= 0));
    paint->value.setStrokeWidth(value);
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_style(cfsk_paint* paint, uint32_t value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint && (value <= 2));
    paint->value.setStyle(static_cast<SkPaint::Style>(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_blender(cfsk_paint* paint, const cfsk_blender* value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint);
    paint->value.setBlender(cfsk::ref(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_color_filter(cfsk_paint* paint, const cfsk_color_filter* value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint);
    paint->value.setColorFilter(cfsk::ref(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_image_filter(cfsk_paint* paint, const cfsk_image_filter* value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint);
    paint->value.setImageFilter(cfsk::ref(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_mask_filter(cfsk_paint* paint, const cfsk_mask_filter* value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint);
    paint->value.setMaskFilter(cfsk::ref(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_path_effect(cfsk_paint* paint, const cfsk_path_effect* value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint);
    paint->value.setPathEffect(cfsk::ref(value));
    return CFSK_OK;
}

cfsk_status cfsk_paint_set_shader(cfsk_paint* paint, const cfsk_shader* value) {
    CFSK_BEGIN;
    CFSK_REQUIRE(paint);
    paint->value.setShader(cfsk::ref(value));
    return CFSK_OK;
}

cfsk_status cfsk_image_decode(cfsk_bytes encoded, cfsk_image** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(encoded));
    CFSK_OUTPUT(output);
    auto image = SkImages::DeferredFromEncodedData(SkData::MakeWithCopy(encoded.data, encoded.length));
    if (!image) return cfsk::fail(CFSK_FAILED, "encoded image decode failed");
    return cfsk::resource(output, image->makeRasterImage());
}

cfsk_status cfsk_image_from_pixels(const cfsk_image_info* info, cfsk_bytes pixels, size_t row_bytes, cfsk_image** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(pixels));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::valid(info));
    auto target = cfsk::image_info(*info);
    CFSK_REQUIRE(cfsk::extent(target, row_bytes, pixels.length) && pixels.data);
    return cfsk::resource(output, SkImages::RasterFromPixmapCopy(SkPixmap(target, pixels.data, row_bytes)));
}

cfsk_status cfsk_image_get_info(const cfsk_image* image, cfsk_image_info* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(image && output);
    *output = cfsk::image_info(image->value->imageInfo(), const_cast<cfsk_image*>(image)->space);
    return CFSK_OK;
}

cfsk_status cfsk_image_has_mipmaps(const cfsk_image* image, uint8_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(image && output);
    *output = image->value->hasMipmaps();
    return CFSK_OK;
}

cfsk_status cfsk_image_texture_size(const cfsk_image* image, size_t* output_bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(image && output_bytes);
    *output_bytes = image->value->textureSize();
    return CFSK_OK;
}

cfsk_status cfsk_image_read_pixels(const cfsk_image* image, const cfsk_image_info* info, int32_t source_x, int32_t source_y, void* output, size_t output_bytes, size_t row_bytes) {
    CFSK_BEGIN;
    CFSK_REQUIRE(image && cfsk::valid(info) && output);
    if (image->value->isTextureBacked()) return cfsk::unsupported("GPU images require asynchronous readback");
    auto target = cfsk::image_info(*info);
    CFSK_REQUIRE(cfsk::extent(target, row_bytes, output_bytes));
    if (!image->value->readPixels(target, output, row_bytes, source_x, source_y)) return cfsk::fail(CFSK_FAILED, "image readPixels failed");
    return CFSK_OK;
}

cfsk_status cfsk_image_encode(const cfsk_image* image, uint32_t format, int32_t quality, cfsk_data** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(image && quality >= 0 && quality <= 100);
    if (image->value->isTextureBacked()) return cfsk::unsupported("GPU encoding requires explicit readback");
    SkPixmap pixels;
    auto raster = image->value->makeRasterImage();
    if (!raster || !raster->peekPixels(&pixels)) return cfsk::fail(CFSK_FAILED, "image pixels unavailable");
    sk_sp<SkData> data;
    switch (format) {
    case CFSK_ENCODE_PNG: data = SkPngEncoder::Encode(pixels, {}); break;
    case CFSK_ENCODE_JPEG: { SkJpegEncoder::Options options; options.fQuality = quality; data = SkJpegEncoder::Encode(pixels, options); break; }
    case CFSK_ENCODE_WEBP: { SkWebpEncoder::Options options; options.fQuality = quality; data = SkWebpEncoder::Encode(pixels, options); break; }
    default: return cfsk::fail(CFSK_INVALID_ARGUMENT, "invalid encode format");
    }
    return cfsk::resource(output, std::move(data));
}

cfsk_status cfsk_image_make_shader(const cfsk_image* image, uint32_t tile_x, uint32_t tile_y, cfsk_sampling sampling, const float* local_matrix, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(image && tile_x <= 3 && tile_y <= 3 && cfsk::valid(sampling) && (!local_matrix || cfsk::finite(local_matrix, 9)));
    SkMatrix m = local_matrix ? cfsk::matrix(local_matrix) : SkMatrix::I();
    return cfsk::resource(output, image->value->makeShader(static_cast<SkTileMode>(tile_x), static_cast<SkTileMode>(tile_y), cfsk::sampling(sampling), &m));
}

cfsk_status cfsk_data_copy(const cfsk_data* data, uint8_t* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(data);
    return cfsk::copy(data->value->data(), data->value->size(), 1, output, capacity, required);
}

cfsk_status cfsk_canvas_read_pixels(cfsk_canvas* canvas, const cfsk_image_info* info, int32_t source_x, int32_t source_y, void* output, size_t output_bytes, size_t row_bytes) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && cfsk::valid(info) && output);
    if (canvas->value->recorder()) return cfsk::unsupported("GPU canvas pixels require asynchronous readback");
    auto target = cfsk::image_info(*info);
    CFSK_REQUIRE(cfsk::extent(target, row_bytes, output_bytes));
    if (!canvas->value->readPixels(target, output, row_bytes, source_x, source_y)) return cfsk::fail(CFSK_FAILED, "canvas readPixels failed");
    return CFSK_OK;
}

cfsk_status cfsk_canvas_write_pixels(cfsk_canvas* canvas, const cfsk_image_info* info, cfsk_bytes pixels, size_t row_bytes, int32_t x, int32_t y) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(cfsk::valid(pixels));
    CFSK_REQUIRE(canvas && cfsk::valid(info) && pixels.data);
    auto source = cfsk::image_info(*info);
    CFSK_REQUIRE(cfsk::extent(source, row_bytes, pixels.length));
    if (!canvas->value->writePixels(source, pixels.data, row_bytes, x, y)) return cfsk::fail(CFSK_FAILED, "canvas writePixels failed");
    return CFSK_OK;
}

void cfsk_surface_destroy(cfsk_surface* value) {
    delete value;
}

void cfsk_paint_destroy(cfsk_paint* value) {
    delete value;
}

void cfsk_path_destroy(cfsk_path* value) {
    delete value;
}

void cfsk_path_builder_destroy(cfsk_path_builder* value) {
    delete value;
}

void cfsk_image_destroy(cfsk_image* value) {
    delete value;
}

void cfsk_shader_destroy(cfsk_shader* value) {
    delete value;
}

void cfsk_runtime_effect_destroy(cfsk_runtime_effect* value) {
    delete value;
}

void cfsk_runtime_effect_builder_destroy(cfsk_runtime_effect_builder* value) {
    delete value;
}

void cfsk_blender_destroy(cfsk_blender* value) {
    delete value;
}

void cfsk_color_filter_destroy(cfsk_color_filter* value) {
    delete value;
}

void cfsk_image_filter_destroy(cfsk_image_filter* value) {
    delete value;
}

void cfsk_mask_filter_destroy(cfsk_mask_filter* value) {
    delete value;
}

void cfsk_path_effect_destroy(cfsk_path_effect* value) {
    delete value;
}

void cfsk_picture_destroy(cfsk_picture* value) {
    delete value;
}

void cfsk_picture_recorder_destroy(cfsk_picture_recorder* value) {
    delete value;
}

void cfsk_font_destroy(cfsk_font* value) {
    delete value;
}

void cfsk_typeface_destroy(cfsk_typeface* value) {
    delete value;
}

void cfsk_text_blob_destroy(cfsk_text_blob* value) {
    delete value;
}

void cfsk_vertices_destroy(cfsk_vertices* value) {
    delete value;
}

void cfsk_contour_iter_destroy(cfsk_contour_iter* value) {
    delete value;
}

void cfsk_contour_destroy(cfsk_contour* value) {
    delete value;
}

void cfsk_width_stroke_destroy(cfsk_width_stroke* value) {
    delete value;
}

void cfsk_color_space_destroy(cfsk_color_space* value) {
    delete value;
}

void cfsk_data_destroy(cfsk_data* value) {
    delete value;
}
