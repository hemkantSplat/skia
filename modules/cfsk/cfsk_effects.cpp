/* C ABI validation and ownership stay at the boundary; Skia owns rendering. */
#include "cfsk_internal.h"
#include "include/core/SkString.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkPerlinNoiseShader.h"
#include "include/effects/SkLumaColorFilter.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkCornerPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/Sk1DPathEffect.h"
#include "include/effects/SkTrimPathEffect.h"
#include "include/effects/SkBlenders.h"
#include "include/effects/SkGlowShader.h"
#include "include/effects/SkOutputTransform.h"
#include "include/effects/SkBlurMaskFilter.h"
#include "src/core/SkColorFilterPriv.h"  // nogncheck: pinned factory has no public counterpart.
namespace {
bool valid_gradient(const cfsk_gradient* g) {
    if (!g || g->count < 2 || g->count > INT_MAX || !g->colors || g->tile_mode > 3 ||
        g->interpolation.in_premul > 1 || g->interpolation.color_space > 14 || g->interpolation.hue_method > 3 ||
        (g->local_matrix && !cfsk::finite(g->local_matrix, 9))) return false;
    for (size_t i = 0; i < g->count; ++i) {
        if (!cfsk::finite(g->colors[i])) return false;
        if (g->positions && (!cfsk::finite(g->positions[i]) || g->positions[i] < 0 || g->positions[i] > 1 ||
            (i && g->positions[i] < g->positions[i-1]))) return false;
    }
    return true;
}
struct Gradient {
    std::vector<SkColor4f> colors;
    const cfsk_gradient& source;
    SkMatrix matrix;
    explicit Gradient(const cfsk_gradient& g) : source(g), matrix(g.local_matrix ? cfsk::matrix(g.local_matrix) : SkMatrix::I()) {
        for (size_t i = 0; i < g.count; ++i) colors.push_back(cfsk::color(g.colors[i]));
    }
    SkGradient value() const {
        return {SkGradient::Colors(SkSpan<const SkColor4f>(colors), source.positions ? SkSpan<const float>(source.positions, source.count) : SkSpan<const float>(), static_cast<SkTileMode>(source.tile_mode), cfsk::ref(source.color_space)),
            {static_cast<SkGradient::Interpolation::InPremul>(source.interpolation.in_premul),
             static_cast<SkGradient::Interpolation::ColorSpace>(source.interpolation.color_space),
             static_cast<SkGradient::Interpolation::HueMethod>(source.interpolation.hue_method)}};
    }
};
bool valid_core(const float* core, size_t count) { return !count || ((count == 13 || count == 15) && cfsk::finite(core, count)); }
SkGlowShader::Core glow_core(const float* p, size_t count) {
    if (!p) return {};
    return {{p[0],p[1],p[2],p[3]}, SkRect::MakeLTRB(p[4],p[5],p[6],p[7]), {p[8],p[9],p[10],p[11]},p[12],count > 13 ? p[13] : 0.f,count > 14 ? p[14] : 0.f};
}
}

cfsk_status cfsk_runtime_effect_make(cfsk_string sksl, uint32_t kind, cfsk_shader_error_handler on_error, void* user, cfsk_runtime_effect** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(sksl));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(kind <= 2);
    SkString source(sksl.data ? sksl.data : "", sksl.length);
    SkRuntimeEffect::Result result;
    if (kind == 0) result = SkRuntimeEffect::MakeForShader(source);
    else if (kind == 1) result = SkRuntimeEffect::MakeForColorFilter(source);
    else result = SkRuntimeEffect::MakeForBlender(source);
    if (!result.effect) {
        cfsk::error.assign(result.errorText.c_str(), result.errorText.size());
        if (on_error) on_error(user, {cfsk::error.data(), cfsk::error.size()});
        return CFSK_FAILED;
    }
    *output = new (std::nothrow) cfsk_runtime_effect{std::move(result.effect), kind};
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "effect allocation failed");
}

