/* C ABI validation and ownership stay at the boundary; Skia owns rendering. */
#include "cfsk_internal.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkStrokeRec.h"
#include "include/core/SkString.h"
#include "include/pathops/SkPathOps.h"
#include "include/utils/SkParsePath.h"
#include "include/utils/SkPathOffset.h"

cfsk_status cfsk_path_builder_make(const cfsk_path* source, cfsk_path_builder** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    return cfsk::owned(output, source ? SkPathBuilder(source->value) : SkPathBuilder());
}

cfsk_status cfsk_path_builder_snapshot(const cfsk_path_builder* builder, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(builder);
    return cfsk::owned(output, builder->value.snapshot());
}

cfsk_status cfsk_path_builder_detach(cfsk_path_builder* builder, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(builder);
    return cfsk::owned(output, builder->value.detach());
}

cfsk_status cfsk_path_builder_reset(cfsk_path_builder* builder) {
    CFSK_BEGIN;
    CFSK_REQUIRE(builder);
    builder->value.reset();
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_move_to(cfsk_path_builder* builder, float x, float y) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(builder);
    builder->value.moveTo(x, y);
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_line_to(cfsk_path_builder* builder, float x, float y) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(builder);
    builder->value.lineTo(x, y);
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_quad_to(cfsk_path_builder* builder, float x1, float y1, float x2, float y2) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x1));
    CFSK_REQUIRE(cfsk::finite(y1));
    CFSK_REQUIRE(cfsk::finite(x2));
    CFSK_REQUIRE(cfsk::finite(y2));
    CFSK_REQUIRE(builder);
    builder->value.quadTo(x1, y1, x2, y2);
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_conic_to(cfsk_path_builder* builder, float x1, float y1, float x2, float y2, float weight) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x1));
    CFSK_REQUIRE(cfsk::finite(y1));
    CFSK_REQUIRE(cfsk::finite(x2));
    CFSK_REQUIRE(cfsk::finite(y2));
    CFSK_REQUIRE(cfsk::finite(weight));
    CFSK_REQUIRE(builder && weight >= 0);
    builder->value.conicTo(x1, y1, x2, y2, weight);
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_cubic_to(cfsk_path_builder* builder, float x1, float y1, float x2, float y2, float x3, float y3) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x1));
    CFSK_REQUIRE(cfsk::finite(y1));
    CFSK_REQUIRE(cfsk::finite(x2));
    CFSK_REQUIRE(cfsk::finite(y2));
    CFSK_REQUIRE(cfsk::finite(x3));
    CFSK_REQUIRE(cfsk::finite(y3));
    CFSK_REQUIRE(builder);
    builder->value.cubicTo(x1, y1, x2, y2, x3, y3);
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_close(cfsk_path_builder* builder) {
    CFSK_BEGIN;
    CFSK_REQUIRE(builder);
    builder->value.close();
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_add_rect(cfsk_path_builder* builder, cfsk_rect rect, uint32_t direction) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(rect));
    CFSK_REQUIRE(builder && direction <= 1);
    builder->value.addRect(cfsk::rect(rect), static_cast<SkPathDirection>(direction));
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_add_rrect(cfsk_path_builder* builder, const cfsk_rrect* rect, uint32_t direction) {
    CFSK_BEGIN;
    CFSK_REQUIRE(builder && cfsk::valid(rect) && direction <= 1);
    builder->value.addRRect(cfsk::rrect(*rect), static_cast<SkPathDirection>(direction));
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_add_circle(cfsk_path_builder* builder, float x, float y, float radius, uint32_t direction) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(cfsk::finite(radius));
    CFSK_REQUIRE(builder && radius >= 0 && direction <= 1);
    builder->value.addCircle(x, y, radius, static_cast<SkPathDirection>(direction));
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_add_path(cfsk_path_builder* builder, const cfsk_path* path, const float matrix[9], uint32_t mode) {
    CFSK_BEGIN;
    CFSK_REQUIRE(builder && path && cfsk::finite(matrix, 9) && mode <= 1);
    builder->value.addPath(path->value, cfsk::matrix(matrix), static_cast<SkPath::AddPathMode>(mode));
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_arc_to_oval(cfsk_path_builder* builder, cfsk_rect oval, float start_degrees, float sweep_degrees, uint8_t force_move) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(oval));
    CFSK_REQUIRE(cfsk::finite(start_degrees));
    CFSK_REQUIRE(cfsk::finite(sweep_degrees));
    CFSK_REQUIRE(force_move <= 1);
    CFSK_REQUIRE(builder);
    builder->value.arcTo(cfsk::rect(oval), start_degrees, sweep_degrees, force_move);
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_arc_to_rotated(cfsk_path_builder* builder, float radius_x, float radius_y, float rotation_degrees, uint8_t large_arc, uint32_t sweep_direction, float x, float y) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(radius_x));
    CFSK_REQUIRE(cfsk::finite(radius_y));
    CFSK_REQUIRE(cfsk::finite(rotation_degrees));
    CFSK_REQUIRE(large_arc <= 1);
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(builder && radius_x >= 0 && radius_y >= 0 && sweep_direction <= 1);
    builder->value.arcTo({radius_x, radius_y}, rotation_degrees, static_cast<SkPathBuilder::ArcSize>(large_arc), static_cast<SkPathDirection>(sweep_direction), {x, y});
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_arc_to_tangent(cfsk_path_builder* builder, float x1, float y1, float x2, float y2, float radius) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x1));
    CFSK_REQUIRE(cfsk::finite(y1));
    CFSK_REQUIRE(cfsk::finite(x2));
    CFSK_REQUIRE(cfsk::finite(y2));
    CFSK_REQUIRE(cfsk::finite(radius));
    CFSK_REQUIRE(builder && radius >= 0);
    builder->value.arcTo({x1, y1}, {x2, y2}, radius);
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_set_fill_type(cfsk_path_builder* builder, uint32_t fill_type) {
    CFSK_BEGIN;
    CFSK_REQUIRE(builder && fill_type <= 3);
    builder->value.setFillType(static_cast<SkPathFillType>(fill_type));
    return CFSK_OK;
}

