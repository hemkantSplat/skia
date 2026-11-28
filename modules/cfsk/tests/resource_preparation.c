#include "cfsk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OK(call) do { \
    cfsk_status status = (call); \
    if (status != CFSK_OK) { \
        fprintf(stderr, "%s: %d\n", #call, status); \
        exit(2); \
    } \
} while (0)

static void record_text(cfsk_canvas* canvas, const char* font_path,
                        cfsk_recorder* prepare, const cfsk_surface* target) {
    FILE* input = fopen(font_path, "rb");
    if (!input || fseek(input, 0, SEEK_END)) exit(2);
    long length = ftell(input);
    if (length <= 0 || fseek(input, 0, SEEK_SET)) exit(2);
    unsigned char* bytes = malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, input) != (size_t)length) exit(2);
    fclose(input);
    cfsk_typeface* face = NULL;
    OK(cfsk_typeface_from_data((cfsk_bytes){bytes, (size_t)length}, 0, &face));
    free(bytes);
    cfsk_font* font = NULL;
    OK(cfsk_font_make(face, 23.5f, 1, 0, &font));
    static const char text[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    uint16_t glyphs[sizeof(text) - 1];
    cfsk_point positions[sizeof(text) - 1];
    size_t glyph_count = 0;
    OK(cfsk_font_glyph_ids(font, (cfsk_string){text, sizeof(text) - 1},
                           glyphs, sizeof(text) - 1, &glyph_count));
    if (glyph_count != sizeof(text) - 1) exit(2);
    for (size_t i = 0; i < glyph_count; ++i) {
        positions[i] = (cfsk_point){(float)(i % 25) * 19, (float)(i / 25) * 32};
    }
    cfsk_paint* paint = NULL;
    OK(cfsk_paint_make(&paint));
    if (prepare) {
        const float identity[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const cfsk_point origin = {8.25f, 34.5f};
        cfsk_glyph_preparation_work work = {0};
        OK(cfsk_recorder_prepare_glyphs(prepare, target, font, glyphs, positions, 1, origin,
                                        identity, paint, &work));
        if (work.inserted != 1 || work.pages != 1) exit(1);
        OK(cfsk_recorder_prepare_glyphs(prepare, target, font, glyphs, positions, 1, origin,
                                        identity, paint, &work));
        if (work.inserted || work.masks || work.pages) exit(1);
    }
    OK(cfsk_canvas_draw_glyphs(canvas, glyphs, positions, glyph_count,
                               (cfsk_point){8.25f, 34.5f}, font, paint));
    cfsk_paint_destroy(paint);
    cfsk_font_destroy(font);
    cfsk_typeface_destroy(face);
}

static void record_effect(cfsk_canvas* canvas) {
    const char sksl[] = "half4 main(float2 p) { return half4(fract(p.x * 0.013), 0.35, 0.7, 1); }";
    cfsk_runtime_effect* effect = NULL;
    cfsk_shader* shader = NULL;
    cfsk_paint* paint = NULL;
    cfsk_image_filter* filter = NULL;
    /* The valid shader exceeds the pipeline archive's source-size limit. */
    const size_t shader_size = (1u << 20) + 1;
    char* shader_source = malloc(shader_size);
    if (!shader_source) exit(2);
    memcpy(shader_source, sksl, sizeof(sksl) - 1);
    memset(shader_source + sizeof(sksl) - 1, ' ', shader_size - (sizeof(sksl) - 1));
    OK(cfsk_runtime_effect_make((cfsk_string){shader_source, shader_size}, 0, NULL, NULL, &effect));
    free(shader_source);
    OK(cfsk_runtime_effect_shader(effect, (cfsk_bytes){0}, NULL, 0, NULL, &shader));
    OK(cfsk_image_filter_blur(4, 3, 0, NULL, NULL, &filter));
    OK(cfsk_paint_make(&paint));
    OK(cfsk_paint_set_shader(paint, shader));
    OK(cfsk_paint_set_image_filter(paint, filter));
    OK(cfsk_canvas_draw_rect(canvas, (cfsk_rect){62, 90, 280, 175}, paint));
    cfsk_paint_destroy(paint);
    cfsk_image_filter_destroy(filter);
    cfsk_shader_destroy(shader);
    cfsk_runtime_effect_destroy(effect);
}


static void record_bitmap(cfsk_canvas* canvas) {
    const cfsk_image_info bitmap_info = {16, 8, CFSK_COLOR_RGBA_8888, CFSK_ALPHA_PREMUL, NULL};
    uint8_t pixels[16 * 8 * 4];
    for (size_t pixel = 0; pixel < sizeof pixels / 4; ++pixel) {
        pixels[pixel * 4] = (uint8_t)pixel;
        pixels[pixel * 4 + 1] = 163;
        pixels[pixel * 4 + 2] = 93;
        pixels[pixel * 4 + 3] = 255;
    }
    cfsk_image* bitmap = NULL;
    OK(cfsk_image_from_pixels(&bitmap_info, (cfsk_bytes){pixels, sizeof pixels}, 16 * 4, &bitmap));
    OK(cfsk_canvas_draw_image(canvas, bitmap, 400, 125, (cfsk_sampling){0}, NULL));
    cfsk_image_destroy(bitmap);
}

static const cfsk_image_info info = {512, 192, CFSK_COLOR_RGBA_8888, CFSK_ALPHA_PREMUL, NULL};

static void issue(cfsk_context* context, cfsk_recorder* recorder) {
    cfsk_recording* recording = NULL;
    cfsk_submission* submission = NULL;
    OK(cfsk_recorder_snap(recorder, &recording));
    OK(cfsk_context_insert(context, &recording, 0, &submission));
    OK(cfsk_context_submit(context, 1));
    cfsk_submission_destroy(submission);
}

static void read_pixels(cfsk_context* context, cfsk_surface* target, uint8_t* pixels) {
    cfsk_readback* readback = NULL;
    OK(cfsk_surface_readback(context, target, &info, (cfsk_irect){0, 0, 512, 192}, &readback));
    OK(cfsk_context_submit(context, 1));
    size_t required = 0;
    OK(cfsk_readback_copy(readback, pixels, 512 * 192 * 4, 512 * 4, &required));
    cfsk_readback_destroy(readback);
}

static void render(const char* font_path, int prepare, uint8_t* pixels,
                   uint8_t* playing_pixels, cfsk_native_costs* costs) {
    cfsk_dawn_host* host = NULL;
    cfsk_context* context = NULL;
    cfsk_recorder *recorder = NULL, *playing = NULL;
    cfsk_surface *target = NULL, *live_target = NULL;
    cfsk_canvas *canvas = NULL, *live_canvas = NULL, *source = NULL;
    cfsk_picture_recorder* picture_recorder = NULL;
    cfsk_picture* picture = NULL;

    OK(cfsk_dawn_host_make(&host));
    OK(cfsk_context_make_dawn_persistent(host, NULL, (cfsk_bytes){0}, &context));
    OK(cfsk_context_make_recorder(context, &recorder));
    OK(cfsk_context_make_recorder(context, &playing));
    OK(cfsk_surface_make_graphite(recorder, &info, 0, &target));
    OK(cfsk_surface_make_graphite(playing, &info, 0, &live_target));
    OK(cfsk_surface_get_canvas(target, &canvas));
    OK(cfsk_surface_get_canvas(live_target, &live_canvas));
    OK(cfsk_canvas_clear(live_canvas, (cfsk_color){0.17f, 0.29f, 0.55f, 1}, NULL));

    OK(cfsk_picture_recorder_make(&picture_recorder));
    OK(cfsk_picture_recorder_begin(picture_recorder, (cfsk_rect){0, 0, 512, 192}, &source));
    OK(cfsk_canvas_clear(source, (cfsk_color){1, 1, 1, 1}, NULL));
    record_text(source, font_path, prepare ? recorder : NULL, target);
    record_effect(source);
    record_bitmap(source);
    OK(cfsk_picture_recorder_finish_verbatim(picture_recorder, &picture));

    if (prepare) {
        cfsk_pipeline_preparation* plan = NULL;
        cfsk_pipeline_preparation_work work = {0};
        uint32_t compiles = 0, glyphs = 0, pages = 0, slices = 0, bitmaps = 0, targets = 0;
        OK(cfsk_pipeline_preparation_make(recorder, target, picture, 1, &work, &plan));
        uint64_t elapsed = work.nanos, worst = work.nanos;
        while (work.more) {
            if (++slices > 1000) exit(2);
            OK(cfsk_pipeline_preparation_step(plan, &work));
            elapsed += work.nanos;
            if (work.nanos > worst) worst = work.nanos;
            compiles += work.compiled;
            glyphs += work.atlas_entries;
            pages += work.atlas_pages;
            bitmaps += work.bitmap_uploads;
            targets += work.targets;
        }
        printf("prep slices=%u glyphs=%u pages=%u pipelines=%u bitmaps=%u targets=%u\n",
               slices, glyphs, pages, compiles, bitmaps, targets);
        if (!compiles || !glyphs || !pages || !bitmaps || !targets || !elapsed) exit(1);
        OK(cfsk_pipeline_preparation_step(plan, &work));
        if (work.more || work.compiled || work.atlas_entries || work.atlas_pages) exit(1);
        cfsk_pipeline_preparation_destroy(plan);
    }

    cfsk_native_costs_enable(1);
    OK(cfsk_canvas_draw_picture(canvas, picture, NULL, NULL));
    issue(context, recorder);
    cfsk_native_costs_take(costs);
    uint32_t inline_count = 0, precompiled = 0, unkeyable = 0;
    OK(cfsk_context_pipeline_stats(context, &inline_count, &precompiled, &unkeyable));
    printf("inline=%u precompiled=%u unkeyable=%u\n", inline_count, precompiled, unkeyable);
    if (!prepare && !unkeyable) exit(2);
    cfsk_native_costs_enable(0);
    issue(context, playing);
    read_pixels(context, target, pixels);
    read_pixels(context, live_target, playing_pixels);
    cfsk_picture_unref(picture);
    cfsk_picture_recorder_destroy(picture_recorder);
    cfsk_surface_destroy(live_target);
    cfsk_surface_destroy(target);
    cfsk_recorder_destroy(playing);
    cfsk_recorder_destroy(recorder);
    cfsk_context_destroy(context);
    OK(cfsk_dawn_host_release(&host));
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    const int omit = argc == 3 && !strcmp(argv[2], "--omit-preparation");
    static uint8_t reference[512 * 192 * 4], candidate[sizeof reference];
    static uint8_t playing_reference[sizeof reference], playing_candidate[sizeof reference];
    cfsk_native_costs baseline = {0}, prepared = {0};
    render(argv[1], 0, reference, playing_reference, &baseline);
    render(argv[1], !omit, candidate, playing_candidate, &prepared);

    for (size_t op = 0; op < 7; ++op) {
        if (op != 0 && op != 2 && op != 5) continue;
        uint64_t before = 0, after = 0;
        for (size_t phase = 0; phase < 3; ++phase) {
            before += baseline.counts[phase * 7 + op];
            after += prepared.counts[phase * 7 + op];
        }
        printf("op=%zu before=%llu after=%llu\n", op,
               (unsigned long long)before, (unsigned long long)after);
        if (!before || after) return 1;
    }
    if (memcmp(reference, candidate, sizeof reference) ||
        memcmp(playing_reference, playing_candidate, sizeof playing_reference)) return 1;
    puts("PASS prepared and concurrently recorded frames match the unprepared controls");
    return 0;
}
