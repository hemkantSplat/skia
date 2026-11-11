/* Opaque wrappers own Skia resources; borrowed canvases stay inside their owner. */
#ifndef CFSK_INTERNAL_H
#define CFSK_INTERNAL_H
#include "cfsk.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkMesh.h"
#include "include/core/SkBlender.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkVertices.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/core/SkContourMeasure.h"
#include "include/utils/SkWidthStroke.h"
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>
#include <thread>
#include <utility>

struct cfsk_canvas { SkCanvas* value = nullptr; std::thread::id owner = std::this_thread::get_id(); };
struct cfsk_color_space { sk_sp<SkColorSpace> value; };
struct cfsk_surface {
    sk_sp<SkSurface> value;
    cfsk_canvas canvas;
    cfsk_color_space space;
    const void* gpu_context = nullptr;
};
struct cfsk_image { sk_sp<SkImage> value; cfsk_color_space space; const void* gpu_context = nullptr; };
struct cfsk_paint { SkPaint value; };
struct cfsk_path { SkPath value; };
struct cfsk_path_builder { SkPathBuilder value; };
struct cfsk_shader { sk_sp<SkShader> value; };
struct cfsk_runtime_effect { sk_sp<SkRuntimeEffect> value; uint32_t kind; };
struct cfsk_runtime_effect_builder { SkRuntimeShaderBuilder value; };
struct cfsk_blender { sk_sp<SkBlender> value; };
struct cfsk_color_filter { sk_sp<SkColorFilter> value; };
struct cfsk_image_filter { sk_sp<SkImageFilter> value; };
struct cfsk_mask_filter { sk_sp<SkMaskFilter> value; };
struct cfsk_path_effect { sk_sp<SkPathEffect> value; };
struct cfsk_data { sk_sp<SkData> value; };
struct cfsk_typeface {
    explicit cfsk_typeface(sk_sp<SkTypeface> face);
    ~cfsk_typeface();
    cfsk_typeface(const cfsk_typeface&) = delete;
    cfsk_typeface& operator=(const cfsk_typeface&) = delete;
    static sk_sp<SkTypeface> retain_live(const cfsk_typeface* face);
    sk_sp<SkTypeface> value;
};
struct cfsk_font { SkFont value; };
struct cfsk_picture { sk_sp<SkPicture> value; };
struct cfsk_picture_recorder { SkPictureRecorder value; cfsk_canvas canvas; bool active = false; };
struct cfsk_text_blob { sk_sp<SkTextBlob> value; };
struct cfsk_vertices { sk_sp<SkVertices> value; };
struct cfsk_mesh_spec { sk_sp<SkMeshSpecification> value; };
struct cfsk_mesh_vertex_buffer {
    sk_sp<SkMesh::VertexBuffer> value;
    const void* recorder_id = nullptr;
    std::thread::id owner = std::this_thread::get_id();
};
struct cfsk_mesh_index_buffer {
    sk_sp<SkMesh::IndexBuffer> value;
    const void* recorder_id = nullptr;
    std::thread::id owner = std::this_thread::get_id();
};
struct cfsk_mesh {
    SkMesh value;
    const void* recorder_id = nullptr;
    std::thread::id owner = std::this_thread::get_id();
};
struct cfsk_contour_iter { SkContourMeasureIter value; };
struct cfsk_contour { sk_sp<SkContourMeasure> value; };
struct cfsk_width_stroke { sk_sp<SkWidthStroke> value; };

