/*
 * Copyright 2026 CourseForge. Use governed by the BSD-style LICENSE in this tree.
 * cfsk ABI v1: m157, CPU raster and Graphite/Dawn. No Ganesh or embind types.
 *
 * ABI NOTE
 * Ownership: direct opaque T** outputs except surface_get_canvas and
 * picture_recorder_begin are owned wrappers. Embedded/array handles have explicit
 * ownership annotations. Destroy each owned wrapper once with its typed
 * *_destroy (NULL is allowed). Rust cf-skia owns these in Drop; borrowed canvases
 * never get Drop and cannot outlive their surface/active picture recording.
 * Factories/setters retain their handle inputs as required by Skia; inputs and
 * slices are borrowed for the call, never stolen. No C++ object layout is public.
 * No handle is a Rust Send/Sync by default. Immutable CPU resources can be moved
 * after publication by the safe layer; mutable builders, paint and canvas cannot.
 *
 * Errors: functions return cfsk_status (fixed int32_t), never throw/unwind across
 * C. Skia is built -fno-exceptions. Invalid enums, non-finite geometry, overflow,
 * out-of-range indices and incompatible contexts return INVALID_ARGUMENT; null
 * Skia factories return FAILED/UNSUPPORTED as appropriate. No silent no-op mesh
 * rendering: a missing Graphite mesh RenderStep returns UNSUPPORTED. On failure
 * owned outputs are NULL; other outputs are untouched except required counts.
 * destroy is infallible. cfsk_error_copy copies the calling thread's last error
 * (UTF-8, no NUL, cleared at the next status call except error_copy itself).
 * For a copy API, NULL/0 queries required capacity; BUFFER_TOO_SMALL writes only
 * required. All sizes are element counts unless named *_bytes or row_bytes.
 * Strings are UTF-8 ptr+length, may contain NUL, never borrowed after a call.
 * Boolean fields/arguments are uint8_t, exactly 0 or 1. No C enum-sized ABI fields.
 * All structs use natural C alignment (no packing); no Rust bool/usize-as-u32.
 * Tagged enums use the pinned Skia ordinal unless an explicit enum below says
 * otherwise. The implementation must static_assert every exposed ordinal.
 *
 * Threading: one Graphite Recorder per record thread, never concurrent use. A
 * Context has one submit-thread owner; only that thread inserts/submits/polls.
 * A snapped Recording may move once to that thread; insert consumes *recording
 * and sets it NULL ONLY on success. Failed insert leaves ownership with caller.
 * Create Context with requireOrderedRecordings=true and choose internal samples
 * ONCE (1 raster-atlas gate/web/headless, 4 production), never per-surface DMSAA.
 * Recorder surfaces/images are confined to their recorder/context. Recording
 * retains submitted resources. Drain all submissions before destroying Context;
 * destroy Recorder before Context. Device loss is a typed status, never a new
 * fallback route. Dawn raw C handles are borrowed at create/wrap; cfsk retains
 * them until its wrapper and every recording using them are destroyed.
 * Native submit may wait; browser WebGPU MUST use poll (no blocking/Asyncify).
 * Submissions/readbacks own the internal C++ callbacks and completion storage;
 * no Rust callbacks are retained or invoked. Poll must process Dawn progress.
 *
 * Paths: cfsk_path is immutable SkPath. All geometry mutation takes a separate
 * cfsk_path_builder (SkPathBuilder). snapshot preserves builder; detach resets
 * it. stroke/simplify/offset/effect/transform return NEW immutable paths; never
 * mutate a path being drawn, cached or shared. Old Path.add/moveTo/etc. rows
 * map explicitly to builder calls, not to a mutable-Path compatibility shim.
 *
 * Images: decode/copy pixels on IO thread into immutable CPU image; cfsk copies
 * encoded/pixel input before returning (stride and total byte extent checked).
 * image_upload(recorder, image, mipmapped) calls TextureFromImage with Required
 * image properties; no upload on IO thread. It returns an owned Graphite image;
 * input CPU image may then be dropped. Mipmap policy is explicit and identity
 * stays in Rust's content store. Browser/GL texture-source operations become
 * explicit pixel upload or Dawn texture import, never a DOM object in this ABI.
 * import_texture retains WGPUTexture and metadata; producer must fence writes
 * before import/use and must not overwrite it until submission completion.
 * Surface snapshots COPY pixels (Graphite AsImageCopy); no mutable AsImage alias
 * can become a retained sealed raster. GPU readback uses a request + poll/copy;
 * the request owns data until destroy, avoiding browser synchronous readbacks.
 *
 * @cfsk rows are checked by tools/skia-native-build/cfsk-census.ts in rf-int.
 * A rust: target names logic/data kept on the Rust side; it is NOT a C stub.
 */
#ifndef CFSK_H
#define CFSK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CFSK_ABI_VERSION 1u
typedef int32_t cfsk_status;
enum {
    CFSK_OK = 0, CFSK_INVALID_ARGUMENT = 1, CFSK_FAILED = 2,
    CFSK_UNSUPPORTED = 3, CFSK_OUT_OF_MEMORY = 4, CFSK_DEVICE_LOST = 5,
    CFSK_BUFFER_TOO_SMALL = 6, CFSK_PENDING = 7, CFSK_WRONG_THREAD = 8
};
typedef struct { const char* data; size_t length; } cfsk_string;
typedef struct { const uint8_t* data; size_t length; } cfsk_bytes;
typedef struct { float x, y; } cfsk_point;
typedef struct { float left, top, right, bottom; } cfsk_rect;
typedef struct { int32_t left, top, right, bottom; } cfsk_irect;
typedef struct { float red, green, blue, alpha; } cfsk_color;
typedef struct { cfsk_rect rect; cfsk_point radii[4]; } cfsk_rrect;
typedef struct { float scos, ssin, translate_x, translate_y; } cfsk_rsxform;
/* Pinned m157 ordinals. Enum constants are names only; ABI fields stay uint32_t.
 * No N32 alias: callers choose explicit RGBA/BGRA for portable pixel layout. */
enum {
    CFSK_COLOR_UNKNOWN = 0, CFSK_COLOR_ALPHA_8 = 1, CFSK_COLOR_RGBA_8888 = 4,
    CFSK_COLOR_BGRA_8888 = 6, CFSK_COLOR_RGBA_F16_NORM = 15,
    CFSK_COLOR_RGBA_F16 = 16, CFSK_COLOR_RGBA_F32 = 18,
    CFSK_ALPHA_UNKNOWN = 0, CFSK_ALPHA_OPAQUE = 1, CFSK_ALPHA_PREMUL = 2, CFSK_ALPHA_UNPREMUL = 3,
    CFSK_TILE_CLAMP = 0, CFSK_TILE_REPEAT = 1, CFSK_TILE_MIRROR = 2, CFSK_TILE_DECAL = 3,
    CFSK_FILTER_NEAREST = 0, CFSK_FILTER_LINEAR = 1,
    CFSK_MIPMAP_NONE = 0, CFSK_MIPMAP_NEAREST = 1, CFSK_MIPMAP_LINEAR = 2,
    CFSK_CLIP_DIFFERENCE = 0, CFSK_CLIP_INTERSECT = 1,
    CFSK_FILL_WINDING = 0, CFSK_FILL_EVEN_ODD = 1, CFSK_FILL_INVERSE_WINDING = 2, CFSK_FILL_INVERSE_EVEN_ODD = 3,
    CFSK_DIRECTION_CW = 0, CFSK_DIRECTION_CCW = 1,
    CFSK_PATH_ADD_APPEND = 0, CFSK_PATH_ADD_EXTEND = 1,
    CFSK_PAINT_FILL = 0, CFSK_PAINT_STROKE = 1, CFSK_PAINT_STROKE_AND_FILL = 2,
    CFSK_CAP_BUTT = 0, CFSK_CAP_ROUND = 1, CFSK_CAP_SQUARE = 2,
    CFSK_JOIN_MITER = 0, CFSK_JOIN_ROUND = 1, CFSK_JOIN_BEVEL = 2,
    CFSK_OFFSET_JOIN_ROUND = 0, CFSK_OFFSET_JOIN_MITER = 1, CFSK_OFFSET_JOIN_BEVEL = 2,
    CFSK_PATH_OP_DIFFERENCE = 0, CFSK_PATH_OP_INTERSECT = 1, CFSK_PATH_OP_UNION = 2,
    CFSK_PATH_OP_XOR = 3, CFSK_PATH_OP_REVERSE_DIFFERENCE = 4,
    CFSK_PATH1D_TRANSLATE = 0, CFSK_PATH1D_ROTATE = 1, CFSK_PATH1D_MORPH = 2,
    CFSK_TRIM_NORMAL = 0, CFSK_TRIM_INVERTED = 1,
    CFSK_EDGING_ALIAS = 0, CFSK_EDGING_ANTIALIAS = 1, CFSK_EDGING_SUBPIXEL_ANTIALIAS = 2,
    CFSK_HINT_NONE = 0, CFSK_HINT_SLIGHT = 1, CFSK_HINT_NORMAL = 2, CFSK_HINT_FULL = 3,
    CFSK_FONT_UPRIGHT = 0, CFSK_FONT_ITALIC = 1, CFSK_FONT_OBLIQUE = 2,
    CFSK_TEXT_LEFT = 0, CFSK_TEXT_RIGHT = 1, CFSK_TEXT_CENTER = 2,
    CFSK_TEXT_JUSTIFY = 3, CFSK_TEXT_START = 4, CFSK_TEXT_END = 5,
    CFSK_TEXT_RTL = 0, CFSK_TEXT_LTR = 1,
    CFSK_BLUR_NORMAL = 0, CFSK_BLUR_SOLID = 1, CFSK_BLUR_OUTER = 2, CFSK_BLUR_INNER = 3,
    CFSK_ENCODE_JPEG = 3, CFSK_ENCODE_PNG = 4, CFSK_ENCODE_WEBP = 6,
    CFSK_CHANNEL_RED = 0, CFSK_CHANNEL_GREEN = 1, CFSK_CHANNEL_BLUE = 2, CFSK_CHANNEL_ALPHA = 3,
    CFSK_VERTICES_TRIANGLES = 0, CFSK_VERTICES_TRIANGLE_STRIP = 1, CFSK_VERTICES_TRIANGLE_FAN = 2,
    CFSK_LAYER_INIT_WITH_PREVIOUS = 1 << 2, CFSK_LAYER_F16 = 1 << 4,
    CFSK_SOURCE_STRICT = 0, CFSK_SOURCE_FAST = 1,
    CFSK_GRAD_DESTINATION = 0, CFSK_GRAD_SRGB_LINEAR = 1, CFSK_GRAD_LAB = 2, CFSK_GRAD_OKLAB = 3,
    CFSK_GRAD_OKLAB_GAMUT_MAP = 4, CFSK_GRAD_LCH = 5, CFSK_GRAD_OKLCH = 6,
    CFSK_GRAD_OKLCH_GAMUT_MAP = 7, CFSK_GRAD_SRGB = 8, CFSK_GRAD_HSL = 9, CFSK_GRAD_HWB = 10,
    CFSK_GRAD_DISPLAY_P3 = 11, CFSK_GRAD_REC2020 = 12, CFSK_GRAD_PROPHOTO_RGB = 13, CFSK_GRAD_A98RGB = 14,
    CFSK_HUE_SHORTER = 0, CFSK_HUE_LONGER = 1, CFSK_HUE_INCREASING = 2, CFSK_HUE_DECREASING = 3,
    CFSK_MESH_FLOAT = 0, CFSK_MESH_FLOAT2 = 1, CFSK_MESH_FLOAT3 = 2, CFSK_MESH_FLOAT4 = 3,
    CFSK_MESH_UBYTE4 = 4, CFSK_VARYING_HALF = 4, CFSK_VARYING_HALF2 = 5,
    CFSK_VARYING_HALF3 = 6, CFSK_VARYING_HALF4 = 7,
    CFSK_ATTRIBUTE_VERTEX = 0, CFSK_ATTRIBUTE_INSTANCE = 1
};
enum {
    CFSK_BLEND_CLEAR = 0, CFSK_BLEND_SRC = 1, CFSK_BLEND_DST = 2,
    CFSK_BLEND_SRC_OVER = 3, CFSK_BLEND_DST_OVER = 4, CFSK_BLEND_SRC_IN = 5, CFSK_BLEND_DST_IN = 6,
    CFSK_BLEND_SRC_OUT = 7, CFSK_BLEND_DST_OUT = 8, CFSK_BLEND_SRC_ATOP = 9, CFSK_BLEND_DST_ATOP = 10,
    CFSK_BLEND_XOR = 11, CFSK_BLEND_PLUS = 12, CFSK_BLEND_MODULATE = 13, CFSK_BLEND_SCREEN = 14,
    CFSK_BLEND_OVERLAY = 15, CFSK_BLEND_DARKEN = 16, CFSK_BLEND_LIGHTEN = 17,
    CFSK_BLEND_COLOR_DODGE = 18, CFSK_BLEND_COLOR_BURN = 19, CFSK_BLEND_HARD_LIGHT = 20,
    CFSK_BLEND_SOFT_LIGHT = 21, CFSK_BLEND_DIFFERENCE = 22, CFSK_BLEND_EXCLUSION = 23,
    CFSK_BLEND_MULTIPLY = 24, CFSK_BLEND_HUE = 25, CFSK_BLEND_SATURATION = 26,
    CFSK_BLEND_COLOR = 27, CFSK_BLEND_LUMINOSITY = 28
};
typedef struct cfsk_canvas cfsk_canvas;
typedef struct cfsk_context cfsk_context;
typedef struct cfsk_recorder cfsk_recorder;
typedef struct cfsk_recording cfsk_recording;
typedef struct cfsk_submission cfsk_submission;
typedef struct cfsk_readback cfsk_readback;
typedef struct cfsk_surface cfsk_surface;
typedef struct cfsk_paint cfsk_paint;
typedef struct cfsk_path cfsk_path;
typedef struct cfsk_path_builder cfsk_path_builder;
typedef struct cfsk_image cfsk_image;
typedef struct cfsk_shader cfsk_shader;
typedef struct cfsk_runtime_effect cfsk_runtime_effect;
typedef struct cfsk_runtime_effect_builder cfsk_runtime_effect_builder;
typedef struct cfsk_blender cfsk_blender;
typedef struct cfsk_color_filter cfsk_color_filter;
typedef struct cfsk_image_filter cfsk_image_filter;
typedef struct cfsk_mask_filter cfsk_mask_filter;
typedef struct cfsk_path_effect cfsk_path_effect;
typedef struct cfsk_picture cfsk_picture;
typedef struct cfsk_picture_recorder cfsk_picture_recorder;
typedef struct cfsk_font cfsk_font;
typedef struct cfsk_typeface cfsk_typeface;
typedef struct cfsk_font_provider cfsk_font_provider;
typedef struct cfsk_font_collection cfsk_font_collection;
typedef struct cfsk_paragraph_builder cfsk_paragraph_builder;
typedef struct cfsk_paragraph cfsk_paragraph;
typedef struct cfsk_text_blob cfsk_text_blob;
typedef struct cfsk_vertices cfsk_vertices;
typedef struct cfsk_contour_iter cfsk_contour_iter;
typedef struct cfsk_contour cfsk_contour;
typedef struct cfsk_mesh_spec cfsk_mesh_spec;
typedef struct cfsk_mesh_vertex_buffer cfsk_mesh_vertex_buffer;
typedef struct cfsk_mesh_index_buffer cfsk_mesh_index_buffer;
typedef struct cfsk_mesh cfsk_mesh;
typedef struct cfsk_width_stroke cfsk_width_stroke;
typedef struct cfsk_color_space cfsk_color_space;
typedef struct cfsk_data cfsk_data;