cfsk_status cfsk_path_builder_transform(cfsk_path_builder* builder, const float matrix[9]) {
    CFSK_BEGIN;
    CFSK_REQUIRE(builder && cfsk::finite(matrix, 9));
    builder->value.transform(cfsk::matrix(matrix));
    return CFSK_OK;
}

cfsk_status cfsk_path_copy(const cfsk_path* path, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path);
    return cfsk::owned(output, path->value);
}

cfsk_status cfsk_path_transform(const cfsk_path* path, const float matrix[9], cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path && cfsk::finite(matrix, 9));
    auto result = path->value.tryMakeTransform(cfsk::matrix(matrix));
    if (!result) return cfsk::fail(CFSK_FAILED, "path transform failed");
    return cfsk::owned(output, *result);
}

cfsk_status cfsk_path_bounds(const cfsk_path* path, cfsk_rect* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(path && output);
    *output = cfsk::rect(path->value.getBounds());
    return CFSK_OK;
}

cfsk_status cfsk_path_fill_type(const cfsk_path* path, uint32_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(path && output);
    *output = static_cast<uint32_t>(path->value.getFillType());
    return CFSK_OK;
}

cfsk_status cfsk_path_point_count(const cfsk_path* path, size_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(path && output);
    *output = path->value.countPoints();
    return CFSK_OK;
}

cfsk_status cfsk_path_is_empty(const cfsk_path* path, uint8_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(path && output);
    *output = path->value.isEmpty();
    return CFSK_OK;
}

cfsk_status cfsk_path_contains(const cfsk_path* path, float x, float y, uint8_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(x));
    CFSK_REQUIRE(cfsk::finite(y));
    CFSK_REQUIRE(path && output);
    *output = path->value.contains(x, y);
    return CFSK_OK;
}