cfsk_status cfsk_runtime_effect_uniform_count(const cfsk_runtime_effect* effect, size_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(effect && output);
    *output = effect->value->uniforms().size();
    return CFSK_OK;
}

cfsk_status cfsk_runtime_effect_uniform_float_count(const cfsk_runtime_effect* effect, size_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(effect && output);
    *output = effect->value->uniformSize() / sizeof(float);
    return CFSK_OK;
}

cfsk_status cfsk_runtime_effect_uniform(const cfsk_runtime_effect* effect, size_t index, cfsk_uniform* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(effect && output && index < effect->value->uniforms().size());
    const auto& u = effect->value->uniforms()[index];
    uint32_t type = static_cast<uint32_t>(u.type);
    static constexpr uint32_t columns[] = {1,2,3,4,2,3,4,1,2,3,4};
    static constexpr uint32_t rows[] = {1,1,1,1,2,3,4,1,1,1,1};
    *output = {{u.name.data(), u.name.size()}, static_cast<uint32_t>(u.offset), static_cast<uint32_t>(u.count), columns[type], rows[type], static_cast<uint8_t>(type >= 7)};
    return CFSK_OK;
}

cfsk_status cfsk_runtime_effect_shader(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, const cfsk_shader* const* children, size_t child_count, const float* local_matrix, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(uniforms));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(effect && effect->kind == 0 && uniforms.length == effect->value->uniformSize());
    auto data = SkData::MakeWithCopy(uniforms.data, uniforms.length);
    CFSK_REQUIRE(child_count == effect->value->children().size() && (!child_count || children));
    std::vector<SkRuntimeEffect::ChildPtr> child_values;
    for (size_t i = 0; i < child_count; ++i) {
        CFSK_REQUIRE(effect->value->children()[i].type == SkRuntimeEffect::ChildType::kShader);
        child_values.emplace_back(cfsk::ref(children[i]));
    }
    CFSK_REQUIRE(!local_matrix || cfsk::finite(local_matrix, 9));
    SkMatrix m = local_matrix ? cfsk::matrix(local_matrix) : SkMatrix::I();
    return cfsk::resource(output, effect->value->makeShader(data, SkSpan<const SkRuntimeEffect::ChildPtr>(child_values), &m));
}

cfsk_status cfsk_runtime_effect_color_filter(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, const cfsk_shader* const* children, size_t child_count, cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(uniforms));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(effect && effect->kind == 1 && uniforms.length == effect->value->uniformSize());
    auto data = SkData::MakeWithCopy(uniforms.data, uniforms.length);
    CFSK_REQUIRE(child_count == effect->value->children().size() && (!child_count || children));
    std::vector<SkRuntimeEffect::ChildPtr> child_values;
    for (size_t i = 0; i < child_count; ++i) {
        CFSK_REQUIRE(effect->value->children()[i].type == SkRuntimeEffect::ChildType::kShader);
        child_values.emplace_back(cfsk::ref(children[i]));
    }
    return cfsk::resource(output, effect->value->makeColorFilter(data, SkSpan<const SkRuntimeEffect::ChildPtr>(child_values)));
}

cfsk_status cfsk_runtime_effect_blender(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, cfsk_blender** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(uniforms));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(effect && effect->kind == 2 && uniforms.length == effect->value->uniformSize());
    auto data = SkData::MakeWithCopy(uniforms.data, uniforms.length);
    CFSK_REQUIRE(effect->value->children().empty());
    return cfsk::resource(output, effect->value->makeBlender(data));
}

cfsk_status cfsk_runtime_effect_builder_make(const cfsk_runtime_effect* effect, cfsk_bytes uniforms, cfsk_runtime_effect_builder** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(uniforms));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(effect && effect->kind == 0 && uniforms.length == effect->value->uniformSize());
    *output = new (std::nothrow) cfsk_runtime_effect_builder{SkRuntimeShaderBuilder(effect->value, SkData::MakeWithCopy(uniforms.data, uniforms.length))};
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "builder allocation failed");
}