typedef struct {
    int32_t width, height;
    uint32_t color_type, alpha_type;
    const cfsk_color_space* color_space; /* NULL = untagged, retained by outputs. */
} cfsk_image_info;
typedef struct { uint32_t filter, mipmap; float cubic_b, cubic_c; uint8_t cubic; } cfsk_sampling;
typedef struct {
    const cfsk_paint* paint; const cfsk_rect* bounds;
    const cfsk_image_filter* backdrop; uint32_t flags, backdrop_tile_mode;
    const cfsk_color_space* color_space;
} cfsk_layer;
typedef struct {
    void* dawn_instance; void* dawn_device; void* dawn_queue; /* WGPU* C handles. */
    uint32_t internal_samples; size_t gpu_budget_bytes;
    const cfsk_runtime_effect* const* known_effects; size_t known_effect_count;
    /* Known effect array order is the stable key order, frozen for build identity. */
} cfsk_context_options;
typedef struct { uint8_t complete, has_gpu_time; uint64_t gpu_time_ns; } cfsk_completion;
typedef struct { size_t budgeted_bytes, purgeable_bytes, limit_bytes; } cfsk_cache_usage;
typedef struct { uint8_t in_premul; uint32_t color_space, hue_method; } cfsk_interpolation;
typedef struct {
    const cfsk_color* colors; const float* positions; size_t count;
    uint32_t tile_mode; const cfsk_color_space* color_space;
    const float* local_matrix; /* NULL or 9 row-major floats. */
    cfsk_interpolation interpolation;
} cfsk_gradient;
typedef struct { cfsk_string name; uint32_t offset_bytes, count, columns, rows; uint8_t is_integer; } cfsk_uniform;
typedef struct { cfsk_string name; const cfsk_image_filter* input; } cfsk_filter_child;
typedef void (*cfsk_shader_error_handler)(void* user, cfsk_string message);
typedef struct { uint32_t tag; float value; } cfsk_axis_coordinate;
typedef struct { uint32_t tag; float minimum, maximum, default_value; uint8_t hidden; } cfsk_axis;
typedef struct {
    float top, ascent, descent, bottom, leading, average_width, max_width;
    float x_min, x_max, x_height, cap_height, underline_thickness, underline_position;
    float strikeout_thickness, strikeout_position; uint32_t flags;
} cfsk_font_metrics;
typedef struct { cfsk_string tag; int32_t value; } cfsk_font_feature;
typedef struct {
    const cfsk_string* families; size_t family_count; float font_size;
    int32_t weight, width; uint32_t slant, text_align, text_direction;
    float letter_spacing, word_spacing, height_multiplier;
    uint8_t height_override, half_leading; cfsk_string locale;
    const cfsk_font_feature* features; size_t feature_count;
    const cfsk_axis_coordinate* variations; size_t variation_count;
    cfsk_color color; size_t max_lines; cfsk_string ellipsis;
} cfsk_paragraph_style;
typedef struct {
    size_t start_index, end_index, end_excluding_whitespace, end_including_newline;
    uint8_t hard_break; double ascent, descent, unscaled_ascent, height, width, left, baseline;
    size_t line_number;
} cfsk_line_metrics;
/* Shaped runs are copied from SkParagraph, not reshaped in Rust; clusters are
 * UTF-16 offsets as in CanvasKit. positions has glyph_count+1 points. */
typedef struct {
    size_t line_index, glyph_start, glyph_count, text_start, text_end;
    cfsk_typeface* typeface; /* Owned per copied run; caller destroys each. */
    float font_size; uint8_t fake_bold, fake_italic; uint32_t bidi_level, flags;
    float line_top, line_bottom, line_baseline;
} cfsk_shaped_run;
typedef struct { cfsk_string name; cfsk_bytes bytes; } cfsk_asset;
typedef struct { cfsk_string name; uint32_t type, offset, rate; } cfsk_mesh_attribute;
typedef struct { cfsk_string name; uint32_t type; } cfsk_mesh_varying;
typedef struct { const cfsk_mesh_vertex_buffer* buffer; size_t count, offset_bytes; } cfsk_mesh_vertex_range;
typedef struct { const cfsk_mesh_index_buffer* buffer; size_t count, offset_bytes; } cfsk_mesh_index_range;
typedef struct {
    uint32_t kind; /* 0 null, 1 shader, 2 color filter, 3 blender. */
    union {
        const cfsk_shader* shader;
        const cfsk_color_filter* color_filter;
        const cfsk_blender* blender;
    } value;
} cfsk_mesh_child;
typedef struct {
    const cfsk_mesh_spec* spec; uint32_t mode;
    cfsk_mesh_vertex_range vertices, instances; cfsk_mesh_index_range indices;
    cfsk_bytes uniforms;
    const cfsk_mesh_child* children; size_t child_count; /* Original declaration order. */
    cfsk_rect bounds;
} cfsk_mesh_desc;
typedef struct { double envelope, path_length; uint8_t merge_joins, subdivide; uint32_t batches; } cfsk_width_stroke_options;
typedef struct { uint32_t start, count, batch; } cfsk_stroke_chunk;
typedef struct { uint32_t start, count; } cfsk_stroke_range;

/* Graphite lifecycle and CPU surfaces */
/* @cfsk CanvasKit.buildIdentity => cfsk_abi_version | Linked header/library version; CanvasKit build handshake replacement */
cfsk_status cfsk_abi_version(uint32_t* version);
/* @cfsk CanvasKit.error => cfsk_error_copy | Copy diagnostic from failed CanvasKit/Skia operation */
cfsk_status cfsk_error_copy(char* output, size_t capacity, size_t* required);
/* @cfsk CanvasKit.MakeGrContext => cfsk_context_make_dawn | Graphite ContextFactory::MakeDawn; no GL context or Ganesh options */
/* @cfsk GpuOptionsApi.makeContext => cfsk_context_make_dawn | Graphite ContextFactory::MakeDawn; no GL context or Ganesh options */
cfsk_status cfsk_context_make_dawn(const cfsk_context_options* options, cfsk_context** output);
/* @cfsk Native.Vulkan => cfsk_context_make_vulkan | Owned offscreen Dawn host; optional CFP1 persistent keys; no fallback adapter */
cfsk_status cfsk_context_make_vulkan(cfsk_bytes persistent_data, cfsk_context** output);
/* @cfsk Graphite.PersistentPipelineStorage => cfsk_context_pipeline_cache | Context::syncPipelineData; owned data copied from storage */
cfsk_status cfsk_context_pipeline_cache(cfsk_context* context, cfsk_data** output);
/* @cfsk Graphite.PipelineCounters => cfsk_context_pipeline_stats | Actual pipeline additions, precompiled additions, unkeyable inline additions; cumulative */
cfsk_status cfsk_context_pipeline_stats(cfsk_context* context, uint32_t* inline_count, uint32_t* precompiled_count, uint32_t* unkeyable_count);
/* @cfsk Graphite.Context.makeRecorder => cfsk_context_make_recorder | Context::makeRecorder; transfer newly created unused recorder to one record thread */
cfsk_status cfsk_context_make_recorder(cfsk_context* context, cfsk_recorder** output);
/* @cfsk Graphite.Recorder.snap => cfsk_recorder_snap | Recorder::snap */
cfsk_status cfsk_recorder_snap(cfsk_recorder* recorder, cfsk_recording** output);
/* @cfsk Graphite.Context.insertRecording => cfsk_context_insert | Context::insertRecording; consumes recording only on success; completion ticket */
cfsk_status cfsk_context_insert(cfsk_context* context, cfsk_recording** recording, uint8_t gpu_time, cfsk_submission** output);
/* @cfsk BindingsApi.flushAndSubmit => cfsk_context_submit | Raster Surface.flush is a Rust no-op; GPU uses recorder_snap/context_insert then Context::submit; wait=1 unsupported in browser WebGPU */
/* @cfsk BindingsSurface.flushAndSubmit => cfsk_context_submit | Raster Surface.flush is a Rust no-op; GPU uses recorder_snap/context_insert then Context::submit; wait=1 unsupported in browser WebGPU */
/* @cfsk Surface.flush => cfsk_context_submit | Raster Surface.flush is a Rust no-op; GPU uses recorder_snap/context_insert then Context::submit; wait=1 unsupported in browser WebGPU */
/* @cfsk GrDirectContext.submit => cfsk_context_submit | Raster Surface.flush is a Rust no-op; GPU uses recorder_snap/context_insert then Context::submit; wait=1 unsupported in browser WebGPU */
cfsk_status cfsk_context_submit(cfsk_context* context, uint8_t wait_for_cpu);
/* @cfsk BindingsApi.checkAsyncWorkCompletion => cfsk_context_poll | Context::checkAsyncWorkCompletion and Dawn progress */
/* @cfsk ForkGrDirectContext.checkAsyncWorkCompletion => cfsk_context_poll | Context::checkAsyncWorkCompletion and Dawn progress */
cfsk_status cfsk_context_poll(cfsk_context* context);
/* @cfsk FlushOptions.onFinished => cfsk_submission_poll | Poll internal InsertRecordingInfo completion; no Rust callback; PENDING until ready */
cfsk_status cfsk_submission_poll(cfsk_submission* submission, cfsk_completion* output);
/* @cfsk DiagnosticsApi.cache => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk ForkGrDirectContext.getResourceCacheUsage => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk ForkGrDirectContext.getResourceCachePurgeableBytes => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk ForkGrDirectContext._getResourceCacheLimitBytes => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk ForkGrDirectContext._getResourceCacheUsageBytes => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk GrDirectContext.getResourceCacheLimitBytes => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk GrDirectContext._getResourceCacheLimitBytes => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk GrDirectContext._getResourceCacheUsageBytes => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk GrDirectContext.getResourceCacheUsage => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
/* @cfsk GrDirectContext.getResourceCachePurgeableBytes => cfsk_context_cache_usage | Context::currentBudgetedBytes/currentPurgeableBytes/maxBudgetedBytes; resource count is Rust bookkeeping */
cfsk_status cfsk_context_cache_usage(cfsk_context* context, cfsk_cache_usage* output);
/* @cfsk GrDirectContext.setResourceCacheLimitBytes => cfsk_context_set_cache_limit | Context::setMaxBudgetedBytes */
cfsk_status cfsk_context_set_cache_limit(cfsk_context* context, size_t bytes);
/* @cfsk ProgramCacheApi.precompile => cfsk_context_precompile | PrecompileContext::precompile(serializedPipeline); GLSL key/data format is retired */
/* @cfsk ForkGrDirectContext.precompileShader => cfsk_context_precompile | PrecompileContext::precompile(serializedPipeline); GLSL key/data format is retired */
cfsk_status cfsk_context_precompile(cfsk_context* context, cfsk_bytes serialized_pipeline);
/* @cfsk Graphite.Context.pipelineCachingCallback => cfsk_context_take_pipeline | Drain one owned serialized pipeline captured internally; OK/NULL if empty */
cfsk_status cfsk_context_take_pipeline(cfsk_context* context, cfsk_data** output);
/* @cfsk CanvasKit.MakeSurface => cfsk_surface_make_raster | SkSurfaces::Raster; host presentation stays Rust */
/* @cfsk CanvasKit.MakeSWCanvasSurface => cfsk_surface_make_raster | SkSurfaces::Raster; host presentation stays Rust */
cfsk_status cfsk_surface_make_raster(const cfsk_image_info* info, cfsk_surface** output);
/* @cfsk GpuOptionsApi.makeSurface => cfsk_surface_make_graphite | SkSurfaces::RenderTarget; host creates Dawn presentation target; samples are Context policy */
/* @cfsk Surface.makeSurface => cfsk_surface_make_graphite | SkSurfaces::RenderTarget; host creates Dawn presentation target; samples are Context policy */
/* @cfsk CanvasKit.MakeOnScreenGLSurface => cfsk_surface_make_graphite | SkSurfaces::RenderTarget; host creates Dawn presentation target; samples are Context policy */
/* @cfsk CanvasKit.MakeWebGLCanvasSurface => cfsk_surface_make_graphite | SkSurfaces::RenderTarget; host creates Dawn presentation target; samples are Context policy */
cfsk_status cfsk_surface_make_graphite(cfsk_recorder* recorder, const cfsk_image_info* info, uint8_t mipmapped, cfsk_surface** output);
/* @cfsk Graphite.Surface.wrapBackendTexture => cfsk_surface_wrap_texture | SkSurfaces::WrapBackendTexture; pinned Dawn C texture handle retained; no borrowed producer writes */
cfsk_status cfsk_surface_wrap_texture(cfsk_recorder* recorder, void* dawn_texture, const cfsk_image_info* info, cfsk_surface** output);
/* @cfsk Surface.getCanvas => cfsk_surface_get_canvas | SkSurface::getCanvas; borrowed, never destroyed */
cfsk_status cfsk_surface_get_canvas(cfsk_surface* surface, cfsk_canvas** output);
/* @cfsk Surface.imageInfo => cfsk_surface_info | SkSurface::imageInfo; output color_space is borrowed from surface */
/* @cfsk Surface.width => cfsk_surface_info | SkSurface::imageInfo; output color_space is borrowed from surface */
/* @cfsk Surface.height => cfsk_surface_info | SkSurface::imageInfo; output color_space is borrowed from surface */
cfsk_status cfsk_surface_info(const cfsk_surface* surface, cfsk_image_info* output);
/* @cfsk Surface.reportBackendTypeIsGPU => cfsk_surface_is_gpu | SkSurface::recorder != nullptr */
cfsk_status cfsk_surface_is_gpu(const cfsk_surface* surface, uint8_t* output);
/* @cfsk Surface.makeImageSnapshot => cfsk_surface_snapshot | Graphite AsImageCopy, raster makeImageSnapshot; immutable retained content */
cfsk_status cfsk_surface_snapshot(cfsk_surface* surface, const cfsk_irect* subset, uint8_t mipmapped, cfsk_image** output);
/* @cfsk Surface.readPixels => cfsk_surface_read_pixels | CPU SkSurface::readPixels; GPU returns UNSUPPORTED, use readback request */
cfsk_status cfsk_surface_read_pixels(cfsk_surface* surface, const cfsk_image_info* info, int32_t source_x, int32_t source_y, void* output, size_t output_bytes, size_t row_bytes);
/* @cfsk Graphite.Context.asyncRescaleAndReadPixels => cfsk_surface_readback | Context::asyncRescaleAndReadPixels; pending request owns internal callback data */
cfsk_status cfsk_surface_readback(cfsk_context* context, cfsk_surface* surface, const cfsk_image_info* info, cfsk_irect source, cfsk_readback** output);
/* @cfsk Graphite.AsyncReadResult => cfsk_readback_copy | PENDING until ready; copy rows from owned AsyncReadResult; never a borrowed Skia pixel pointer */
cfsk_status cfsk_readback_copy(cfsk_readback* readback, void* output, size_t output_bytes, size_t row_bytes, size_t* required_bytes);