namespace cfsk {
cfsk_status mesh_recorder_identity(cfsk_recorder*, const void**);
std::vector<sk_sp<SkRuntimeEffect>> picture_known_effects(const cfsk_picture_resources* resources);
extern thread_local std::string error;
inline cfsk_status fail(cfsk_status code, const char* message) { error = message; return code; }
inline cfsk_status unsupported(const char* feature) { return fail(CFSK_UNSUPPORTED, feature); }
inline SkRect rect(cfsk_rect r) { return SkRect::MakeLTRB(r.left, r.top, r.right, r.bottom); }
inline cfsk_rect rect(const SkRect& r) { return {r.left(), r.top(), r.right(), r.bottom()}; }
inline SkIRect irect(cfsk_irect r) { return SkIRect::MakeLTRB(r.left, r.top, r.right, r.bottom); }
inline SkPoint point(cfsk_point p) { return {p.x, p.y}; }
inline SkColor4f color(cfsk_color c) { return {c.red, c.green, c.blue, c.alpha}; }
inline bool finite(float x) { return std::isfinite(x); }
inline bool finite(double x) { return std::isfinite(x); }
inline bool finite(cfsk_point p) { return finite(p.x) && finite(p.y); }
inline bool finite(cfsk_rect r) { return finite(r.left) && finite(r.top) && finite(r.right) && finite(r.bottom); }
inline bool finite(cfsk_color c) { return finite(c.red) && finite(c.green) && finite(c.blue) && finite(c.alpha); }
inline bool finite(const float* v, size_t n) {
    if (!v) return false;
    for (size_t i = 0; i < n; ++i) if (!finite(v[i])) return false;
    return true;
}
inline bool valid(cfsk_bytes s) { return s.data || !s.length; }
inline bool valid(cfsk_string s) { return s.data || !s.length; }
inline bool color_type(uint32_t t) {
    return t == 1 || t == 4 || t == 6 || t == 15 || t == 16 || t == 18;
}
inline bool valid(const cfsk_image_info* info) {
    return info && info->width > 0 && info->height > 0 && color_type(info->color_type) &&
           info->alpha_type >= 1 && info->alpha_type <= 3;
}
inline SkImageInfo image_info(const cfsk_image_info& i) {
    return SkImageInfo::Make(i.width, i.height, static_cast<SkColorType>(i.color_type),
                            static_cast<SkAlphaType>(i.alpha_type), i.color_space ? i.color_space->value : nullptr);
}
inline cfsk_image_info image_info(const SkImageInfo& i, cfsk_color_space& space) {
    space.value = i.refColorSpace();
    return {i.width(), i.height(), static_cast<uint32_t>(i.colorType()),
            static_cast<uint32_t>(i.alphaType()), space.value ? &space : nullptr};
}
inline bool extent(const SkImageInfo& info, size_t row_bytes, size_t capacity, size_t* bytes = nullptr) {
    if (row_bytes < info.minRowBytes() || !info.validRowBytes(row_bytes)) return false;
    size_t size = info.computeByteSize(row_bytes);
    if (size == SIZE_MAX) return false;
    if (bytes) *bytes = size;
    return size <= capacity;
}
inline SkMatrix matrix(const float* p) { SkMatrix m; m.set9(p); return m; }
inline SkRRect rrect(const cfsk_rrect& r) {
    SkVector radii[4];
    for (int i = 0; i < 4; ++i) radii[i] = point(r.radii[i]);
    SkRRect result; result.setRectRadii(rect(r.rect), radii); return result;
}
inline bool valid(const cfsk_rrect* r) {
    if (!r || !finite(r->rect)) return false;
    for (auto p : r->radii) if (!finite(p) || p.x < 0 || p.y < 0) return false;
    return true;
}
inline bool valid(cfsk_sampling s) {
    return s.filter <= 1 && s.mipmap <= 2 && s.cubic <= 1 && finite(s.cubic_b) && finite(s.cubic_c);
}
inline SkSamplingOptions sampling(cfsk_sampling s) {
    return s.cubic ? SkSamplingOptions(SkCubicResampler{s.cubic_b, s.cubic_c}) :
           SkSamplingOptions(static_cast<SkFilterMode>(s.filter), static_cast<SkMipmapMode>(s.mipmap));
}
template <typename W, typename V> cfsk_status owned(W** out, V&& value) {
    if (!out) return fail(CFSK_INVALID_ARGUMENT, "null output");
    *out = new (std::nothrow) W{std::forward<V>(value)};
    return *out ? CFSK_OK : fail(CFSK_OUT_OF_MEMORY, "wrapper allocation failed");
}
template <typename W, typename V> cfsk_status resource(W** out, sk_sp<V> value) {
    if (!value) return fail(CFSK_FAILED, "Skia factory failed");
    return owned(out, std::move(value));
}
template <typename T> auto ref(const T* v) { return v ? v->value : nullptr; }
inline cfsk_status copy(const void* data, size_t count, size_t stride, void* out, size_t capacity, size_t* required) {
    if (!required || (!out && capacity)) return fail(CFSK_INVALID_ARGUMENT, "invalid copy buffer");
    *required = count;
    if (!out && !capacity) return CFSK_OK;
    if (capacity < count) return fail(CFSK_BUFFER_TOO_SMALL, "copy buffer too small");
    if (count) std::memcpy(out, data, count * stride);
    return CFSK_OK;
}
}
#define CFSK_BEGIN cfsk::error.clear()
#define CFSK_REQUIRE(condition) do { if (!(condition)) return cfsk::fail(CFSK_INVALID_ARGUMENT, #condition); } while (0)
#define CFSK_OUTPUT(output) do { CFSK_REQUIRE(output); *(output) = nullptr; } while (0)
#endif