cfsk_status cfsk_runtime_effect_builder_uniforms(cfsk_runtime_effect_builder* builder, cfsk_bytes uniforms) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(uniforms));
    CFSK_REQUIRE(builder && uniforms.length == builder->value.effect()->uniformSize());
    for (const auto& u : builder->value.effect()->uniforms()) {
        builder->value.uniform(u.name).set(uniforms.data + u.offset, static_cast<int>(u.sizeInBytes()));
    }
    return CFSK_OK;
}

cfsk_status cfsk_runtime_effect_builder_child(cfsk_runtime_effect_builder* builder, cfsk_string name, const cfsk_shader* shader) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(name));
    CFSK_REQUIRE(builder);
    std::string_view key(name.data ? name.data : "", name.length);
    auto child = builder->value.effect()->findChild(key);
    CFSK_REQUIRE(child && child->type == SkRuntimeEffect::ChildType::kShader);
    builder->value.child(key) = cfsk::ref(shader);
    return CFSK_OK;
}

cfsk_status cfsk_shader_color(cfsk_color color, const cfsk_color_space* space, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(color));
    CFSK_OUTPUT(output);
    return cfsk::resource(output, SkShaders::Color(cfsk::color(color), cfsk::ref(space)));
}

cfsk_status cfsk_shader_blend(uint32_t mode, const cfsk_shader* destination, const cfsk_shader* source, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(destination && source && mode <= 28);
    return cfsk::resource(output, SkShaders::Blend(static_cast<SkBlendMode>(mode), destination->value, source->value));
}

cfsk_status cfsk_shader_gradient_linear(cfsk_point start, cfsk_point end, const cfsk_gradient* gradient, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(start));
    CFSK_REQUIRE(cfsk::finite(end));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(valid_gradient(gradient) && (true));
    Gradient grad(*gradient);
    SkPoint points[2] = {cfsk::point(start), cfsk::point(end)};
    return cfsk::resource(output, SkShaders::LinearGradient(points, grad.value(), &grad.matrix));
}

cfsk_status cfsk_shader_gradient_radial(cfsk_point center, float radius, const cfsk_gradient* gradient, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(center));
    CFSK_REQUIRE(cfsk::finite(radius));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(valid_gradient(gradient) && (radius >= 0));
    Gradient grad(*gradient);
    return cfsk::resource(output, SkShaders::RadialGradient(cfsk::point(center), radius, grad.value(), &grad.matrix));
}

cfsk_status cfsk_shader_gradient_conical(cfsk_point start, float start_radius, cfsk_point end, float end_radius, const cfsk_gradient* gradient, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(start));
    CFSK_REQUIRE(cfsk::finite(start_radius));
    CFSK_REQUIRE(cfsk::finite(end));
    CFSK_REQUIRE(cfsk::finite(end_radius));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(valid_gradient(gradient) && (start_radius >= 0 && end_radius >= 0));
    Gradient grad(*gradient);
    return cfsk::resource(output, SkShaders::TwoPointConicalGradient(cfsk::point(start), start_radius, cfsk::point(end), end_radius, grad.value(), &grad.matrix));
}

cfsk_status cfsk_shader_gradient_sweep(cfsk_point center, float start_degrees, float end_degrees, const cfsk_gradient* gradient, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(center));
    CFSK_REQUIRE(cfsk::finite(start_degrees));
    CFSK_REQUIRE(cfsk::finite(end_degrees));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(valid_gradient(gradient) && (start_degrees < end_degrees));
    Gradient grad(*gradient);
    return cfsk::resource(output, SkShaders::SweepGradient(cfsk::point(center), start_degrees, end_degrees, grad.value(), &grad.matrix));
}

