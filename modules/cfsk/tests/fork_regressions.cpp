/* Standalone checks against a production cfsk archive. */
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkImageFilters.h"
#include "src/core/SkMipmap.h"
#include "src/core/SkOpts.h"
#include "src/core/SkRasterPipeline.h"
#include "src/core/SkRasterPipelineOpContexts.h"
#include "src/core/SkRasterPipelineOpList.h"

#if defined(CFSK_TEST_SCALAR_RASTER)
#define SKRP_CPU_SCALAR
#define SK_OPTS_NS cfsk_test_scalar
#include "src/opts/SkRasterPipeline_opts.h"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

sk_sp<SkImageFilter> SkMakeDeviceMaskBlur(float sigma);

static unsigned blur_tail(const SkImageFilter* filter) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(96, 96));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->scale(0.25f, 0.25f);
    SkPaint effect;
    effect.setImageFilter(sk_ref_sp(filter));
    canvas->saveLayer(nullptr, &effect);
    canvas->drawRect(SkRect::MakeLTRB(128, 128, 256, 256), SkPaint());
    canvas->restore();
    SkPixmap pixmap;
    return surface->peekPixels(&pixmap) ? SkColorGetA(pixmap.getColor(23, 48)) : 0;
}

static bool test_blur() {
    const auto filter = SkMakeDeviceMaskBlur(10.f);
    auto bytes = filter->serialize();
    auto decoded = SkImageFilter::Deserialize(bytes->data(), bytes->size());
    const auto source = SkRect::MakeLTRB(128, 128, 256, 256);
    const bool containsTail = filter->computeFastBounds(source).contains(94.f, 194.f);
    const unsigned liveTail = blur_tail(filter.get());
    const unsigned decodedTail = decoded ? blur_tail(decoded.get()) : 0;
    const auto ordinary = SkImageFilters::Blur(10, 10, nullptr);
    const auto ordinaryBytes = ordinary->serialize();
    const auto ordinaryDecoded = SkImageFilter::Deserialize(ordinaryBytes->data(), ordinaryBytes->size());
    const bool ordinaryEqual = ordinaryDecoded &&
            ordinaryDecoded->computeFastBounds(source) == ordinary->computeFastBounds(source);
    std::printf("zoom=0.25 liveTail=%u decodedTail=%u boundsContainTail=%d ordinaryEqual=%d\n",
                liveTail, decodedTail, containsTail, ordinaryEqual);
    return liveTail > 0 && liveTail == decodedTail && containsTail && ordinaryEqual;
}

static bool test_horizon() {
    SkRasterPipelineContexts::MipmapCtx mip{};
    std::fill_n(mip.a, std::size(mip.a), 1.f);
    mip.perPixel = true;
    mip.inv[2] = mip.inv[4] = mip.inv[6] = 1.f;
    mip.inv[8] = -0.5f;
    SkColor4f result{};
    SkRasterPipelineContexts::MemoryCtx out{&result, 1};
    SkSTArenaAlloc<256> alloc;
    SkRasterPipeline pipeline(&alloc);
    pipeline.appendConstantColor(&alloc, SkColor4f{1, 0, 0, 1});
    pipeline.append(SkRasterPipelineOp::mipmap_linear_finish, &mip);
    pipeline.appendStore(kRGBA_F32_SkColorType, &out);
    std::feclearexcept(FE_ALL_EXCEPT);
    pipeline.run(0, 0, 1, 1);
    const int exceptions = std::fetestexcept(FE_DIVBYZERO | FE_INVALID);
    std::printf("horizon red=%g alpha=%g exceptions=%d\n", result.fR, result.fA, exceptions);
    return result.fR == 1.f && result.fA == 1.f && !exceptions;
}