/* Canvas: row-major matrices, LTRB rectangles, straight float RGBA + explicit color spaces */
/* @cfsk Canvas.clear => cfsk_canvas_clear | SkCanvas::clear(SkColor4f) in canvas color space; convert tagged color if supplied */
cfsk_status cfsk_canvas_clear(cfsk_canvas* canvas, cfsk_color color, const cfsk_color_space* space);
/* @cfsk Canvas.save => cfsk_canvas_save | SkCanvas::save */
cfsk_status cfsk_canvas_save(cfsk_canvas* canvas, int32_t* save_count);
/* @cfsk Canvas.getSaveCount => cfsk_canvas_get_save_count | SkCanvas::getSaveCount */
cfsk_status cfsk_canvas_get_save_count(cfsk_canvas* canvas, int32_t* save_count);
/* @cfsk Canvas.restore => cfsk_canvas_restore | SkCanvas::restore */
cfsk_status cfsk_canvas_restore(cfsk_canvas* canvas);
/* @cfsk Canvas.restoreToCount => cfsk_canvas_restore_to_count | SkCanvas::restoreToCount */
cfsk_status cfsk_canvas_restore_to_count(cfsk_canvas* canvas, int32_t count);
/* @cfsk Canvas.concat => cfsk_canvas_concat | SkCanvas::concat(SkMatrix) */
cfsk_status cfsk_canvas_concat(cfsk_canvas* canvas, const float matrix[9]);
/* @cfsk Canvas.getTotalMatrix => cfsk_canvas_get_matrix | SkCanvas::getTotalMatrix */
cfsk_status cfsk_canvas_get_matrix(cfsk_canvas* canvas, float matrix[9]);
/* @cfsk Canvas.getLocalToDevice => cfsk_canvas_get_matrix44 | SkCanvas::getLocalToDevice (row-major output) */
cfsk_status cfsk_canvas_get_matrix44(cfsk_canvas* canvas, float matrix[16]);
/* @cfsk Canvas.getDeviceClipBounds => cfsk_canvas_get_device_clip_bounds | SkCanvas::getDeviceClipBounds */
cfsk_status cfsk_canvas_get_device_clip_bounds(cfsk_canvas* canvas, cfsk_irect* bounds);
/* @cfsk Canvas.translate => cfsk_canvas_translate | SkCanvas::translate */
cfsk_status cfsk_canvas_translate(cfsk_canvas* canvas, float dx, float dy);
/* @cfsk Canvas.scale => cfsk_canvas_scale | SkCanvas::scale */
cfsk_status cfsk_canvas_scale(cfsk_canvas* canvas, float scale_x, float scale_y);
/* @cfsk Canvas.skew => cfsk_canvas_skew | SkCanvas::skew */
cfsk_status cfsk_canvas_skew(cfsk_canvas* canvas, float skew_x, float skew_y);
/* @cfsk Canvas.rotate => cfsk_canvas_rotate | SkCanvas::rotate */
cfsk_status cfsk_canvas_rotate(cfsk_canvas* canvas, float degrees, float pivot_x, float pivot_y);
/* @cfsk Canvas.concat44 => cfsk_canvas_concat44 | SkCanvas::concat(SkM44); 16 row-major floats, no singular basis shim */
cfsk_status cfsk_canvas_concat44(cfsk_canvas* canvas, const float matrix[16]);
/* @cfsk Canvas.saveLayer => cfsk_canvas_save_layer | SkCanvas::saveLayer(SaveLayerRec); includes color space/backdrop/tile mode */
/* @cfsk CompositingApi.saveLayer => cfsk_canvas_save_layer | SkCanvas::saveLayer(SaveLayerRec); includes color space/backdrop/tile mode */
cfsk_status cfsk_canvas_save_layer(cfsk_canvas* canvas, const cfsk_layer* layer, int32_t* save_count);
/* @cfsk Canvas.clipRect => cfsk_canvas_clip_rect | SkCanvas::clipRect */
cfsk_status cfsk_canvas_clip_rect(cfsk_canvas* canvas, cfsk_rect shape, uint32_t op, uint8_t antialias);
/* @cfsk Canvas.clipRRect => cfsk_canvas_clip_rrect | SkCanvas::clipRRect */
cfsk_status cfsk_canvas_clip_rrect(cfsk_canvas* canvas, const cfsk_rrect* shape, uint32_t op, uint8_t antialias);
/* @cfsk Canvas.clipPath => cfsk_canvas_clip_path | SkCanvas::clipPath */
cfsk_status cfsk_canvas_clip_path(cfsk_canvas* canvas, const cfsk_path* shape, uint32_t op, uint8_t antialias);
/* @cfsk Canvas.drawRect => cfsk_canvas_draw_rect | SkCanvas::drawRect */
cfsk_status cfsk_canvas_draw_rect(cfsk_canvas* canvas, cfsk_rect rect, const cfsk_paint* paint);
/* @cfsk Canvas.drawRRect => cfsk_canvas_draw_rrect | SkCanvas::drawRRect */
cfsk_status cfsk_canvas_draw_rrect(cfsk_canvas* canvas, const cfsk_rrect* rect, const cfsk_paint* paint);
/* @cfsk Canvas.drawOval => cfsk_canvas_draw_oval | SkCanvas::drawOval */
cfsk_status cfsk_canvas_draw_oval(cfsk_canvas* canvas, cfsk_rect rect, const cfsk_paint* paint);
/* @cfsk Canvas.drawPath => cfsk_canvas_draw_path | SkCanvas::drawPath */
cfsk_status cfsk_canvas_draw_path(cfsk_canvas* canvas, const cfsk_path* path, const cfsk_paint* paint);
/* @cfsk Canvas.drawLine => cfsk_canvas_draw_line | SkCanvas::drawLine */
cfsk_status cfsk_canvas_draw_line(cfsk_canvas* canvas, cfsk_point start, cfsk_point end, const cfsk_paint* paint);
/* @cfsk Canvas.drawPaint => cfsk_canvas_draw_paint | SkCanvas::drawPaint */
cfsk_status cfsk_canvas_draw_paint(cfsk_canvas* canvas, const cfsk_paint* paint);
/* @cfsk Canvas.drawCircle => cfsk_canvas_draw_circle | SkCanvas::drawCircle; used by the JS source-tree vertex-color probe */
cfsk_status cfsk_canvas_draw_circle(cfsk_canvas* canvas, float x, float y, float radius, const cfsk_paint* paint);
/* @cfsk Canvas.drawPicture => cfsk_canvas_draw_picture | SkCanvas::drawPicture; matrix=NULL or 9 floats, paint nullable */
cfsk_status cfsk_canvas_draw_picture(cfsk_canvas* canvas, const cfsk_picture* picture, const float* matrix, const cfsk_paint* paint);
/* @cfsk Canvas.drawImage => cfsk_canvas_draw_image | SkCanvas::drawImage with SkSamplingOptions; paint nullable */
/* @cfsk Canvas.drawImageOptions => cfsk_canvas_draw_image | SkCanvas::drawImage with SkSamplingOptions; paint nullable */
cfsk_status cfsk_canvas_draw_image(cfsk_canvas* canvas, const cfsk_image* image, float x, float y, cfsk_sampling sampling, const cfsk_paint* paint);
/* @cfsk Canvas.drawImageRectCubic => cfsk_canvas_draw_image_rect | SkCanvas::drawImageRect; explicit strict/fast source constraint */
/* @cfsk Canvas.drawImageRectOptions => cfsk_canvas_draw_image_rect | SkCanvas::drawImageRect; explicit strict/fast source constraint */
cfsk_status cfsk_canvas_draw_image_rect(cfsk_canvas* canvas, const cfsk_image* image, cfsk_rect source, cfsk_rect destination, cfsk_sampling sampling, const cfsk_paint* paint, uint32_t constraint);
/* @cfsk Canvas.drawGlyphs => cfsk_canvas_draw_glyphs | SkCanvas::drawGlyphs */
cfsk_status cfsk_canvas_draw_glyphs(cfsk_canvas* canvas, const uint16_t* glyphs, const cfsk_point* positions, size_t count, cfsk_point origin, const cfsk_font* font, const cfsk_paint* paint);
/* @cfsk Canvas.drawTextBlob => cfsk_canvas_draw_text_blob | SkCanvas::drawTextBlob */
cfsk_status cfsk_canvas_draw_text_blob(cfsk_canvas* canvas, const cfsk_text_blob* blob, float x, float y, const cfsk_paint* paint);
/* @cfsk Canvas.drawText => cfsk_canvas_draw_text | SkCanvas::drawSimpleText with UTF8; shaping uses Paragraph */
cfsk_status cfsk_canvas_draw_text(cfsk_canvas* canvas, cfsk_string text, float x, float y, const cfsk_font* font, const cfsk_paint* paint);
/* @cfsk coverage-mesh.drawCoverageMesh => cfsk_canvas_draw_coverage_mesh | CPU float-coverage mask batch; arrays borrowed synchronously; caller owns layer and SrcIn fill */
cfsk_status cfsk_canvas_draw_coverage_mesh(cfsk_canvas* canvas, const float* positions, const float* coverage, size_t vertex_count, const uint16_t* indices, size_t index_count);
/* @cfsk Canvas.drawVertices => cfsk_canvas_draw_vertices | SkCanvas::drawVertices */
cfsk_status cfsk_canvas_draw_vertices(cfsk_canvas* canvas, const cfsk_vertices* vertices, uint32_t blend_mode, const cfsk_paint* paint);
/* @cfsk Canvas.readPixels => cfsk_canvas_read_pixels | CPU SkCanvas::readPixels; GPU uses surface_readback */
cfsk_status cfsk_canvas_read_pixels(cfsk_canvas* canvas, const cfsk_image_info* info, int32_t source_x, int32_t source_y, void* output, size_t output_bytes, size_t row_bytes);
/* @cfsk Canvas.writePixels => cfsk_canvas_write_pixels | SkCanvas::writePixels; checked pixel extent */
cfsk_status cfsk_canvas_write_pixels(cfsk_canvas* canvas, const cfsk_image_info* info, cfsk_bytes pixels, size_t row_bytes, int32_t x, int32_t y);