cfsk_status cfsk_shader_noise(uint32_t kind, float frequency_x, float frequency_y, int32_t octaves, float seed, int32_t tile_width, int32_t tile_height, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(frequency_x));
    CFSK_REQUIRE(cfsk::finite(frequency_y));
    CFSK_REQUIRE(cfsk::finite(seed));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(kind <= 1 && frequency_x >= 0 && frequency_y >= 0 && octaves >= 0 && octaves <= 255 && tile_width >= 0 && tile_height >= 0);
    SkISize tile = SkISize::Make(tile_width, tile_height);
    return cfsk::resource(output, kind == 0 ? SkShaders::MakeFractalNoise(frequency_x, frequency_y, octaves, seed, &tile) : SkShaders::MakeTurbulence(frequency_x, frequency_y, octaves, seed, &tile));
}

cfsk_status cfsk_color_filter_compose(const cfsk_color_filter* outer, const cfsk_color_filter* inner, cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(outer || inner);
    return cfsk::resource(output, SkColorFilters::Compose(cfsk::ref(outer), cfsk::ref(inner)));
}

cfsk_status cfsk_color_filter_blend(cfsk_color color, const cfsk_color_space* space, uint32_t mode, cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(color));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(mode <= 28);
    return cfsk::resource(output, SkColorFilters::Blend(cfsk::color(color), cfsk::ref(space), static_cast<SkBlendMode>(mode)));
}

cfsk_status cfsk_color_filter_matrix(const float matrix[20], cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::finite(matrix, 20));
    return cfsk::resource(output, SkColorFilters::Matrix(matrix));
}

cfsk_status cfsk_color_filter_luma(cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, SkLumaColorFilter::Make());
}

cfsk_status cfsk_color_filter_srgb_to_linear(cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, SkColorFilters::SRGBToLinearGamma());
}

cfsk_status cfsk_color_filter_linear_to_srgb(cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, SkColorFilters::LinearToSRGBGamma());
}

cfsk_status cfsk_color_space_srgb(uint8_t linear, cfsk_color_space** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(linear <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, linear ? SkColorSpace::MakeSRGBLinear() : SkColorSpace::MakeSRGB());
}

cfsk_status cfsk_color_filter_color_space_xform(const cfsk_color_space* source, const cfsk_color_space* destination, cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(source && destination);
    return cfsk::resource(output, SkColorFilterPriv::MakeColorSpaceXform(source->value, destination->value));
}

cfsk_status cfsk_color_filter_output_decode(cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, SkOutputTransform::Decode());
}

cfsk_status cfsk_color_filter_output_encode(cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, SkOutputTransform::Encode());
}

cfsk_status cfsk_color_filter_tone_map(uint32_t curve, float exposure, cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(exposure));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(curve <= 5);
    return cfsk::resource(output, SkOutputTransform::ToneMap(static_cast<SkOutputTransform::ToneCurve>(curve), exposure));
}

cfsk_status cfsk_color_filter_grade(const float uniforms[31], const cfsk_image* curves, cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::finite(uniforms, 31) && curves);
    return cfsk::resource(output, SkOutputTransform::Grade(SkSpan<const float>(uniforms, 31), curves->value));
}

cfsk_status cfsk_color_filter_lut_strip(const cfsk_image* strip, int32_t size, float intensity, const float domain_min[3], const float domain_max[3], cfsk_color_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(intensity));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(strip && size >= 2 && intensity >= 0 && intensity <= 1 && cfsk::finite(domain_min, 3) && cfsk::finite(domain_max, 3));
    return cfsk::resource(output, SkOutputTransform::LutStrip(strip->value, size, intensity, {domain_min[0], domain_min[1], domain_min[2]}, {domain_max[0], domain_max[1], domain_max[2]}));
}

cfsk_status cfsk_mask_filter_blur(uint32_t style, float sigma, uint8_t respect_ctm, cfsk_mask_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(sigma));
    CFSK_REQUIRE(respect_ctm <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(style <= 3 && sigma > 0);
    return cfsk::resource(output, SkMaskFilter::MakeBlur(static_cast<SkBlurStyle>(style), sigma, respect_ctm));
}

