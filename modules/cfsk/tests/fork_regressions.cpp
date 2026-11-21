/* Standalone checks against a production cfsk archive. */
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkImageFilters.h"
#include "src/core/SkMipmap.h"
#include "src/core/SkRasterPipeline.h"
#include "src/core/SkRasterPipelineOpContexts.h"
#include "src/core/SkRasterPipelineOpList.h"

#include <algorithm>
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
    const bool blur = test_blur();
    const bool horizon = test_horizon();
    return !(blur && horizon);
}