/* Paint and immutable path geometry */
/* @cfsk CanvasKit.Paint => cfsk_paint_make | SkPaint default constructor */
cfsk_status cfsk_paint_make(cfsk_paint** output);
/* @cfsk Paint.copy => cfsk_paint_copy | SkPaint copy constructor */
cfsk_status cfsk_paint_copy(const cfsk_paint* paint, cfsk_paint** output);
/* @cfsk Paint.setColor => cfsk_paint_set_color | SkPaint::setColor4f; preserves HDR values */
cfsk_status cfsk_paint_set_color(cfsk_paint* paint, cfsk_color color, const cfsk_color_space* space);
/* @cfsk Paint.getColor => cfsk_paint_get_color | SkPaint::getColor4f */
cfsk_status cfsk_paint_get_color(const cfsk_paint* paint, cfsk_color* color);
/* @cfsk Paint.setAlphaf => cfsk_paint_set_alpha | SkPaint::setAlphaf */
cfsk_status cfsk_paint_set_alpha(cfsk_paint* paint, float value);
/* @cfsk Paint.setAntiAlias => cfsk_paint_set_antialias | SkPaint::setAntiAlias */
cfsk_status cfsk_paint_set_antialias(cfsk_paint* paint, uint8_t value);
/* @cfsk Paint.setDither => cfsk_paint_set_dither | SkPaint::setDither */
cfsk_status cfsk_paint_set_dither(cfsk_paint* paint, uint8_t value);
/* @cfsk Paint.setBlendMode => cfsk_paint_set_blend_mode | SkPaint::setBlendMode */
cfsk_status cfsk_paint_set_blend_mode(cfsk_paint* paint, uint32_t value);
/* @cfsk Paint.setStrokeCap => cfsk_paint_set_stroke_cap | SkPaint::setStrokeCap */
cfsk_status cfsk_paint_set_stroke_cap(cfsk_paint* paint, uint32_t value);
/* @cfsk Paint.setStrokeJoin => cfsk_paint_set_stroke_join | SkPaint::setStrokeJoin */
cfsk_status cfsk_paint_set_stroke_join(cfsk_paint* paint, uint32_t value);
/* @cfsk Paint.setStrokeMiter => cfsk_paint_set_stroke_miter | SkPaint::setStrokeMiter */
cfsk_status cfsk_paint_set_stroke_miter(cfsk_paint* paint, float value);
/* @cfsk Paint.setStrokeWidth => cfsk_paint_set_stroke_width | SkPaint::setStrokeWidth */
cfsk_status cfsk_paint_set_stroke_width(cfsk_paint* paint, float value);
/* @cfsk Paint.setStyle => cfsk_paint_set_style | SkPaint::setStyle */
cfsk_status cfsk_paint_set_style(cfsk_paint* paint, uint32_t value);
/* @cfsk Paint.setBlender => cfsk_paint_set_blender | SkPaint::setBlender; NULL clears; retains input */
cfsk_status cfsk_paint_set_blender(cfsk_paint* paint, const cfsk_blender* value);
/* @cfsk Paint.setColorFilter => cfsk_paint_set_color_filter | SkPaint::setColorFilter; NULL clears; retains input */
cfsk_status cfsk_paint_set_color_filter(cfsk_paint* paint, const cfsk_color_filter* value);
/* @cfsk Paint.setImageFilter => cfsk_paint_set_image_filter | SkPaint::setImageFilter; NULL clears; retains input */
cfsk_status cfsk_paint_set_image_filter(cfsk_paint* paint, const cfsk_image_filter* value);
/* @cfsk Paint.setMaskFilter => cfsk_paint_set_mask_filter | SkPaint::setMaskFilter; NULL clears; retains input */
cfsk_status cfsk_paint_set_mask_filter(cfsk_paint* paint, const cfsk_mask_filter* value);
/* @cfsk Paint.setPathEffect => cfsk_paint_set_path_effect | SkPaint::setPathEffect; NULL clears; retains input */
cfsk_status cfsk_paint_set_path_effect(cfsk_paint* paint, const cfsk_path_effect* value);
/* @cfsk Paint.setShader => cfsk_paint_set_shader | SkPaint::setShader; NULL clears; retains input */
cfsk_status cfsk_paint_set_shader(cfsk_paint* paint, const cfsk_shader* value);
/* @cfsk CanvasKit.Path => cfsk_path_builder_make | SkPathBuilder(source or empty); immutable Path is never constructed for mutation */
/* @cfsk ForkBindingsStatics.Path => cfsk_path_builder_make | SkPathBuilder(source or empty); immutable Path is never constructed for mutation */
cfsk_status cfsk_path_builder_make(const cfsk_path* source, cfsk_path_builder** output);
/* @cfsk PathBuilder.snapshot => cfsk_path_builder_snapshot | SkPathBuilder::snapshot; keeps builder state */
cfsk_status cfsk_path_builder_snapshot(const cfsk_path_builder* builder, cfsk_path** output);
/* @cfsk PathBuilder.detach => cfsk_path_builder_detach | SkPathBuilder::detach; resets builder */
cfsk_status cfsk_path_builder_detach(cfsk_path_builder* builder, cfsk_path** output);
/* @cfsk PathBuilder.reset => cfsk_path_builder_reset | SkPathBuilder::reset */
cfsk_status cfsk_path_builder_reset(cfsk_path_builder* builder);
/* @cfsk Path.moveTo => cfsk_path_builder_move_to | SkPathBuilder::moveTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.moveTo => cfsk_path_builder_move_to | SkPathBuilder::moveTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_move_to(cfsk_path_builder* builder, float x, float y);
/* @cfsk Path.lineTo => cfsk_path_builder_line_to | SkPathBuilder::lineTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.lineTo => cfsk_path_builder_line_to | SkPathBuilder::lineTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_line_to(cfsk_path_builder* builder, float x, float y);
/* @cfsk Path.quadTo => cfsk_path_builder_quad_to | SkPathBuilder::quadTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.quadTo => cfsk_path_builder_quad_to | SkPathBuilder::quadTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_quad_to(cfsk_path_builder* builder, float x1, float y1, float x2, float y2);
/* @cfsk Path.conicTo => cfsk_path_builder_conic_to | SkPathBuilder::conicTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.conicTo => cfsk_path_builder_conic_to | SkPathBuilder::conicTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_conic_to(cfsk_path_builder* builder, float x1, float y1, float x2, float y2, float weight);
/* @cfsk Path.cubicTo => cfsk_path_builder_cubic_to | SkPathBuilder::cubicTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.cubicTo => cfsk_path_builder_cubic_to | SkPathBuilder::cubicTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_cubic_to(cfsk_path_builder* builder, float x1, float y1, float x2, float y2, float x3, float y3);
/* @cfsk Path.close => cfsk_path_builder_close | SkPathBuilder::close (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.close => cfsk_path_builder_close | SkPathBuilder::close (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_close(cfsk_path_builder* builder);
/* @cfsk Path.addRect => cfsk_path_builder_add_rect | SkPathBuilder::addRect (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.addRect => cfsk_path_builder_add_rect | SkPathBuilder::addRect (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_add_rect(cfsk_path_builder* builder, cfsk_rect rect, uint32_t direction);
/* @cfsk Path.addRRect => cfsk_path_builder_add_rrect | SkPathBuilder::addRRect (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.addRRect => cfsk_path_builder_add_rrect | SkPathBuilder::addRRect (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_add_rrect(cfsk_path_builder* builder, const cfsk_rrect* rect, uint32_t direction);
/* @cfsk Path.addCircle => cfsk_path_builder_add_circle | SkPathBuilder::addCircle (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.addCircle => cfsk_path_builder_add_circle | SkPathBuilder::addCircle (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_add_circle(cfsk_path_builder* builder, float x, float y, float radius, uint32_t direction);
/* @cfsk Path.addPath => cfsk_path_builder_add_path | SkPathBuilder::addPath (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.addPath => cfsk_path_builder_add_path | SkPathBuilder::addPath (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_add_path(cfsk_path_builder* builder, const cfsk_path* path, const float matrix[9], uint32_t mode);
/* @cfsk Path.arcToOval => cfsk_path_builder_arc_to_oval | SkPathBuilder::arcTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.arcToOval => cfsk_path_builder_arc_to_oval | SkPathBuilder::arcTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_arc_to_oval(cfsk_path_builder* builder, cfsk_rect oval, float start_degrees, float sweep_degrees, uint8_t force_move);
/* @cfsk Path.arcToRotated => cfsk_path_builder_arc_to_rotated | SkPathBuilder::arcTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.arcToRotated => cfsk_path_builder_arc_to_rotated | SkPathBuilder::arcTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_arc_to_rotated(cfsk_path_builder* builder, float radius_x, float radius_y, float rotation_degrees, uint8_t large_arc, uint32_t sweep_direction, float x, float y);
/* @cfsk Path.arcToTangent => cfsk_path_builder_arc_to_tangent | SkPathBuilder::arcTo (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.arcToTangent => cfsk_path_builder_arc_to_tangent | SkPathBuilder::arcTo (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_arc_to_tangent(cfsk_path_builder* builder, float x1, float y1, float x2, float y2, float radius);
/* @cfsk Path.setFillType => cfsk_path_builder_set_fill_type | SkPathBuilder::setFillType (m157 builder operation, not mutable SkPath) */
/* @cfsk PathBuilder.setFillType => cfsk_path_builder_set_fill_type | SkPathBuilder::setFillType (m157 builder operation, not mutable SkPath) */
cfsk_status cfsk_path_builder_set_fill_type(cfsk_path_builder* builder, uint32_t fill_type);
/* @cfsk PathBuilder.transform => cfsk_path_builder_transform | SkPathBuilder::transform */
cfsk_status cfsk_path_builder_transform(cfsk_path_builder* builder, const float matrix[9]);
/* @cfsk Path.copy => cfsk_path_copy | SkPath value copy */
/* @cfsk ForkBindingsPath.copy => cfsk_path_copy | SkPath value copy */
cfsk_status cfsk_path_copy(const cfsk_path* path, cfsk_path** output);
/* @cfsk Path.transform => cfsk_path_transform | SkPath::makeTransform; NEW path */
cfsk_status cfsk_path_transform(const cfsk_path* path, const float matrix[9], cfsk_path** output);
/* @cfsk Path.getBounds => cfsk_path_bounds | SkPath::getBounds */
cfsk_status cfsk_path_bounds(const cfsk_path* path, cfsk_rect* output);
/* @cfsk Path.getFillType => cfsk_path_fill_type | SkPath::getFillType */
cfsk_status cfsk_path_fill_type(const cfsk_path* path, uint32_t* output);
/* @cfsk Path.countPoints => cfsk_path_point_count | SkPath::countPoints */
cfsk_status cfsk_path_point_count(const cfsk_path* path, size_t* output);
/* @cfsk Path.isEmpty => cfsk_path_is_empty | SkPath::isEmpty */
cfsk_status cfsk_path_is_empty(const cfsk_path* path, uint8_t* output);
/* @cfsk Path.getPoint => cfsk_path_get_point | SkPath::getPoint with checked index */
cfsk_status cfsk_path_get_point(const cfsk_path* path, size_t index, cfsk_point* output);
/* @cfsk Path.contains => cfsk_path_contains | SkPath::contains */
cfsk_status cfsk_path_contains(const cfsk_path* path, float x, float y, uint8_t* output);
/* @cfsk PathConstructorAndFactory.MakeFromSVGString => cfsk_path_from_svg | SkParsePath::FromSVGString into builder, detach */
cfsk_status cfsk_path_from_svg(cfsk_string svg, cfsk_path** output);
/* @cfsk PathConstructorAndFactory.MakeFromCmds => cfsk_path_from_commands | Decode CanvasKit verb+float command stream into SkPathBuilder; validate before detach */
cfsk_status cfsk_path_from_commands(const float* commands, size_t count, cfsk_path** output);
/* @cfsk Path.toCmds => cfsk_path_to_commands | SkPath iteration to CanvasKit verb+float command stream; copied output */
cfsk_status cfsk_path_to_commands(const cfsk_path* path, float* output, size_t capacity, size_t* required);
/* @cfsk Path.toSVGString => cfsk_path_to_svg | SkParsePath::ToSVGString; owned UTF8 data */
cfsk_status cfsk_path_to_svg(const cfsk_path* path, cfsk_data** output);
/* @cfsk PathConstructorAndFactory.MakeFromOp => cfsk_path_op | SkPathOps Op; NEW immutable output */
cfsk_status cfsk_path_op(const cfsk_path* first, const cfsk_path* second, uint32_t op, cfsk_path** output);
/* @cfsk Path.simplify => cfsk_path_simplify | SkPathOps Simplify; NEW immutable output */
cfsk_status cfsk_path_simplify(const cfsk_path* path, cfsk_path** output);
/* @cfsk Path.stroke => cfsk_path_stroke | skpathutils::FillPathWithPaint; NEW output */
cfsk_status cfsk_path_stroke(const cfsk_path* path, float width, uint32_t join, uint32_t cap, float miter_limit, float resolution_scale, cfsk_path** output);
/* @cfsk OffsetPath.makeOffset => cfsk_path_offset | SkPathOffset signed fill-boundary offset (carried fork utility) */
/* @cfsk ForkBindingsPath.makeOffset => cfsk_path_offset | SkPathOffset signed fill-boundary offset (carried fork utility) */
cfsk_status cfsk_path_offset(const cfsk_path* path, float distance, uint32_t join, float miter_limit, float tolerance, cfsk_path** output);
/* @cfsk ForkBindingsPath.makeWithEffect => cfsk_path_with_effect | SkPathEffect::filterPath; NEW output */
/* @cfsk Path.makeWithEffect => cfsk_path_with_effect | SkPathEffect::filterPath; NEW output */
cfsk_status cfsk_path_with_effect(const cfsk_path* path, const cfsk_path_effect* effect, uint32_t style, float resolution_scale, cfsk_path** output);
/* @cfsk CanvasKit.ContourMeasureIter => cfsk_contour_iter_make | SkContourMeasureIter */
cfsk_status cfsk_contour_iter_make(const cfsk_path* path, uint8_t force_closed, float resolution_scale, cfsk_contour_iter** output);
/* @cfsk ContourMeasureIter.next => cfsk_contour_iter_next | SkContourMeasureIter::next; OK/NULL at end */
cfsk_status cfsk_contour_iter_next(cfsk_contour_iter* iterator, cfsk_contour** output);
/* @cfsk ContourMeasure.length => cfsk_contour_length | SkContourMeasure::length */
cfsk_status cfsk_contour_length(const cfsk_contour* contour, float* output);
/* @cfsk ContourMeasure.isClosed => cfsk_contour_is_closed | SkContourMeasure::isClosed */
cfsk_status cfsk_contour_is_closed(const cfsk_contour* contour, uint8_t* output);
/* @cfsk ContourMeasure.getPosTan => cfsk_contour_pos_tan | SkContourMeasure::getPosTan */
cfsk_status cfsk_contour_pos_tan(const cfsk_contour* contour, float distance, cfsk_point* position, cfsk_point* tangent);
/* @cfsk ContourMeasure.getSegment => cfsk_contour_segment | SkContourMeasure::getSegment into builder, detach */
cfsk_status cfsk_contour_segment(const cfsk_contour* contour, float start, float stop, uint8_t start_with_move, cfsk_path** output);

/* Images, uploads and explicit readback */
/* @cfsk CanvasKit.MakeImageFromEncoded => cfsk_image_decode | SkImages::DeferredFromEncodedData then makeRasterImage on IO thread; bytes copied */
cfsk_status cfsk_image_decode(cfsk_bytes encoded, cfsk_image** output);
/* @cfsk CanvasKit.MakeImage => cfsk_image_from_pixels | SkImages::RasterFromPixmapCopy; Rust host materializes source pixels */
/* @cfsk CanvasKit.MakeImageFromCanvasImageSource => cfsk_image_from_pixels | SkImages::RasterFromPixmapCopy; Rust host materializes source pixels */
/* @cfsk CanvasKit.MakeLazyImageFromTextureSource => cfsk_image_from_pixels | SkImages::RasterFromPixmapCopy; Rust host materializes source pixels */
cfsk_status cfsk_image_from_pixels(const cfsk_image_info* info, cfsk_bytes pixels, size_t row_bytes, cfsk_image** output);
/* @cfsk Surface.makeImageFromImage => cfsk_image_upload | SkImages::TextureFromImage(recorder, source, RequiredProperties{mipmapped}) */
cfsk_status cfsk_image_upload(cfsk_recorder* recorder, const cfsk_image* source, uint8_t mipmapped, cfsk_image** output);
/* @cfsk Surface.makeImageFromTexture => cfsk_image_import_texture | SkImages::WrapTexture with retained Dawn BackendTexture; no GL ids */
/* @cfsk Surface.makeImageFromTextureSource => cfsk_image_import_texture | SkImages::WrapTexture with retained Dawn BackendTexture; no GL ids */
cfsk_status cfsk_image_import_texture(cfsk_recorder* recorder, void* dawn_texture, const cfsk_image_info* info, uint32_t origin, uint8_t generate_mipmaps, cfsk_image** output);
/* @cfsk Image.getImageInfo => cfsk_image_get_info | SkImage::imageInfo; output color_space borrowed from image */
/* @cfsk Image.width => cfsk_image_get_info | SkImage::imageInfo; output color_space borrowed from image */
/* @cfsk Image.height => cfsk_image_get_info | SkImage::imageInfo; output color_space borrowed from image */
cfsk_status cfsk_image_get_info(const cfsk_image* image, cfsk_image_info* output);
/* @cfsk Image.hasMipmaps => cfsk_image_has_mipmaps | SkImage::hasMipmaps */
cfsk_status cfsk_image_has_mipmaps(const cfsk_image* image, uint8_t* output);
/* @cfsk Image.textureSize => cfsk_image_texture_size | SkImage::textureSize; zero for CPU image */
cfsk_status cfsk_image_texture_size(const cfsk_image* image, size_t* output_bytes);
/* @cfsk Image.readPixels => cfsk_image_read_pixels | CPU SkImage::readPixels; GPU uses image_readback */
cfsk_status cfsk_image_read_pixels(const cfsk_image* image, const cfsk_image_info* info, int32_t source_x, int32_t source_y, void* output, size_t output_bytes, size_t row_bytes);
/* @cfsk Graphite.Image.readback => cfsk_image_readback | Context::asyncRescaleAndReadPixels(image); request + poll */
cfsk_status cfsk_image_readback(cfsk_context* context, const cfsk_image* image, const cfsk_image_info* info, cfsk_irect source, cfsk_readback** output);
/* @cfsk Image.encodeToBytes => cfsk_image_encode | SkPngEncoder/SkJpegEncoder/SkWebpEncoder on CPU pixels; explicit GPU readback first */
cfsk_status cfsk_image_encode(const cfsk_image* image, uint32_t format, int32_t quality, cfsk_data** output);
/* @cfsk Image.makeShaderCubic => cfsk_image_make_shader | SkImage::makeShader; NULL matrix or 9 row-major floats */
/* @cfsk Image.makeShaderOptions => cfsk_image_make_shader | SkImage::makeShader; NULL matrix or 9 row-major floats */
cfsk_status cfsk_image_make_shader(const cfsk_image* image, uint32_t tile_x, uint32_t tile_y, cfsk_sampling sampling, const float* local_matrix, cfsk_shader** output);
/* @cfsk CanvasKit.ownedBytes => cfsk_data_copy | Copy encoded/serialized SkData to caller-owned buffer */
cfsk_status cfsk_data_copy(const cfsk_data* data, uint8_t* output, size_t capacity, size_t* required);