cfsk_status cfsk_path_effect_corner(float radius, cfsk_path_effect** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(radius));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(radius >= 0);
    return cfsk::resource(output, SkCornerPathEffect::Make(radius));
}

cfsk_status cfsk_path_effect_dash(const float* intervals, size_t count, float phase, cfsk_path_effect** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(phase));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(count <= INT_MAX && count > 0 && !(count % 2) && cfsk::finite(intervals, count));
    double total = 0;
    for (size_t i = 0; i < count; ++i) { CFSK_REQUIRE(intervals[i] >= 0); total += intervals[i]; }
    CFSK_REQUIRE(total > 0);
    return cfsk::resource(output, SkDashPathEffect::Make(SkSpan<const float>(intervals, count), phase));
}

cfsk_status cfsk_path_effect_path1d(const cfsk_path* path, float advance, float phase, uint32_t style, cfsk_path_effect** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(advance));
    CFSK_REQUIRE(cfsk::finite(phase));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(path && advance > 0 && style <= 2);
    return cfsk::resource(output, SkPath1DPathEffect::Make(path->value, advance, phase, static_cast<SkPath1DPathEffect::Style>(style)));
}

cfsk_status cfsk_path_effect_trim(float start, float end, uint32_t mode, cfsk_path_effect** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(start));
    CFSK_REQUIRE(cfsk::finite(end));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(mode <= 1 && start >= 0 && start <= 1 && end >= 0 && end <= 1);
    return cfsk::resource(output, SkTrimPathEffect::Make(start, end, static_cast<SkTrimPathEffect::Mode>(mode)));
}

cfsk_status cfsk_blender_add(cfsk_blender** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, SkBlenders::Add());
}

cfsk_status cfsk_blender_min(cfsk_blender** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(true);
    return cfsk::resource(output, SkBlenders::Min());
}

cfsk_status cfsk_blender_light_resolve(uint32_t kind, uint32_t into_space, uint32_t light_space, uint8_t emissive, cfsk_blender** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(emissive <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(kind <= 1 && into_space <= 1 && light_space <= 1);
    return cfsk::resource(output, kind == 0 ? SkBlenders::LightResolve(static_cast<SkBlenders::LightSpace>(into_space), static_cast<SkBlenders::LightSpace>(light_space), emissive) : SkBlenders::LayerResolve(static_cast<SkBlenders::LightSpace>(into_space)));
}

cfsk_status cfsk_color_space_rgb(const float transfer_fn[7], const float gamut[9], cfsk_color_space** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::finite(transfer_fn, 7) && cfsk::finite(gamut, 9));
    skcms_TransferFunction transfer; skcms_Matrix3x3 matrix;
    std::memcpy(&transfer, transfer_fn, sizeof(transfer)); std::memcpy(&matrix, gamut, sizeof(matrix));
    return cfsk::resource(output, SkColorSpace::MakeRGB(transfer, matrix));
}

cfsk_status cfsk_color_space_equals(const cfsk_color_space* first, const cfsk_color_space* second, uint8_t* output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(output);
    *output = SkColorSpace::Equals(first ? first->value.get() : nullptr, second ? second->value.get() : nullptr);
    return CFSK_OK;
}

cfsk_status cfsk_image_filter_blur(float sigma_x, float sigma_y, uint32_t tile_mode, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(sigma_x));
    CFSK_REQUIRE(cfsk::finite(sigma_y));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((sigma_x >= 0 && sigma_y >= 0 && tile_mode <= 3) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::Blur(sigma_x, sigma_y, static_cast<SkTileMode>(tile_mode), cfsk::ref(input), clip));
}

cfsk_status cfsk_image_filter_blend(uint32_t mode, const cfsk_image_filter* background, const cfsk_image_filter* foreground, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((mode <= 28) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::Blend(static_cast<SkBlendMode>(mode), cfsk::ref(background), cfsk::ref(foreground), clip));
}

cfsk_status cfsk_image_filter_color_filter(const cfsk_color_filter* filter, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((filter) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::ColorFilter(filter->value, cfsk::ref(input), clip));
}