static bool test_mip_strip(const std::array<float, 9>& matrix,
                           const std::array<float, 17>& expected) {
    SkRasterPipelineContexts::MipmapCtx mip{};
    std::copy(matrix.begin(), matrix.end(), mip.inv);
    std::fill_n(mip.a, std::size(mip.a), 1.f);
    mip.perPixel = true;
    mip.levelOffset = 0.5f;
    std::array<SkColor4f, 17> result{};
    SkRasterPipelineContexts::MemoryCtx out{result.data(), result.size()};

#if defined(CFSK_TEST_SCALAR_RASTER)
    using namespace cfsk_test_scalar;
    static_assert(N == 1);
    SkRasterPipelineContexts::UniformColorCtx color{};
    color.r = color.a = 1.f;
    SkRasterPipelineStage stages[] = {
        {(SkOpts::StageFn)uniform_color, &color},
        {(SkOpts::StageFn)mipmap_linear_finish, &mip},
        {(SkOpts::StageFn)store_f32, &out},
        {(SkOpts::StageFn)just_return, nullptr},
    };
    std::feclearexcept(FE_ALL_EXCEPT);
    start_pipeline(0, 0, result.size(), 1, stages, {}, nullptr);
#else
    SkSTArenaAlloc<256> alloc;
    SkRasterPipeline pipeline(&alloc);
    pipeline.appendConstantColor(&alloc, SkColor4f{1, 0, 0, 1});
    pipeline.append(SkRasterPipelineOp::mipmap_linear_finish, &mip);
    pipeline.appendStore(kRGBA_F32_SkColorType, &out);
    std::feclearexcept(FE_ALL_EXCEPT);
    pipeline.run(0, 0, result.size(), 1);
#endif

    const int exceptions = std::fetestexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);
    unsigned differences = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        differences += result[i] != SkColor4f{expected[i], 0, 0, 1};
    }
    std::printf("W=(%a,%a,%a) first=%g middle=%g differences=%u exceptions=%d\n",
                matrix[6], matrix[7], matrix[8], result.front().fR, result[8].fR,
                differences, exceptions);
    return differences == 0 && !exceptions;
}

static bool test_mip_divisor() {
    std::array<float, 17> expected{};
    bool passed = true;
    for (float sign : {1.f, -1.f}) {
        const float zero = std::copysign(0.f, sign);
        expected.fill(0.f);
        passed &= test_mip_strip({0, 0, 0, 0, 0, 0, zero, zero, zero}, expected);
        for (float magnitude : {std::numeric_limits<float>::denorm_min(),
                                0x1p-38f, 0x1p-37f, 0x1p-32f}) {
            const float w = std::copysign(magnitude, sign);
            expected.fill(0.f);
            passed &= test_mip_strip({0, 0, 0, 0, 0, 0, 0, 0, w}, expected);
            expected.fill(0.5f);
            passed &= test_mip_strip({magnitude, 0, 0, 0, 0, 0, 0, 0, w}, expected);
        }
    }
    // For x measured from the horizon, the longer derivative is 1/|x|
    // at every nonzero integer in the strip. W == 0 selects the lower mip.
    expected.fill(0.f);
    expected[7] = expected[9] = 0.5f;
    expected[8] = 1.f;
    constexpr float slope = 0x1p-40f;
    passed &= test_mip_strip({0, 0, 0.5f * slope, 0, slope, 0, slope, 0, -8.5f * slope},
                             expected);
    return passed;
}

static bool test_log2() {
    constexpr uint32_t kInfinity = 0x7f800000;
    uint32_t differences = 0;
    uint32_t mipDifferences = 0;
    for (uint32_t bits = 1; bits < kInfinity; ++bits) {
        const float value = std::bit_cast<float>(bits);
        const float expected = static_cast<float>(std::log2(static_cast<double>(value)));
        const float actual = SkMipmap::LevelLog2(value);
        differences += std::bit_cast<uint32_t>(actual) != std::bit_cast<uint32_t>(expected);
        mipDifferences += value < 1.f &&
                std::max(-actual - 0.5f, 0.f) != std::max(-expected - 0.5f, 0.f);
    }
    const float infinity = std::numeric_limits<float>::infinity();
    const bool special = SkMipmap::LevelLog2(0.f) == -infinity &&
            SkMipmap::LevelLog2(-0.f) == -infinity && SkMipmap::LevelLog2(infinity) == infinity &&
            std::isnan(SkMipmap::LevelLog2(-1.f)) &&
            std::isnan(SkMipmap::LevelLog2(std::numeric_limits<float>::quiet_NaN()));
    std::printf("positiveFinite=%u logDifferences=%u mipDifferences=%u special=%d\n",
                kInfinity - 1, differences, mipDifferences, special);
    return differences == 0 && mipDifferences == 0 && special;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "log2") {
        return !test_log2();
    }
    if (argc == 2 && std::string_view(argv[1]) == "mip") {
        return !test_mip_divisor();
    }
    const bool blur = test_blur();
    const bool horizon = test_horizon();
    const bool divisor = test_mip_divisor();
    return !(blur && horizon && divisor);
}