/* Runtime effects, gradients, image filters, carried fork effects */
/* @cfsk RuntimeEffectFactory.Make => cfsk_runtime_effect_make | SkRuntimeEffect::MakeForShader/ColorFilter/Blender; kind=0/1/2; optional synchronous error handler, never retained */
/* @cfsk RuntimeEffectFactory.MakeForBlender => cfsk_runtime_effect_make | SkRuntimeEffect::MakeForShader/ColorFilter/Blender; kind=0/1/2; optional synchronous error handler, never retained */
/* @cfsk BindingsApiStatics.RuntimeEffect.MakeForColorFilter => cfsk_runtime_effect_make | SkRuntimeEffect::MakeForShader/ColorFilter/Blender; kind=0/1/2; optional synchronous error handler, never retained */
/* @cfsk ForkBindingsStatics.RuntimeEffect.Make => cfsk_runtime_effect_make | SkRuntimeEffect::MakeForShader/ColorFilter/Blender; kind=0/1/2; optional synchronous error handler, never retained */
cfsk_status cfsk_runtime_effect_make(cfsk_string sksl, uint32_t kind, cfsk_shader_error_handler on_error, void* user, cfsk_runtime_effect** output);
/* @cfsk RuntimeEffect.getUniformCount => cfsk_runtime_effect_uniform_count | SkRuntimeEffect uniform metadata; float count = uniformSize/4 */
cfsk_status cfsk_runtime_effect_uniform_count(const cfsk_runtime_effect* effect, size_t* output);
/* @cfsk RuntimeEffect.getUniformFloatCount => cfsk_runtime_effect_uniform_float_count | SkRuntimeEffect uniform metadata; float count = uniformSize/4 */
cfsk_status cfsk_runtime_effect_uniform_float_count(const cfsk_runtime_effect* effect, size_t* output);
/* @cfsk RuntimeEffect.getUniform => cfsk_runtime_effect_uniform | Checked SkRuntimeEffect::uniforms; name borrowed until effect destroy */
/* @cfsk RuntimeEffect.getUniformName => cfsk_runtime_effect_uniform | Checked SkRuntimeEffect::uniforms; name borrowed until effect destroy */
cfsk_status cfsk_runtime_effect_uniform(const cfsk_runtime_effect* effect, size_t index, cfsk_uniform* output);
/* @cfsk RuntimeEffect.makeShader => cfsk_runtime_effect_shader | SkRuntimeEffect::makeShader; exact byte size; typed child order */
/* @cfsk RuntimeEffect.makeShaderWithChildren => cfsk_runtime_effect_shader | SkRuntimeEffect::makeShader; exact byte size; typed child order */
cfsk_status cfsk_runtime_effect_shader(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, const cfsk_shader* const* children, size_t child_count, const float* local_matrix, cfsk_shader** output);
/* @cfsk RuntimeEffect.makeBlender => cfsk_runtime_effect_blender | SkRuntimeEffect::makeBlender */
cfsk_status cfsk_runtime_effect_blender(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, cfsk_blender** output);
/* @cfsk ColorFilterEffect.makeColorFilter => cfsk_runtime_effect_color_filter | SkRuntimeEffect::makeColorFilter */
cfsk_status cfsk_runtime_effect_color_filter(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, const cfsk_shader* const* children, size_t child_count, cfsk_color_filter** output);
/* @cfsk ForkBindingsRuntimeEffect.makeBuilder => cfsk_runtime_effect_builder_make | SkRuntimeShaderBuilder; uniform bytes copied */
cfsk_status cfsk_runtime_effect_builder_make(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, cfsk_runtime_effect_builder** output);
/* @cfsk RuntimeEffectBuilder.setUniforms => cfsk_runtime_effect_builder_uniforms | Validate and copy complete uniform block */
cfsk_status cfsk_runtime_effect_builder_uniforms(cfsk_runtime_effect_builder* builder, cfsk_bytes uniforms);
/* @cfsk RuntimeEffectBuilder.setChild => cfsk_runtime_effect_builder_child | SkRuntimeShaderBuilder::child(name); retains shader, NULL clears */
cfsk_status cfsk_runtime_effect_builder_child(cfsk_runtime_effect_builder* builder, cfsk_string name, const cfsk_shader* shader);
/* @cfsk ShaderFactory.MakeColor => cfsk_shader_color | SkShaders::Color */
cfsk_status cfsk_shader_color(cfsk_color color, const cfsk_color_space* space, cfsk_shader** output);
/* @cfsk ShaderFactory.MakeBlend => cfsk_shader_blend | SkShaders::Blend */
cfsk_status cfsk_shader_blend(uint32_t mode, const cfsk_shader* destination, const cfsk_shader* source, cfsk_shader** output);
/* @cfsk ShaderFactory.MakeLinearGradient => cfsk_shader_gradient_linear | SkShaders::LinearGradient with SkGradient (m157); full float color/interpolation contract */
/* @cfsk ForkBindingsStatics.Shader.MakeLinearGradient => cfsk_shader_gradient_linear | SkShaders::LinearGradient with SkGradient (m157); full float color/interpolation contract */
cfsk_status cfsk_shader_gradient_linear(cfsk_point start, cfsk_point end, const cfsk_gradient* gradient, cfsk_shader** output);
/* @cfsk ShaderFactory.MakeRadialGradient => cfsk_shader_gradient_radial | SkShaders::RadialGradient with SkGradient (m157); full float color/interpolation contract */
/* @cfsk ForkBindingsStatics.Shader.MakeRadialGradient => cfsk_shader_gradient_radial | SkShaders::RadialGradient with SkGradient (m157); full float color/interpolation contract */
cfsk_status cfsk_shader_gradient_radial(cfsk_point center, float radius, const cfsk_gradient* gradient, cfsk_shader** output);
/* @cfsk ShaderFactory.MakeTwoPointConicalGradient => cfsk_shader_gradient_conical | SkShaders::TwoPointConicalGradient with SkGradient (m157); full float color/interpolation contract */
/* @cfsk ForkBindingsStatics.Shader.MakeTwoPointConicalGradient => cfsk_shader_gradient_conical | SkShaders::TwoPointConicalGradient with SkGradient (m157); full float color/interpolation contract */
cfsk_status cfsk_shader_gradient_conical(cfsk_point start, float start_radius, cfsk_point end, float end_radius, const cfsk_gradient* gradient, cfsk_shader** output);
/* @cfsk ShaderFactory.MakeSweepGradient => cfsk_shader_gradient_sweep | SkShaders::SweepGradient with SkGradient (m157); full float color/interpolation contract */
/* @cfsk ForkBindingsStatics.Shader.MakeSweepGradient => cfsk_shader_gradient_sweep | SkShaders::SweepGradient with SkGradient (m157); full float color/interpolation contract */
cfsk_status cfsk_shader_gradient_sweep(cfsk_point center, float start_degrees, float end_degrees, const cfsk_gradient* gradient, cfsk_shader** output);
/* @cfsk ShaderFactory.MakeFractalNoise => cfsk_shader_noise | SkShaders::MakeFractalNoise/MakeTurbulence; kind=0/1 */
/* @cfsk ShaderFactory.MakeTurbulence => cfsk_shader_noise | SkShaders::MakeFractalNoise/MakeTurbulence; kind=0/1 */
cfsk_status cfsk_shader_noise(uint32_t kind, float frequency_x, float frequency_y, int32_t octaves, float seed, int32_t tile_width, int32_t tile_height, cfsk_shader** output);
/* @cfsk ColorFilterFactory.MakeCompose => cfsk_color_filter_compose | SkColorFilters::Compose */
cfsk_status cfsk_color_filter_compose(const cfsk_color_filter* outer, const cfsk_color_filter* inner, cfsk_color_filter** output);
/* @cfsk ColorFilterFactory.MakeBlend => cfsk_color_filter_blend | SkColorFilters::Blend */
cfsk_status cfsk_color_filter_blend(cfsk_color color, const cfsk_color_space* space, uint32_t mode, cfsk_color_filter** output);
/* @cfsk ColorFilterFactory.MakeMatrix => cfsk_color_filter_matrix | SkColorFilters::Matrix */
cfsk_status cfsk_color_filter_matrix(const float matrix[20], cfsk_color_filter** output);
/* @cfsk ColorFilterFactory.MakeLuma => cfsk_color_filter_luma | SkLumaColorFilter::Make */
cfsk_status cfsk_color_filter_luma(cfsk_color_filter** output);
/* @cfsk ColorFilterFactory.MakeSRGBToLinearGamma => cfsk_color_filter_srgb_to_linear | SkColorFilters::SRGBToLinearGamma */
cfsk_status cfsk_color_filter_srgb_to_linear(cfsk_color_filter** output);
/* @cfsk ColorFilterFactory.MakeLinearToSRGBGamma => cfsk_color_filter_linear_to_srgb | SkColorFilters::LinearToSRGBGamma */
cfsk_status cfsk_color_filter_linear_to_srgb(cfsk_color_filter** output);
/* @cfsk CanvasKit.ColorSpace.SRGB => cfsk_color_space_srgb | SkColorSpace::MakeSRGB/MakeSRGBLinear */
/* @cfsk CanvasKit.ColorSpace.SRGB_LINEAR => cfsk_color_space_srgb | SkColorSpace::MakeSRGB/MakeSRGBLinear */
/* @cfsk CompositingApi.srgbLinear => cfsk_color_space_srgb | SkColorSpace::MakeSRGB/MakeSRGBLinear */
cfsk_status cfsk_color_space_srgb(uint8_t linear, cfsk_color_space** output);
/* @cfsk HdrColorApi.makeRgb => cfsk_color_space_rgb | SkColorSpace::MakeRGB with skcms_TransferFunction and row-major gamut */
/* @cfsk HdrColorStatics.ColorSpace.MakeRGB => cfsk_color_space_rgb | SkColorSpace::MakeRGB with skcms_TransferFunction and row-major gamut */
cfsk_status cfsk_color_space_rgb(const float transfer_fn[7], const float gamut[9], cfsk_color_space** output);
/* @cfsk ColorSpaceEnumValues.Equals => cfsk_color_space_equals | SkColorSpace::Equals */
cfsk_status cfsk_color_space_equals(const cfsk_color_space* first, const cfsk_color_space* second, uint8_t* output);
/* @cfsk HdrColorApi.makeColorSpaceXform => cfsk_color_filter_color_space_xform | SkColorFilterPriv::MakeColorSpaceXform (inside Skia tree) */
/* @cfsk HdrColorStatics.ColorFilter.MakeColorSpaceXform => cfsk_color_filter_color_space_xform | SkColorFilterPriv::MakeColorSpaceXform (inside Skia tree) */
cfsk_status cfsk_color_filter_color_space_xform(const cfsk_color_space* source, const cfsk_color_space* destination, cfsk_color_filter** output);
/* @cfsk ImageFilterFactory.MakeBlur => cfsk_image_filter_blur | SkImageFilters::Blur */
cfsk_status cfsk_image_filter_blur(float sigma_x, float sigma_y, uint32_t tile_mode, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeBlend => cfsk_image_filter_blend | SkImageFilters::Blend */
cfsk_status cfsk_image_filter_blend(uint32_t mode, const cfsk_image_filter* background, const cfsk_image_filter* foreground, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeColorFilter => cfsk_image_filter_color_filter | SkImageFilters::ColorFilter */
cfsk_status cfsk_image_filter_color_filter(const cfsk_color_filter* filter, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeCompose => cfsk_image_filter_compose | SkImageFilters::Compose */
cfsk_status cfsk_image_filter_compose(const cfsk_image_filter* outer, const cfsk_image_filter* inner, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeDilate => cfsk_image_filter_dilate | SkImageFilters::Dilate */
cfsk_status cfsk_image_filter_dilate(float radius_x, float radius_y, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeErode => cfsk_image_filter_erode | SkImageFilters::Erode */
cfsk_status cfsk_image_filter_erode(float radius_x, float radius_y, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeDisplacementMap => cfsk_image_filter_displacement | SkImageFilters::DisplacementMap */
cfsk_status cfsk_image_filter_displacement(uint32_t channel_x, uint32_t channel_y, float scale, const cfsk_image_filter* displacement, const cfsk_image_filter* color, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeDropShadow => cfsk_image_filter_shadow | SkImageFilters::DropShadow/DropShadowOnly SkColor4f overload (HDR preserved) */
/* @cfsk ImageFilterFactory.MakeDropShadowOnly => cfsk_image_filter_shadow | SkImageFilters::DropShadow/DropShadowOnly SkColor4f overload (HDR preserved) */
cfsk_status cfsk_image_filter_shadow(float dx, float dy, float sigma_x, float sigma_y, cfsk_color color, const cfsk_color_space* space, uint8_t shadow_only, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeMatrixTransform => cfsk_image_filter_matrix | SkImageFilters::MatrixTransform */
cfsk_status cfsk_image_filter_matrix(const float matrix[9], cfsk_sampling sampling, const cfsk_image_filter* input, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeOffset => cfsk_image_filter_offset | SkImageFilters::Offset */
cfsk_status cfsk_image_filter_offset(float dx, float dy, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ImageFilterFactory.MakeShader => cfsk_image_filter_shader | SkImageFilters::Shader */
cfsk_status cfsk_image_filter_shader(const cfsk_shader* shader, uint8_t dither, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk BindingsApi.makeArithmetic => cfsk_image_filter_arithmetic | SkImageFilters::Arithmetic */
/* @cfsk BindingsStatics.ImageFilter.MakeArithmetic => cfsk_image_filter_arithmetic | SkImageFilters::Arithmetic */
cfsk_status cfsk_image_filter_arithmetic(float k1, float k2, float k3, float k4, uint8_t enforce_premul, const cfsk_image_filter* background, const cfsk_image_filter* foreground, const cfsk_rect* crop, cfsk_image_filter** output);
/* @cfsk ForkBindingsStatics.ImageFilter.MakeCrop => cfsk_image_filter_crop | SkImageFilters::Crop */
cfsk_status cfsk_image_filter_crop(cfsk_rect rect, uint32_t tile_mode, const cfsk_image_filter* input, cfsk_image_filter** output);
/* @cfsk ForkBindingsStatics.ImageFilter.MakeRuntimeShader => cfsk_image_filter_runtime | SkImageFilters::RuntimeShader; device-resolution overload is carried fork policy */
/* @cfsk ForkBindingsStatics.ImageFilter.MakeRuntimeShaderAtDeviceResolution => cfsk_image_filter_runtime | SkImageFilters::RuntimeShader; device-resolution overload is carried fork policy */
cfsk_status cfsk_image_filter_runtime(const cfsk_runtime_effect_builder* builder, const cfsk_filter_child* children, size_t child_count, float sample_radius, uint8_t device_resolution, cfsk_image_filter** output);
/* @cfsk MotionBlurApi.transformBlur => cfsk_image_filter_transform_blur | SkImageFilters::TransformBlur; 2..16 row-major matrices; no TS shutter maths in C */
/* @cfsk MotionBlurStatics.ImageFilter.MakeTransformBlur => cfsk_image_filter_transform_blur | SkImageFilters::TransformBlur; 2..16 row-major matrices; no TS shutter maths in C */
/* Valid still/singular source identity: CFSK_OK with a NULL output filter. */
cfsk_status cfsk_image_filter_transform_blur(const float* matrices44, size_t matrix_count, int32_t minimum_samples, int32_t maximum_samples, const cfsk_image_filter* input, cfsk_image_filter** output);
/* @cfsk MaskFilterFactory.MakeBlur => cfsk_mask_filter_blur | SkMaskFilter::MakeBlur */
cfsk_status cfsk_mask_filter_blur(uint32_t style, float sigma, uint8_t respect_ctm, cfsk_mask_filter** output);
/* @cfsk PathEffectFactory.MakeCorner => cfsk_path_effect_corner | SkCornerPathEffect::Make */
cfsk_status cfsk_path_effect_corner(float radius, cfsk_path_effect** output);
/* @cfsk PathEffectFactory.MakeDash => cfsk_path_effect_dash | SkDashPathEffect::Make */
cfsk_status cfsk_path_effect_dash(const float* intervals, size_t count, float phase, cfsk_path_effect** output);
/* @cfsk PathEffectFactory.MakePath1D => cfsk_path_effect_path1d | SkPath1DPathEffect::Make */
cfsk_status cfsk_path_effect_path1d(const cfsk_path* path, float advance, float phase, uint32_t style, cfsk_path_effect** output);
/* @cfsk PathEffectFactory.MakeTrim => cfsk_path_effect_trim | SkTrimPathEffect::Make */
/* @cfsk ForkBindingsStatics.PathEffect.MakeTrim => cfsk_path_effect_trim | SkTrimPathEffect::Make */
cfsk_status cfsk_path_effect_trim(float start, float end, uint32_t mode, cfsk_path_effect** output);
/* @cfsk CompositingApi.makeAdd => cfsk_blender_add | SkBlenders::Add; Graphite fixed-function mapping is a separate backend change */
/* @cfsk CompositingBindings.Blender.MakeAdd => cfsk_blender_add | SkBlenders::Add; Graphite fixed-function mapping is a separate backend change */
cfsk_status cfsk_blender_add(cfsk_blender** output);
/* @cfsk MinBlendApi.makeMin => cfsk_blender_min | SkBlenders::Min; Graphite fixed-function mapping is a separate backend change */
/* @cfsk MinBlendStatics.Blender.MakeMin => cfsk_blender_min | SkBlenders::Min; Graphite fixed-function mapping is a separate backend change */
cfsk_status cfsk_blender_min(cfsk_blender** output);
/* @cfsk NativeEffectsApi.makeLightResolve => cfsk_blender_light_resolve | SkBlenders::LightResolve/LayerResolve; kind=0 light/1 layer, spaces=0 encoded/1 linear */
/* @cfsk NativeEffectsStatics.Blender.MakeLightResolve => cfsk_blender_light_resolve | SkBlenders::LightResolve/LayerResolve; kind=0 light/1 layer, spaces=0 encoded/1 linear */
cfsk_status cfsk_blender_light_resolve(uint32_t kind, uint32_t into_space, uint32_t light_space, uint8_t emissive, cfsk_blender** output);
/* @cfsk NativeEffectsApi.makeGlow => cfsk_shader_glow | SkGlowShader::Make; lobe stride10, core NULL/13/15 floats as fork contract */
/* @cfsk NativeEffectsStatics.Shader.MakeGlow => cfsk_shader_glow | SkGlowShader::Make; lobe stride10, core NULL/13/15 floats as fork contract */
cfsk_status cfsk_shader_glow(const float* lobes, size_t lobe_float_count, const float* core, size_t core_float_count, cfsk_shader** output);
/* @cfsk NativeEffectsApi.makeRadialGlow => cfsk_shader_radial_glow | SkGlowShader::MakeRadial; tint is premultiplied */
/* @cfsk NativeEffectsStatics.Shader.MakeRadialGlow => cfsk_shader_radial_glow | SkGlowShader::MakeRadial; tint is premultiplied */
cfsk_status cfsk_shader_radial_glow(const cfsk_image* profile, int32_t length, float step, cfsk_color tint, const float* core, size_t core_float_count, cfsk_shader** output);
/* @cfsk NativeEffectsApi.outputDecode => cfsk_color_filter_output_decode | SkOutputTransform::Decode */
/* @cfsk NativeEffectsStatics.ColorFilter.MakeOutputDecode => cfsk_color_filter_output_decode | SkOutputTransform::Decode */
cfsk_status cfsk_color_filter_output_decode(cfsk_color_filter** output);
/* @cfsk NativeEffectsApi.outputEncode => cfsk_color_filter_output_encode | SkOutputTransform::Encode */
/* @cfsk NativeEffectsStatics.ColorFilter.MakeOutputEncode => cfsk_color_filter_output_encode | SkOutputTransform::Encode */
cfsk_status cfsk_color_filter_output_encode(cfsk_color_filter** output);
/* @cfsk NativeEffectsApi.toneMap => cfsk_color_filter_tone_map | SkOutputTransform::ToneMap; curves=linear/reinhard/cineon/aces/agx/neutral ordinals 0..5 */
/* @cfsk NativeEffectsStatics.ColorFilter.MakeToneMap => cfsk_color_filter_tone_map | SkOutputTransform::ToneMap; curves=linear/reinhard/cineon/aces/agx/neutral ordinals 0..5 */
cfsk_status cfsk_color_filter_tone_map(uint32_t curve, float exposure, cfsk_color_filter** output);
/* @cfsk NativeEffectsApi.grade => cfsk_color_filter_grade | SkOutputTransform::Grade; sepia is float31, no color rounding */
/* @cfsk NativeEffectsStatics.ColorFilter.MakeGrade => cfsk_color_filter_grade | SkOutputTransform::Grade; sepia is float31, no color rounding */
cfsk_status cfsk_color_filter_grade(const float uniforms[31], const cfsk_image* curves, cfsk_color_filter** output);
/* @cfsk NativeEffectsApi.lutStrip => cfsk_color_filter_lut_strip | SkOutputTransform::LutStrip */
/* @cfsk NativeEffectsStatics.ColorFilter.MakeLutStrip => cfsk_color_filter_lut_strip | SkOutputTransform::LutStrip */
cfsk_status cfsk_color_filter_lut_strip(const cfsk_image* strip, int32_t size, float intensity, const float domain_min[3], const float domain_max[3], cfsk_color_filter** output);

/* Fonts from bytes, paragraphs and pictures */
/* @cfsk TypefaceFactory.MakeFreeTypeFaceFromData => cfsk_typeface_from_data | SkTypeface from pinned FreeType font manager; copied bytes, no host font lookup */
cfsk_status cfsk_typeface_from_data(cfsk_bytes bytes, int32_t collection_index, cfsk_typeface** output);
/* @cfsk Typeface.getVariationAxes => cfsk_typeface_variation_axes | SkTypeface::getVariationDesignParameters */
cfsk_status cfsk_typeface_variation_axes(const cfsk_typeface* face, cfsk_axis* output, size_t capacity, size_t* required);
/* @cfsk Typeface.makeVariation => cfsk_typeface_make_variation | SkTypeface::makeClone(SkFontArguments) */
cfsk_status cfsk_typeface_make_variation(const cfsk_typeface* face, const cfsk_axis_coordinate* coordinates, size_t count, cfsk_typeface** output);
/* @cfsk CanvasKit.Font => cfsk_font_make | SkFont(face,size,scaleX,skewX); face nullable for explicit empty default */
/* @cfsk ForkBindingsStatics.Font => cfsk_font_make | SkFont(face,size,scaleX,skewX); face nullable for explicit empty default */
cfsk_status cfsk_font_make(const cfsk_typeface* face, float size, float scale_x, float skew_x, cfsk_font** output);
/* @cfsk Font.setSize => cfsk_font_set_size | SkFont::setSize */
cfsk_status cfsk_font_set_size(cfsk_font* font, float value);
/* @cfsk Font.setSkewX => cfsk_font_set_skew_x | SkFont::setSkewX */
cfsk_status cfsk_font_set_skew_x(cfsk_font* font, float value);
/* @cfsk Font.setEdging => cfsk_font_set_edging | SkFont::setEdging */
cfsk_status cfsk_font_set_edging(cfsk_font* font, uint32_t value);
/* @cfsk Font.setHinting => cfsk_font_set_hinting | SkFont::setHinting */
cfsk_status cfsk_font_set_hinting(cfsk_font* font, uint32_t value);
/* @cfsk Font.setEmbolden => cfsk_font_set_embolden | SkFont::setEmbolden */
cfsk_status cfsk_font_set_embolden(cfsk_font* font, uint8_t value);
/* @cfsk Font.setLinearMetrics => cfsk_font_set_linear_metrics | SkFont::setLinearMetrics */
cfsk_status cfsk_font_set_linear_metrics(cfsk_font* font, uint8_t value);
/* @cfsk Font.setSubpixel => cfsk_font_set_subpixel | SkFont::setSubpixel */
cfsk_status cfsk_font_set_subpixel(cfsk_font* font, uint8_t value);
/* @cfsk Font.getGlyphIDs => cfsk_font_glyph_ids | SkFont::textToGlyphs UTF8 */
cfsk_status cfsk_font_glyph_ids(const cfsk_font* font, cfsk_string utf8, uint16_t* output, size_t capacity, size_t* required);
/* @cfsk Font.getGlyphWidths => cfsk_font_glyph_widths | SkFont::getWidthsBounds; output count floats */
cfsk_status cfsk_font_glyph_widths(const cfsk_font* font, const uint16_t* glyphs, size_t count, const cfsk_paint* paint, float* output);
/* @cfsk Font.getGlyphBounds => cfsk_font_glyph_bounds | SkFont::getBounds; output count rects */
cfsk_status cfsk_font_glyph_bounds(const cfsk_font* font, const uint16_t* glyphs, size_t count, const cfsk_paint* paint, cfsk_rect* output);
/* @cfsk Font.getMetrics => cfsk_font_get_metrics | SkFont::getMetrics */
cfsk_status cfsk_font_get_metrics(const cfsk_font* font, cfsk_font_metrics* output);
/* @cfsk ForkBindingsFont.getPaths => cfsk_font_paths | SkFont::getPaths; count outputs, each owned or NULL for outline-less glyph */
/* @cfsk ForkBindingsFont.getPath => cfsk_font_paths | SkFont::getPaths; count outputs, each owned or NULL for outline-less glyph */
/* @cfsk Font.getPaths => cfsk_font_paths | SkFont::getPaths; count outputs, each owned or NULL for outline-less glyph */
/* @cfsk Font.getPath => cfsk_font_paths | SkFont::getPaths; count outputs, each owned or NULL for outline-less glyph */
cfsk_status cfsk_font_paths(const cfsk_font* font, const uint16_t* glyphs, size_t count, cfsk_path** output);
/* @cfsk TypefaceFontProviderFactory.Make => cfsk_font_provider_make | skparagraph::TypefaceFontProvider */
cfsk_status cfsk_font_provider_make(cfsk_font_provider** output);
/* @cfsk TypefaceFontProvider.registerFont => cfsk_font_provider_register_bytes | Decode copied font bytes and registerTypeface */
cfsk_status cfsk_font_provider_register_bytes(cfsk_font_provider* provider, cfsk_bytes bytes, cfsk_string alias);
/* @cfsk TypefaceFontProvider.registerTypeface => cfsk_font_provider_register_typeface | TypefaceFontProvider::registerTypeface; retains face */
/* @cfsk FontMgr.registerTypeface => cfsk_font_provider_register_typeface | TypefaceFontProvider::registerTypeface; retains face */
cfsk_status cfsk_font_provider_register_typeface(cfsk_font_provider* provider, const cfsk_typeface* face, cfsk_string alias);
/* @cfsk FontCollectionFactory.Make => cfsk_font_collection_make | skparagraph::FontCollection; disable system fallback */
cfsk_status cfsk_font_collection_make(cfsk_font_collection** output);
/* @cfsk FontCollection.setDefaultFontManager => cfsk_font_collection_set_provider | FontCollection::setDefaultFontManager; retained explicit byte-backed provider */
cfsk_status cfsk_font_collection_set_provider(cfsk_font_collection* collection, cfsk_font_provider* provider);
/* @cfsk ParagraphBuilderFactory.MakeFromFontCollection => cfsk_paragraph_builder_make | skparagraph::ParagraphBuilder::make; copies style and retains collection */
cfsk_status cfsk_paragraph_builder_make(const cfsk_paragraph_style* style, cfsk_font_collection* fonts, cfsk_paragraph_builder** output);
/* @cfsk ParagraphBuilder.addText => cfsk_paragraph_builder_add_text | ParagraphBuilder::addText UTF8 */
cfsk_status cfsk_paragraph_builder_add_text(cfsk_paragraph_builder* builder, cfsk_string text);
/* @cfsk ParagraphBuilder.build => cfsk_paragraph_builder_build | ParagraphBuilder::Build */
cfsk_status cfsk_paragraph_builder_build(cfsk_paragraph_builder* builder, cfsk_paragraph** output);
/* @cfsk Paragraph.layout => cfsk_paragraph_layout | Paragraph::layout */
cfsk_status cfsk_paragraph_layout(cfsk_paragraph* paragraph, float width);
/* @cfsk Paragraph.getLongestLine => cfsk_paragraph_longest_line | Paragraph::getLongestLine */
cfsk_status cfsk_paragraph_longest_line(const cfsk_paragraph* paragraph, float* output);
/* @cfsk Paragraph.getMaxIntrinsicWidth => cfsk_paragraph_max_intrinsic_width | Paragraph::getMaxIntrinsicWidth */
cfsk_status cfsk_paragraph_max_intrinsic_width(const cfsk_paragraph* paragraph, float* output);
/* @cfsk Paragraph.getLineMetrics => cfsk_paragraph_line_metrics | Paragraph::getLineMetrics; scalar C copies, no std::map */
cfsk_status cfsk_paragraph_line_metrics(const cfsk_paragraph* paragraph, cfsk_line_metrics* output, size_t capacity, size_t* required);
/* @cfsk Paragraph.getShapedLines => cfsk_paragraph_shaped_runs | Paragraph::visit/getShapedLines; copied run metadata; output typefaces owned */
cfsk_status cfsk_paragraph_shaped_runs(const cfsk_paragraph* paragraph, cfsk_shaped_run* output, size_t capacity, size_t* required);
/* @cfsk Paragraph.getShapedLines.glyphs => cfsk_paragraph_run_glyphs | Copy shaped run glyphs/UTF16 clusters; positions and clusters capacity >= glyph capacity+1 */
cfsk_status cfsk_paragraph_run_glyphs(const cfsk_paragraph* paragraph, size_t run_index, uint16_t* glyphs, cfsk_point* positions, uint32_t* clusters, size_t capacity, size_t* required);
/* @cfsk TextBlobFactory.MakeFromRSXformGlyphs => cfsk_text_blob_from_rsxform_glyphs | SkTextBlob::MakeFromRSXform with glyph encoding */
cfsk_status cfsk_text_blob_from_rsxform_glyphs(const uint16_t* glyphs, const cfsk_rsxform* transforms, size_t count, const cfsk_font* font, cfsk_text_blob** output);
/* @cfsk CanvasKit.PictureRecorder => cfsk_picture_recorder_make | SkPictureRecorder */
cfsk_status cfsk_picture_recorder_make(cfsk_picture_recorder** output);
/* @cfsk PictureRecorder.beginRecording => cfsk_picture_recorder_begin | SkPictureRecorder::beginRecording; borrowed until finish/destroy; at most one active */
cfsk_status cfsk_picture_recorder_begin(cfsk_picture_recorder* recorder, cfsk_rect bounds, cfsk_canvas** output);
/* @cfsk PictureRecorder.finishRecordingAsPicture => cfsk_picture_recorder_finish | SkPictureRecorder::finishRecordingAsPicture; invalidates borrowed canvas */
cfsk_status cfsk_picture_recorder_finish(cfsk_picture_recorder* recorder, cfsk_picture** output);
/* @cfsk CanvasKit.MakePicture => cfsk_picture_deserialize | SkPicture::MakeFromData; input borrowed only during call; failure returns NULL output */
cfsk_status cfsk_picture_deserialize(cfsk_bytes bytes, cfsk_picture** output);
/* @cfsk SkPicture.serialize => cfsk_picture_serialize | SkPicture::serialize; lossless CPU images and embedded typefaces; owned data */
cfsk_status cfsk_picture_serialize(const cfsk_picture* picture, cfsk_data** output);
/* @cfsk SkPicture.unref => cfsk_picture_unref | Release owned picture wrapper; NULL allowed; alias of picture_destroy */
void cfsk_picture_unref(cfsk_picture* value);
/* @cfsk SkPicture.approximateBytesUsed => cfsk_picture_bytes_used | SkPicture::approximateBytesUsed */
cfsk_status cfsk_picture_bytes_used(const cfsk_picture* picture, size_t* output_bytes);
/* @cfsk SkPicture.makeShader => cfsk_picture_make_shader | SkPicture::makeShader */
cfsk_status cfsk_picture_make_shader(const cfsk_picture* picture, uint32_t tile_x, uint32_t tile_y, uint32_t filter_mode, const float* local_matrix, const cfsk_rect* tile, cfsk_shader** output);

/* Retained mesh/vertices and width strokes; Graphite draw must refuse if unsupported */
/* @cfsk CanvasKit.MakeVertices => cfsk_vertices_make | SkVertices::MakeCopy; float vertex HDR path belongs to Graphite mesh redo */
cfsk_status cfsk_vertices_make(uint32_t mode, const cfsk_point* positions, const cfsk_point* texcoords, const uint32_t* colors, size_t vertex_count, const uint16_t* indices, size_t index_count, cfsk_vertices** output);
/* @cfsk Vertices.uniqueID => cfsk_vertices_unique_id | SkVertices::uniqueID */
cfsk_status cfsk_vertices_unique_id(const cfsk_vertices* vertices, uint32_t* output);
/* @cfsk ForkBindingsStatics.MeshSpecification.Make => cfsk_mesh_spec_make | SkMeshSpecification::Make; Graphite RenderStep required, never silently accept unsupported draw */
cfsk_status cfsk_mesh_spec_make(const cfsk_mesh_attribute* attributes, size_t attribute_count, size_t stride_bytes, const cfsk_mesh_varying* varyings, size_t varying_count, cfsk_string vertex_sksl, cfsk_string fragment_sksl, const cfsk_color_space* space, uint32_t alpha_type, size_t instance_stride_bytes, cfsk_mesh_spec** output);
/* @cfsk MeshSpecification.uniformSize => cfsk_mesh_spec_uniform_size | SkMeshSpecification::uniformSize */
cfsk_status cfsk_mesh_spec_uniform_size(const cfsk_mesh_spec* spec, size_t* output_bytes);
/* @cfsk ForkBindingsStatics.MakeMeshVertexBuffer => cfsk_mesh_vertex_buffer_make | SkMesh vertex storage; recorder nullable for CPU staging; Graphite upload route */
cfsk_status cfsk_mesh_vertex_buffer_make(cfsk_recorder* recorder, cfsk_bytes bytes, cfsk_mesh_vertex_buffer** output);
/* @cfsk ForkBindingsStatics.MakeMeshIndexBuffer => cfsk_mesh_index_buffer_make | SkMesh index storage; copied input; Graphite upload route */
cfsk_status cfsk_mesh_index_buffer_make(cfsk_recorder* recorder, const uint16_t* indices, size_t count, cfsk_mesh_index_buffer** output);
/* @cfsk MeshVertexBuffer.size => cfsk_mesh_vertex_buffer_size | SkMesh buffer::size */
cfsk_status cfsk_mesh_vertex_buffer_size(const cfsk_mesh_vertex_buffer* buffer, size_t* output_bytes);
/* @cfsk MeshVertexBuffer.update => cfsk_mesh_vertex_buffer_update | Ordered buffer upload; no WebGL callback or raw GL allocation */
cfsk_status cfsk_mesh_vertex_buffer_update(cfsk_recorder* recorder, cfsk_mesh_vertex_buffer* buffer, size_t offset_bytes, cfsk_bytes bytes);
/* @cfsk MeshIndexBuffer.size => cfsk_mesh_index_buffer_size | SkMesh buffer::size */
cfsk_status cfsk_mesh_index_buffer_size(const cfsk_mesh_index_buffer* buffer, size_t* output_bytes);
/* @cfsk MeshIndexBuffer.update => cfsk_mesh_index_buffer_update | Ordered buffer upload; no WebGL callback or raw GL allocation */
cfsk_status cfsk_mesh_index_buffer_update(cfsk_recorder* recorder, cfsk_mesh_index_buffer* buffer, size_t offset_bytes, cfsk_bytes bytes);
/* @cfsk ForkBindingsStatics.MakeMesh => cfsk_mesh_make | SkMesh::Make/MakeIndexed; validates supported Graphite route before success */
cfsk_status cfsk_mesh_make(const cfsk_mesh_desc* descriptor, cfsk_mesh** output);
/* @cfsk ForkBindingsCanvas.drawMesh => cfsk_canvas_draw_mesh | SkCanvas::drawMesh only with Graphite mesh RenderStep; otherwise UNSUPPORTED */
cfsk_status cfsk_canvas_draw_mesh(cfsk_canvas* canvas, const cfsk_mesh* mesh, uint32_t blend_mode, const cfsk_paint* paint);
/* @cfsk ForkBindingsStatics.MakeWidthStroke => cfsk_width_stroke_make | SkWidthStroke::Make; retained carried fork geometry */
cfsk_status cfsk_width_stroke_make(const float* rows, size_t float_count, const cfsk_width_stroke_options* options, cfsk_width_stroke** output);
/* @cfsk WidthStroke.vertices => cfsk_width_stroke_vertices | SkWidthStroke::vertices; owned reference, checked chunk index */
cfsk_status cfsk_width_stroke_vertices(const cfsk_width_stroke* stroke, size_t chunk, cfsk_vertices** output);
/* @cfsk WidthStroke.bounds => cfsk_width_stroke_bounds | SkWidthStroke::bounds */
cfsk_status cfsk_width_stroke_bounds(const cfsk_width_stroke* stroke, cfsk_rect* output);
/* @cfsk WidthStroke.vertexCount => cfsk_width_stroke_vertex_count | SkWidthStroke::vertexCount */
cfsk_status cfsk_width_stroke_vertex_count(const cfsk_width_stroke* stroke, size_t* output);
/* @cfsk WidthStroke.chunks => cfsk_width_stroke_chunks | Copy SkWidthStroke chunks; NULL/0 gets count */
/* @cfsk WidthStroke.chunkCount => cfsk_width_stroke_chunks | Copy SkWidthStroke chunks; NULL/0 gets count */
cfsk_status cfsk_width_stroke_chunks(const cfsk_width_stroke* stroke, cfsk_stroke_chunk* output, size_t capacity, size_t* required);
/* @cfsk WidthStroke.batches => cfsk_width_stroke_batches | Copy SkWidthStroke::batches */
cfsk_status cfsk_width_stroke_batches(const cfsk_width_stroke* stroke, cfsk_stroke_range* output, size_t capacity, size_t* required);
/* @cfsk WidthStroke.depositCuts => cfsk_width_stroke_deposit_cuts | Copy SkWidthStroke::depositCuts */
cfsk_status cfsk_width_stroke_deposit_cuts(const cfsk_width_stroke* stroke, uint32_t* output, size_t capacity, size_t* required);
/* @cfsk WidthStroke.geometry => cfsk_width_stroke_geometry | Copy diagnostic positions/uv/u32 indices; BUFFER_TOO_SMALL writes required counts only */
cfsk_status cfsk_width_stroke_geometry(const cfsk_width_stroke* stroke, cfsk_point* positions, cfsk_point* uv, size_t vertex_capacity, uint32_t* indices, size_t index_capacity, size_t* required_vertices, size_t* required_indices);

/* Skottie: explicit assets, no JS asset loader or retained logging callbacks */
typedef struct cfsk_skottie cfsk_skottie;
/* @cfsk CanvasKit.MakeManagedAnimation => cfsk_skottie_make | skottie::Animation::Builder with copied named assets; logs through error_copy; no audio runtime (host muxes) */
/* @cfsk SkottieApi.MakeManagedAnimation => cfsk_skottie_make | skottie::Animation::Builder with copied named assets; logs through error_copy; no audio runtime (host muxes) */
cfsk_status cfsk_skottie_make(cfsk_string json, const cfsk_asset* assets, size_t asset_count, cfsk_skottie** output);
/* @cfsk SkottieAnimation.seekFrame => cfsk_skottie_seek_frame | skottie::Animation::seekFrame; frame is continuous, never quantized */
cfsk_status cfsk_skottie_seek_frame(cfsk_skottie* animation, double frame, cfsk_rect* damage);
/* @cfsk SkottieAnimation.render => cfsk_skottie_render | skottie::Animation::render */
cfsk_status cfsk_skottie_render(cfsk_skottie* animation, cfsk_canvas* canvas, const cfsk_rect* destination);

/* Typed ownership (including inherited EmbindObject.delete) */
/* @cfsk GrDirectContext.delete => cfsk_context_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_context_destroy(cfsk_context* value);
/* @cfsk Graphite.Recorder.delete => cfsk_recorder_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_recorder_destroy(cfsk_recorder* value);
/* @cfsk Graphite.Recording.delete => cfsk_recording_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_recording_destroy(cfsk_recording* value);
/* @cfsk Graphite.Submission.delete => cfsk_submission_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_submission_destroy(cfsk_submission* value);
/* @cfsk Graphite.Readback.delete => cfsk_readback_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_readback_destroy(cfsk_readback* value);
/* @cfsk Surface.delete => cfsk_surface_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_surface_destroy(cfsk_surface* value);
/* @cfsk Paint.delete => cfsk_paint_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_paint_destroy(cfsk_paint* value);
/* @cfsk Path.delete => cfsk_path_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_path_destroy(cfsk_path* value);
/* @cfsk PathBuilder.delete => cfsk_path_builder_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_path_builder_destroy(cfsk_path_builder* value);
/* @cfsk Image.delete => cfsk_image_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_image_destroy(cfsk_image* value);
/* @cfsk Shader.delete => cfsk_shader_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_shader_destroy(cfsk_shader* value);
/* @cfsk RuntimeEffect.delete => cfsk_runtime_effect_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_runtime_effect_destroy(cfsk_runtime_effect* value);
/* @cfsk RuntimeEffectBuilder.delete => cfsk_runtime_effect_builder_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_runtime_effect_builder_destroy(cfsk_runtime_effect_builder* value);
/* @cfsk Blender.delete => cfsk_blender_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_blender_destroy(cfsk_blender* value);
/* @cfsk ColorFilter.delete => cfsk_color_filter_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_color_filter_destroy(cfsk_color_filter* value);
/* @cfsk ImageFilter.delete => cfsk_image_filter_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_image_filter_destroy(cfsk_image_filter* value);
/* @cfsk MaskFilter.delete => cfsk_mask_filter_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_mask_filter_destroy(cfsk_mask_filter* value);
/* @cfsk PathEffect.delete => cfsk_path_effect_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_path_effect_destroy(cfsk_path_effect* value);
/* @cfsk SkPicture.delete => cfsk_picture_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_picture_destroy(cfsk_picture* value);
/* @cfsk PictureRecorder.delete => cfsk_picture_recorder_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_picture_recorder_destroy(cfsk_picture_recorder* value);
/* @cfsk Font.delete => cfsk_font_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_font_destroy(cfsk_font* value);
/* @cfsk Typeface.delete => cfsk_typeface_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_typeface_destroy(cfsk_typeface* value);
/* @cfsk TypefaceFontProvider.delete => cfsk_font_provider_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_font_provider_destroy(cfsk_font_provider* value);
/* @cfsk FontCollection.delete => cfsk_font_collection_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_font_collection_destroy(cfsk_font_collection* value);
/* @cfsk ParagraphBuilder.delete => cfsk_paragraph_builder_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_paragraph_builder_destroy(cfsk_paragraph_builder* value);
/* @cfsk Paragraph.delete => cfsk_paragraph_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_paragraph_destroy(cfsk_paragraph* value);
/* @cfsk TextBlob.delete => cfsk_text_blob_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_text_blob_destroy(cfsk_text_blob* value);
/* @cfsk Vertices.delete => cfsk_vertices_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_vertices_destroy(cfsk_vertices* value);
/* @cfsk ContourMeasureIter.delete => cfsk_contour_iter_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_contour_iter_destroy(cfsk_contour_iter* value);
/* @cfsk ContourMeasure.delete => cfsk_contour_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_contour_destroy(cfsk_contour* value);
/* @cfsk MeshSpecification.delete => cfsk_mesh_spec_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_mesh_spec_destroy(cfsk_mesh_spec* value);
/* @cfsk MeshVertexBuffer.delete => cfsk_mesh_vertex_buffer_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_mesh_vertex_buffer_destroy(cfsk_mesh_vertex_buffer* value);
/* @cfsk MeshIndexBuffer.delete => cfsk_mesh_index_buffer_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_mesh_index_buffer_destroy(cfsk_mesh_index_buffer* value);
/* @cfsk Mesh.delete => cfsk_mesh_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_mesh_destroy(cfsk_mesh* value);
/* @cfsk WidthStroke.delete => cfsk_width_stroke_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_width_stroke_destroy(cfsk_width_stroke* value);
/* @cfsk ColorSpace.delete => cfsk_color_space_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_color_space_destroy(cfsk_color_space* value);
/* @cfsk SkData.delete => cfsk_data_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_data_destroy(cfsk_data* value);
/* @cfsk ManagedSkottieAnimation.delete => cfsk_skottie_destroy | Release owned wrapper and its Skia ownership; NULL allowed */
void cfsk_skottie_destroy(cfsk_skottie* value);

/* Becomes Rust: no engine logic is ported by this surface job */
/* @cfsk CanvasKitInit => rust:cf-skia::build_link | Native Skia is linked once; Rust host initialization replaces the JS/wasm module loader */
/* @cfsk EmbindObject.delete => rust:cf-skia::Drop | Typed Drop uses matching destroy; ownership state lives in Rust, no embind liveness probe */
/* @cfsk EmbindObject.isDeleted => rust:cf-skia::Drop | Typed Drop uses matching destroy; ownership state lives in Rust, no embind liveness probe */
/* @cfsk Canvas.__inkParentClip => rust:cf-skia::test_state | Test-double metadata/prototype inspection only; no native CanvasKit operation */
/* @cfsk CanvasKit.Canvas.prototype => rust:cf-skia::test_state | Test-double metadata/prototype inspection only; no native CanvasKit operation */
/* @cfsk CanvasKit.Image.prototype => rust:cf-skia::test_state | Test-double metadata/prototype inspection only; no native CanvasKit operation */
/* @cfsk BlendModeEnumValues.[lookup] => rust:cf-skia::constants | Dynamic lookup of a pinned blend-mode enum is Rust table data, not dynamic ABI dispatch */
/* @cfsk CanvasKit.GetWebGLContext => rust:cf-gpu::DawnHost | Window/device lifecycle in host; no GL context handle crosses cfsk */
/* @cfsk CanvasKit.deleteContext => rust:cf-gpu::DawnHost | Window/device lifecycle in host; no GL context handle crosses cfsk */
/* @cfsk CanvasKit.Malloc => rust:cf-skia::buffers | Rust Vec/slices own memory; cfsk copies or borrows only for a call */
/* @cfsk CanvasKit.Free => rust:cf-skia::buffers | Rust Vec/slices own memory; cfsk copies or borrows only for a call */
/* @cfsk CanvasKit._malloc => rust:cf-skia::buffers | Rust Vec/slices own memory; cfsk copies or borrows only for a call */
/* @cfsk CanvasKit.HEAPU8 => rust:cf-skia::buffers | Rust Vec/slices own memory; cfsk copies or borrows only for a call */
/* @cfsk CompositionPlanStatics.HEAPU8 => rust:cf-skia::buffers | Rust Vec/slices own memory; cfsk copies or borrows only for a call */
/* @cfsk MallocObj.toTypedArray => rust:cf-skia::buffers | Rust Vec/slices own memory; cfsk copies or borrows only for a call */
/* @cfsk CanvasKit.Color => rust:cf-math::values | Plain C float tuples and rect/rrect structs, no FFI for pure tuple construction */
/* @cfsk CanvasKit.Color4f => rust:cf-math::values | Plain C float tuples and rect/rrect structs, no FFI for pure tuple construction */
/* @cfsk CanvasKit.LTRBRect => rust:cf-math::values | Plain C float tuples and rect/rrect structs, no FFI for pure tuple construction */
/* @cfsk CanvasKit.XYWHRect => rust:cf-math::values | Plain C float tuples and rect/rrect structs, no FFI for pure tuple construction */
/* @cfsk CanvasKit.RRectXY => rust:cf-math::values | Plain C float tuples and rect/rrect structs, no FFI for pure tuple construction */
/* @cfsk CanvasKit.Matrix => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk CanvasKit.M44 => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix3x3Helpers.invert => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix3x3Helpers.mapPoints => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix3x3Helpers.multiply => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix3x3Helpers.rotated => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix3x3Helpers.scaled => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix3x3Helpers.translated => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix4x4Helpers.invert => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk Matrix4x4Helpers.multiply => rust:cf-math::matrix | Pinned matrix maths in Rust, pass row-major floats to canvas/path calls */
/* @cfsk NamedTransferFns.ScaledHLG => rust:cf-math::color | Named transfer functions/gamuts are data and pure pinned maths; make_rgb is the C boundary */
/* @cfsk HdrColorApi.transferFn => rust:cf-math::color | Pinned named transfer/gamut data; spaces are constructed with color_space_rgb */
/* @cfsk HdrColorApi.gamut => rust:cf-math::color | Pinned named transfer/gamut data; spaces are constructed with color_space_rgb */
/* @cfsk HdrColorApi.rec2020Pq => rust:cf-math::color | Pinned named transfer/gamut data; spaces are constructed with color_space_rgb */
/* @cfsk HdrColorApi.rec2020Hlg => rust:cf-math::color | Pinned named transfer/gamut data; spaces are constructed with color_space_rgb */
/* @cfsk HdrColorApi.displayP3Linear => rust:cf-math::color | Pinned named transfer/gamut data; spaces are constructed with color_space_rgb */
/* @cfsk SkottieLogger.onError => rust:cf-skia::diagnostics | Copy error diagnostics; no retained JS/Rust logger callback crosses cfsk */
/* @cfsk SkottieLogger.onWarning => rust:cf-skia::diagnostics | Copy error diagnostics; no retained JS/Rust logger callback crosses cfsk */
/* @cfsk DiagnosticsApi.gpuStats => rust:cf-gpu::counters | Graphite recorder/submit counters; Ganesh GpuStats schema and GL translation counters retired */
/* @cfsk ForkGrDirectContext.gpuStats => rust:cf-gpu::counters | Graphite recorder/submit counters; Ganesh GpuStats schema and GL translation counters retired */
/* @cfsk ProgramCacheApi.pendingLinks => rust:cf-gpu::pipeline_store | Rust storage/keys own persistence; Graphite precompile + internal pipeline capture replace GLSL caches/links */
/* @cfsk ProgramCacheApi.translations => rust:cf-gpu::pipeline_store | Rust storage/keys own persistence; Graphite precompile + internal pipeline capture replace GLSL caches/links */
/* @cfsk ForkGrDirectContext.pendingProgramLinks => rust:cf-gpu::pipeline_store | Rust storage/keys own persistence; Graphite precompile + internal pipeline capture replace GLSL caches/links */
/* @cfsk ProgramCacheCallbacks.load => rust:cf-gpu::pipeline_store | Rust storage/keys own persistence; Graphite precompile + internal pipeline capture replace GLSL caches/links */
/* @cfsk ProgramCacheCallbacks.store => rust:cf-gpu::pipeline_store | Rust storage/keys own persistence; Graphite precompile + internal pipeline capture replace GLSL caches/links */
/* @cfsk ForkGrDirectContext.resetContext => rust:cf-gpu::texture_handoff | Dawn device/queue ordering and external texture fences replace shared GL reset */
/* @cfsk SharedGlApi.resetContext => rust:cf-gpu::texture_handoff | Dawn device/queue ordering and external texture fences replace shared GL reset */
/* @cfsk Surface._resetContext => rust:cf-gpu::texture_handoff | Dawn device/queue ordering and external texture fences replace shared GL reset */
/* @cfsk F16MsaaApi.maxSampleCount => rust:cf-gpu::aa_policy | One Context AA strategy/internal sample count; no per-surface MSAA query */
/* @cfsk ForkGrDirectContext.maxSurfaceSampleCount => rust:cf-gpu::aa_policy | One Context AA strategy/internal sample count; no per-surface MSAA query */
/* @cfsk Surface.sampleCnt => rust:cf-gpu::aa_policy | One Context AA strategy/internal sample count; no per-surface MSAA query */
/* @cfsk ForkBindingsCanvas.getMeshContext => rust:cf-gpu::recorder_owner | Rust carries owning Recorder explicitly; no GrDirectContext extraction from a canvas */
/* @cfsk ForkBindingsStatics.MakeWebGLMeshVertexBuffer => rust:cf-paint::mesh_upload | Rust CPU geometry or approved Dawn compute path; no WebGL callback; use typed mesh buffer upload */
/* @cfsk WebGLMeshVertexBuffer.write => rust:cf-paint::mesh_upload | Rust CPU geometry or approved Dawn compute path; no WebGL callback; use typed mesh buffer upload */
/* @cfsk WebGLMeshVertexBuffer.delete => rust:cf-paint::mesh_upload | Rust CPU geometry or approved Dawn compute path; no WebGL callback; use typed mesh buffer upload */
/* @cfsk CompositionPlan.apply => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.beginReplay => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.beginReplayChildren => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.bounds => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.cancelReplay => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.clearRaster => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.counters => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.counts => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.dispose => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.endReplay => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.inspect => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.liveContext => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.packetBuffer => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.raster => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.registerImage => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.registerPicture => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.releaseImage => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.releasePicture => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.replay => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.replayChildren => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.setRaster => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.step => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlan.takeClearedRasters => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlanApi.createPlan => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlanStatics.CompositionPlan => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk CompositionPlanStatics.compositionPlanApi => rust:cf-skia::build_capabilities | Linked fork build identity replaces composition-plan JS capability markers */
/* @cfsk CompositionPlanStatics.compositionPlanSpatialApi => rust:cf-skia::build_capabilities | Linked fork build identity replaces composition-plan JS capability markers */
/* @cfsk RawCompositionPlan._apply => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._beginReplay => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._beginReplayChildren => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._beginReplayContent => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._bounds => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._counters => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._counts => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._endReplay => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._inspect => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._liveContexts => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._openTokens => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._packetBuffer => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._raster => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._setRaster => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan._step => rust:cf-paint::replay | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan.clearRaster => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan.delete => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan.registerImage => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan.registerPicture => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan.releaseImage => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan.releasePicture => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk RawCompositionPlan.takeClearedRasters => rust:cf-retain::plan | Plan packets/atomic apply, generations, registrations, bounds, sealed rasters and replay tokens move to Rust */
/* @cfsk interface:BindingsApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:BindingsStatics => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:BindingsSurface => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:CanvasKitBuildIdentity => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:ColorSpaceLayer => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:CompositingApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:CompositingBindings => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:CompositionPlan => rust:cf-retain::plan_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:CompositionPlanApi => rust:cf-retain::plan_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:CustomBuildSources => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:DiagnosticsApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:F16MsaaApi => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:FlushFinished => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:FlushOptions => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:GpuCacheReading => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:GpuContextOptions => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:GpuOptionsApi => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:GpuSurfaceOptions => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:HdrColorApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:HdrColorStatics => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:LivePaintContext => rust:cf-retain::plan_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:MinBlendApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:MinBlendStatics => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:MotionBlurApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:MotionBlurStatics => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:MsaaRequest => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:MsaaTarget => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:NamedGamuts => rust:cf-math::color | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:NamedTransferFns => rust:cf-math::color | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:NativeEffectsApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:NativeEffectsStatics => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:OffsetPath => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:PageCanvasKit => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:PlanApplyResult => rust:cf-retain::plan_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:PlanPacketHeader => rust:cf-retain::plan_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:ProgramCacheApi => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:ProgramCacheCallbacks => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:ReplayTarget => rust:cf-retain::plan_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:ServedMsaa => rust:cf-gpu::policy_types | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:SharedGlApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:SkottieApi => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk interface:SkottieLogger => rust:cf-skia::facades | Rust facade/data contract; callable members have separate checked ABI or becomes-Rust rows */
/* @cfsk NamedGamuts.AdobeRGB => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedGamuts.DisplayP3 => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedGamuts.Rec2020 => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedGamuts.SRGB => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedGamuts.XYZ => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedTransferFns.HLG => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedTransferFns.Linear => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedTransferFns.PQ => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedTransferFns.Rec2020 => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedTransferFns.SRGB => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk NamedTransferFns.TwoDotTwo => rust:cf-math::color | Pinned named color transfer/gamut data; no FFI call for a constant tuple */
/* @cfsk CanvasKit.AlphaType => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.BLACK => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.BLUE => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.BlendMode => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.BlurStyle => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.Canvas => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ClipOp => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ColorChannel => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ColorFilter => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ColorSpace => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ColorType => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.FillType => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.FilterMode => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.FontCollection => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.FontEdging => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.FontHinting => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.Image => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ImageFilter => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ImageFormat => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.MaskFilter => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.MipmapMode => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.PaintStyle => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ParagraphBuilder => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.ParagraphStyle => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.Path1DEffect => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.PathEffect => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.PathOp => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.RED => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.RuntimeEffect => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.SaveLayerF16ColorType => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.SaveLayerInitWithPrevious => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.Shader => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.StrokeCap => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.StrokeJoin => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.TRANSPARENT => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.TextAlign => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.TextBlob => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.TextDirection => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.TileMode => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.Typeface => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.TypefaceFontProvider => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.VertexMode => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.WHITE => rust:cf-skia::constants | Pinned enums/flags/colors or factory namespace; concrete callable rows are enumerated separately */
/* @cfsk CanvasKit.bindingsApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.compositingApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.compositionPlanApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.compositionPlanSpatialApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.diagnosticsApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.f16MsaaApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.gpuOptionsApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.hdrColorApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.minBlendApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.motionBlurApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.nativeEffectsApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.programCacheApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.programShareApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.sharedGlApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.skottieApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk CanvasKit.wideVertexColorApi => rust:cf-skia::build_capabilities | Linked fork ABI/build identity replaces optional JS capability markers */
/* @cfsk ForkBindingsStatics.meshBufferInteropApi => rust:cf-skia::build_capabilities | Linked build identity and caller-owned Rust slices replace embind capability/heap reads */
/* @cfsk ForkBindingsStatics.meshChildrenApi => rust:cf-skia::build_capabilities | Linked build identity and caller-owned Rust slices replace embind capability/heap reads */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* CFSK_H */