cfsk_status cfsk_image_filter_dilate(float radius_x, float radius_y, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(radius_x));
    CFSK_REQUIRE(cfsk::finite(radius_y));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((radius_x >= 0 && radius_y >= 0) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::Dilate(radius_x, radius_y, cfsk::ref(input), clip));
}

cfsk_status cfsk_image_filter_erode(float radius_x, float radius_y, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(radius_x));
    CFSK_REQUIRE(cfsk::finite(radius_y));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((radius_x >= 0 && radius_y >= 0) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::Erode(radius_x, radius_y, cfsk::ref(input), clip));
}

cfsk_status cfsk_image_filter_displacement(uint32_t channel_x, uint32_t channel_y, float scale, const cfsk_image_filter* displacement, const cfsk_image_filter* color, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(scale));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((channel_x <= 3 && channel_y <= 3) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::DisplacementMap(static_cast<SkColorChannel>(channel_x), static_cast<SkColorChannel>(channel_y), scale, cfsk::ref(displacement), cfsk::ref(color), clip));
}

cfsk_status cfsk_image_filter_shadow(float dx, float dy, float sigma_x, float sigma_y, cfsk_color color, const cfsk_color_space* space, uint8_t shadow_only, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(dx));
    CFSK_REQUIRE(cfsk::finite(dy));
    CFSK_REQUIRE(cfsk::finite(sigma_x));
    CFSK_REQUIRE(cfsk::finite(sigma_y));
    CFSK_REQUIRE(cfsk::finite(color));
    CFSK_REQUIRE(shadow_only <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((sigma_x >= 0 && sigma_y >= 0) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, shadow_only ? SkImageFilters::DropShadowOnly(dx, dy, sigma_x, sigma_y, cfsk::color(color), cfsk::ref(space), cfsk::ref(input), clip) : SkImageFilters::DropShadow(dx, dy, sigma_x, sigma_y, cfsk::color(color), cfsk::ref(space), cfsk::ref(input), clip));
}

cfsk_status cfsk_image_filter_offset(float dx, float dy, const cfsk_image_filter* input, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(dx));
    CFSK_REQUIRE(cfsk::finite(dy));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((true) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::Offset(dx, dy, cfsk::ref(input), clip));
}

cfsk_status cfsk_image_filter_shader(const cfsk_shader* shader, uint8_t dither, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(dither <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((shader) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::Shader(shader->value, static_cast<SkImageFilters::Dither>(dither), clip));
}

cfsk_status cfsk_image_filter_arithmetic(float k1, float k2, float k3, float k4, uint8_t enforce_premul, const cfsk_image_filter* background, const cfsk_image_filter* foreground, const cfsk_rect* crop, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(k1));
    CFSK_REQUIRE(cfsk::finite(k2));
    CFSK_REQUIRE(cfsk::finite(k3));
    CFSK_REQUIRE(cfsk::finite(k4));
    CFSK_REQUIRE(enforce_premul <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE((true) && (!crop || cfsk::finite(*crop)));
    SkImageFilters::CropRect clip = crop ? SkImageFilters::CropRect(cfsk::rect(*crop)) : SkImageFilters::CropRect();
    return cfsk::resource(output, SkImageFilters::Arithmetic(k1, k2, k3, k4, enforce_premul, cfsk::ref(background), cfsk::ref(foreground), clip));
}

cfsk_status cfsk_image_filter_compose(const cfsk_image_filter* outer, const cfsk_image_filter* inner, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(outer && inner);
    return cfsk::resource(output, SkImageFilters::Compose(outer->value, inner->value));
}

cfsk_status cfsk_image_filter_crop(cfsk_rect rect, uint32_t tile_mode, const cfsk_image_filter* input, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(rect));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(tile_mode <= 3);
    return cfsk::resource(output, SkImageFilters::Crop(cfsk::rect(rect), static_cast<SkTileMode>(tile_mode), cfsk::ref(input)));
}

