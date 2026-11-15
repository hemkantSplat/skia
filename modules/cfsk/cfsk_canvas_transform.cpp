/* Matrix validation and mutation have one owner for legacy and fused calls. */
#include "cfsk_internal.h"

namespace {
cfsk_status apply_transform(cfsk_canvas* canvas, uint32_t operation, const float* parameters,
                            float* resulting_matrix, bool fused) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id())
        return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    CFSK_REQUIRE(canvas && operation <= CFSK_TRANSFORM_ROTATE);
    CFSK_REQUIRE(!fused || resulting_matrix);
    const size_t count = operation == CFSK_TRANSFORM_CONCAT9 ? 9 :
                         operation == CFSK_TRANSFORM_CONCAT44 ? 16 :
                         operation == CFSK_TRANSFORM_ROTATE ? 3 : 2;
    CFSK_REQUIRE(cfsk::finite(parameters, count));
    switch (operation) {
        case CFSK_TRANSFORM_CONCAT9:
            canvas->value->concat(cfsk::matrix(parameters));
            break;
        case CFSK_TRANSFORM_CONCAT44:
            canvas->value->concat(SkM44::RowMajor(parameters));
            break;
        case CFSK_TRANSFORM_TRANSLATE:
            canvas->value->translate(parameters[0], parameters[1]);
            break;
        case CFSK_TRANSFORM_SCALE:
            canvas->value->scale(parameters[0], parameters[1]);
            break;
        case CFSK_TRANSFORM_SKEW:
            canvas->value->skew(parameters[0], parameters[1]);
            break;
        case CFSK_TRANSFORM_ROTATE:
            canvas->value->rotate(parameters[0], parameters[1], parameters[2]);
            break;
    }
    if (resulting_matrix) canvas->value->getLocalToDevice().getRowMajor(resulting_matrix);
    return CFSK_OK;
}
}  // namespace

cfsk_status cfsk_canvas_transform(cfsk_canvas* canvas, uint32_t operation,
                                const float parameters[16], float resulting_matrix[16]) {
    return apply_transform(canvas, operation, parameters, resulting_matrix, true);
}

cfsk_status cfsk_canvas_concat(cfsk_canvas* canvas, const float matrix[9]) {
    return apply_transform(canvas, CFSK_TRANSFORM_CONCAT9, matrix, nullptr, false);
}

cfsk_status cfsk_canvas_concat44(cfsk_canvas* canvas, const float matrix[16]) {
    return apply_transform(canvas, CFSK_TRANSFORM_CONCAT44, matrix, nullptr, false);
}

cfsk_status cfsk_canvas_translate(cfsk_canvas* canvas, float dx, float dy) {
    const float parameters[16] = {dx, dy};
    return apply_transform(canvas, CFSK_TRANSFORM_TRANSLATE, parameters, nullptr, false);
}

cfsk_status cfsk_canvas_scale(cfsk_canvas* canvas, float scale_x, float scale_y) {
    const float parameters[16] = {scale_x, scale_y};
    return apply_transform(canvas, CFSK_TRANSFORM_SCALE, parameters, nullptr, false);
}

cfsk_status cfsk_canvas_skew(cfsk_canvas* canvas, float skew_x, float skew_y) {
    const float parameters[16] = {skew_x, skew_y};
    return apply_transform(canvas, CFSK_TRANSFORM_SKEW, parameters, nullptr, false);
}

cfsk_status cfsk_canvas_rotate(cfsk_canvas* canvas, float degrees, float pivot_x, float pivot_y) {
    const float parameters[16] = {degrees, pivot_x, pivot_y};
    return apply_transform(canvas, CFSK_TRANSFORM_ROTATE, parameters, nullptr, false);
}