cfsk_status cfsk_path_get_point(const cfsk_path* path, size_t index, cfsk_point* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(path && output && index < static_cast<size_t>(path->value.countPoints()));
    auto p = path->value.getPoint(static_cast<int>(index));
    *output = {p.x(), p.y()};
    return CFSK_OK;
}

cfsk_status cfsk_path_from_svg(cfsk_string svg, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(svg));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(!svg.length || !std::memchr(svg.data, 0, svg.length));
    std::string text(svg.data ? svg.data : "", svg.length);
    auto path = SkParsePath::FromSVGString(text.c_str());
    if (!path) return cfsk::fail(CFSK_INVALID_ARGUMENT, "invalid SVG path");
    return cfsk::owned(output, *path);
}

cfsk_status cfsk_path_from_commands(const float* commands, size_t count, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(!count || commands);
    SkPathBuilder builder;
    size_t i = 0;
    while (i < count) {
        float verb = commands[i++];
        CFSK_REQUIRE(verb >= 0 && verb <= 5 && std::floor(verb) == verb);
        static constexpr size_t sizes[] = {2, 2, 4, 5, 6, 0};
        size_t n = sizes[static_cast<int>(verb)];
        CFSK_REQUIRE(n <= count - i && (!n || cfsk::finite(commands + i, n)));
        const float* p = commands + i;
        switch (static_cast<int>(verb)) {
        case 0: builder.moveTo(p[0], p[1]); break;
        case 1: builder.lineTo(p[0], p[1]); break;
        case 2: builder.quadTo(p[0], p[1], p[2], p[3]); break;
        case 3: CFSK_REQUIRE(p[4] >= 0); builder.conicTo(p[0], p[1], p[2], p[3], p[4]); break;
        case 4: builder.cubicTo(p[0], p[1], p[2], p[3], p[4], p[5]); break;
        case 5: builder.close(); break;
        }
        i += n;
    }
    return cfsk::owned(output, builder.detach());
}

cfsk_status cfsk_path_to_commands(const cfsk_path* path, float* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(path);
    SkPath::RawIter iter(path->value);
    SkPoint points[4];
    std::vector<float> commands;
    for (SkPath::Verb verb; (verb = iter.next(points)) != SkPath::kDone_Verb;) {
        commands.push_back(static_cast<float>(verb));
        size_t begin = verb == SkPath::kMove_Verb ? 0 : 1;
        size_t end = verb == SkPath::kMove_Verb ? 1 : verb == SkPath::kLine_Verb ? 2 : verb == SkPath::kCubic_Verb ? 4 : verb == SkPath::kClose_Verb ? 0 : 3;
        for (size_t i = begin; i < end; ++i) { commands.push_back(points[i].x()); commands.push_back(points[i].y()); }
        if (verb == SkPath::kConic_Verb) commands.push_back(iter.conicWeight());
    }
    return cfsk::copy(commands.data(), commands.size(), sizeof(float), output, capacity, required);
}

cfsk_status cfsk_path_to_svg(const cfsk_path* path, cfsk_data** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path);
    auto text = SkParsePath::ToSVGString(path->value);
    return cfsk::resource(output, SkData::MakeWithCopy(text.c_str(), text.size()));
}

cfsk_status cfsk_path_op(const cfsk_path* first, const cfsk_path* second, uint32_t op, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(first && second && op <= 4);
    auto result = Op(first->value, second->value, static_cast<SkPathOp>(op));
    if (!result) return cfsk::fail(CFSK_FAILED, "path operation failed");
    return cfsk::owned(output, *result);
}

cfsk_status cfsk_path_simplify(const cfsk_path* path, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path);
    auto result = Simplify(path->value);
    if (!result) return cfsk::fail(CFSK_FAILED, "path simplify failed");
    return cfsk::owned(output, *result);
}