cfsk_status cfsk_image_filter_matrix(const float matrix[9], cfsk_sampling sampling, const cfsk_image_filter* input, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::finite(matrix, 9) && cfsk::valid(sampling));
    return cfsk::resource(output, SkImageFilters::MatrixTransform(cfsk::matrix(matrix), cfsk::sampling(sampling), cfsk::ref(input)));
}

cfsk_status cfsk_image_filter_runtime(const cfsk_runtime_effect_builder* builder, const cfsk_filter_child* children, size_t child_count, float sample_radius, uint8_t device_resolution, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(sample_radius));
    CFSK_REQUIRE(device_resolution <= 1);
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(builder && child_count <= INT_MAX && (!child_count || children) && sample_radius >= 0);
    std::vector<std::string_view> names;
    std::vector<sk_sp<SkImageFilter>> inputs;
    for (size_t i = 0; i < child_count; ++i) {
        CFSK_REQUIRE(cfsk::valid(children[i].name));
        names.emplace_back(children[i].name.data ? children[i].name.data : "", children[i].name.length);
        auto child = builder->value.effect()->findChild(names.back());
        CFSK_REQUIRE(child && child->type == SkRuntimeEffect::ChildType::kShader);
        inputs.push_back(cfsk::ref(children[i].input));
    }
    return cfsk::resource(output, SkImageFilters::RuntimeShader(builder->value, sample_radius, names.data(), inputs.data(), static_cast<int>(child_count), device_resolution ? SkImageFilters::RuntimeShaderRasterPolicy::kDevice : SkImageFilters::RuntimeShaderRasterPolicy::kParameter));
}

cfsk_status cfsk_image_filter_transform_blur(const float* matrices44, size_t matrix_count, int32_t minimum_samples, int32_t maximum_samples, const cfsk_image_filter* input, cfsk_image_filter** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(matrix_count >= 2 && matrix_count <= 16 && cfsk::finite(matrices44, matrix_count * 16) && minimum_samples > 0 && maximum_samples >= minimum_samples);
    std::vector<SkM44> path;
    for (size_t i = 0; i < matrix_count; ++i) path.push_back(SkM44::RowMajor(matrices44 + i * 16));
    return cfsk::resource(output, SkImageFilters::TransformBlur(SkSpan<const SkM44>(path), minimum_samples, maximum_samples, cfsk::ref(input)));
}

cfsk_status cfsk_shader_glow(const float* lobes, size_t lobe_float_count, const float* core, size_t core_float_count, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(lobe_float_count % 10 == 0 && lobe_float_count <= 320 && (!lobe_float_count || cfsk::finite(lobes, lobe_float_count)) && valid_core(core, core_float_count));
    std::vector<SkGlowShader::Lobe> values;
    for (size_t i = 0; i < lobe_float_count; i += 10) {
        values.push_back({SkRect::MakeLTRB(lobes[i], lobes[i+1], lobes[i+2], lobes[i+3]), {lobes[i+4], lobes[i+5], lobes[i+6], lobes[i+7]}, lobes[i+8], lobes[i+9]});
    }
    auto c = core_float_count ? glow_core(core, core_float_count) : SkGlowShader::Core{};
    return cfsk::resource(output, SkGlowShader::Make(SkSpan<const SkGlowShader::Lobe>(values), core_float_count ? &c : nullptr));
}

cfsk_status cfsk_shader_radial_glow(const cfsk_image* profile, int32_t length, float step, cfsk_color tint, const float* core, size_t core_float_count, cfsk_shader** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(step));
    CFSK_REQUIRE(cfsk::finite(tint));
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(profile && length > 0 && step > 0 && valid_core(core, core_float_count));
    auto c = core_float_count ? glow_core(core, core_float_count) : SkGlowShader::Core{};
    return cfsk::resource(output, SkGlowShader::MakeRadial(profile->value, length, step, {tint.red, tint.green, tint.blue, tint.alpha}, core_float_count ? &c : nullptr));
}