cfsk_status cfsk_path_stroke(const cfsk_path* path, float width, uint32_t join, uint32_t cap, float miter_limit, float resolution_scale, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(width));
    CFSK_REQUIRE(cfsk::finite(miter_limit));
    CFSK_REQUIRE(cfsk::finite(resolution_scale));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path && width > 0 && join <= 2 && cap <= 2 && miter_limit >= 0 && resolution_scale > 0);
    SkPaint paint; paint.setStyle(SkPaint::kStroke_Style); paint.setStrokeWidth(width);
    paint.setStrokeJoin(static_cast<SkPaint::Join>(join)); paint.setStrokeCap(static_cast<SkPaint::Cap>(cap)); paint.setStrokeMiter(miter_limit);
    SkPathBuilder builder;
    if (!skpathutils::FillPathWithPaint(path->value, paint, &builder, nullptr, SkMatrix::Scale(resolution_scale, resolution_scale)))
        return cfsk::fail(CFSK_FAILED, "path stroke failed");
    return cfsk::owned(output, builder.detach());
}

cfsk_status cfsk_path_offset(const cfsk_path* path, float distance, uint32_t join, float miter_limit, float tolerance, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(distance));
    CFSK_REQUIRE(cfsk::finite(miter_limit));
    CFSK_REQUIRE(cfsk::finite(tolerance));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path && join <= 2 && miter_limit >= 0 && tolerance > 0);
    SkPath result;
    if (!SkPathOffset(path->value, distance, static_cast<SkPathOffsetJoin>(join), miter_limit, tolerance, &result))
        return cfsk::fail(CFSK_FAILED, "path offset failed");
    return cfsk::owned(output, result);
}

cfsk_status cfsk_path_with_effect(const cfsk_path* path, const cfsk_path_effect* effect, uint32_t style, float resolution_scale, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(resolution_scale));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path && effect && style <= 2 && resolution_scale > 0);
    SkPaint paint; paint.setStyle(static_cast<SkPaint::Style>(style));
    SkStrokeRec stroke(paint, resolution_scale);
    SkPathBuilder builder;
    if (!effect->value->filterPath(&builder, path->value, &stroke)) return cfsk::fail(CFSK_FAILED, "path effect refused path");
    return cfsk::owned(output, builder.detach());
}

cfsk_status cfsk_contour_iter_make(const cfsk_path* path, uint8_t force_closed, float resolution_scale, cfsk_contour_iter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(force_closed <= 1);
    CFSK_REQUIRE(cfsk::finite(resolution_scale));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path && resolution_scale > 0);
    return cfsk::owned(output, SkContourMeasureIter(path->value, force_closed, resolution_scale));
}

cfsk_status cfsk_contour_iter_next(cfsk_contour_iter* iterator, cfsk_contour** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(iterator);
    auto next = iterator->value.next();
    return next ? cfsk::resource(output, std::move(next)) : CFSK_OK;
}

cfsk_status cfsk_contour_length(const cfsk_contour* contour, float* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(contour && output);
    *output = contour->value->length();
    return CFSK_OK;
}

cfsk_status cfsk_contour_is_closed(const cfsk_contour* contour, uint8_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(contour && output);
    *output = contour->value->isClosed();
    return CFSK_OK;
}

cfsk_status cfsk_contour_pos_tan(const cfsk_contour* contour, float distance, cfsk_point* position, cfsk_point* tangent) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(distance));
    CFSK_REQUIRE(contour && position && tangent);
    SkPoint p; SkVector t;
    if (!contour->value->getPosTan(distance, &p, &t)) return cfsk::fail(CFSK_FAILED, "contour has no tangent");
    *position = {p.x(), p.y()}; *tangent = {t.x(), t.y()};
    return CFSK_OK;
}

cfsk_status cfsk_contour_segment(const cfsk_contour* contour, float start, float stop, uint8_t start_with_move, cfsk_path** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(start));
    CFSK_REQUIRE(cfsk::finite(stop));
    CFSK_REQUIRE(start_with_move <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(contour);
    SkPathBuilder builder;
    if (!contour->value->getSegment(start, stop, &builder, start_with_move)) return cfsk::fail(CFSK_FAILED, "empty contour segment");
    return cfsk::owned(output, builder.detach());
}
