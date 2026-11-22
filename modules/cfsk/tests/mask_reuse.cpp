#include "include/core/SkBitmap.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkStrokeRec.h"
#include "src/gpu/graphite/RasterPathUtils.h"
#include "src/gpu/graphite/geom/Shape.h"
#include "src/gpu/graphite/geom/Transform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

using skgpu::UniqueKey;
using skgpu::graphite::RasterMaskHelper;
using skgpu::graphite::Shape;
using skgpu::graphite::Transform;

namespace {
const skvx::half2 kSize{17, 13};
const SkStrokeRec kFill{SkStrokeRec::kFill_InitStyle};

UniqueKey key(const Shape& shape, const Transform& transform, SkIVector offset,
              skvx::half2 size = kSize, const SkStrokeRec& style = kFill) {
#if defined(CFSK_TEST_FULL_MASK_KEY)
    (void)offset;
    return skgpu::graphite::GeneratePathMaskKey(shape, transform, style, {0, 0}, size);
#else
    return skgpu::graphite::GenerateExactPathMaskKey(shape, transform, style, offset, size);
#endif
}

Shape path(float notch) {
    SkPathBuilder builder;
    builder.moveTo(1.f, 1.f)
           .lineTo(15.f, 0.5f)
           .quadTo(11.5f, notch, 14.f, 12.f)
           .lineTo(1.5f, 11.5f)
           .close();
    return Shape(builder.detach());
}

SkBitmap raster(const Shape& shape, const Transform& transform, SkIVector offset) {
    auto [bitmap, mask] = RasterMaskHelper::Allocate({kSize.x(), kSize.y()}, -offset, 1);
    mask.drawShape(shape, transform, kFill);
    return std::move(bitmap);
}

bool equal_pixels(const SkBitmap& a, const SkBitmap& b) {
    return a.computeByteSize() == b.computeByteSize() &&
           std::memcmp(a.getPixels(), b.getPixels(), a.computeByteSize()) == 0;
}

bool hold(int count) {
    std::vector<std::pair<UniqueKey, SkBitmap>> masks;
    int generated = 0;
    bool pixelsMatch = true;
    for (bool scrolled : {false, true}) {
        generated = 0;
        for (int i = 0; i <= count; ++i) {
            const int x = 20 * (i + 1) + (scrolled ? -24 : 0);
            const int y = scrolled ? 34 : 2;
            const SkIVector offset{x, y};
            const float phase = scrolled && i == 0 ? 0.625f : 0.125f;
            const Transform transform = Transform::Translate(x + phase, y + 0.375f);
            const Shape shape = path(1.f + i * 0.0625f);
            const UniqueKey maskKey = key(shape, transform, offset);
            const auto found = std::find_if(masks.begin(), masks.end(), [&](const auto& mask) {
                return mask.first == maskKey;
            });
            if (found == masks.end()) {
                masks.emplace_back(maskKey, raster(shape, transform, offset));
                ++generated;
            } else {
                pixelsMatch &= equal_pixels(found->second, raster(shape, transform, offset));
            }
        }
    }
    std::printf("static=%d generated=%d pixels=%s\n", count, generated,
                pixelsMatch ? "equal" : "different");
    return generated == 1 && pixelsMatch;
}

bool distinguishes_masks() {
    const Shape shape = path(4.f);
    const Transform transform = Transform::Translate(20.125f, -2.5f);
    const SkIVector offset{20, -3};
    const auto original = key(shape, transform, offset);
    SkStrokeRec stroke(SkStrokeRec::kHairline_InitStyle);
    return original != key(shape, transform, {21, -3}) &&
           original != key(shape, transform, offset, {16, 13}) &&
           original != key(shape, transform, offset, kSize, stroke) &&
           original != key(shape, Transform::Translate(20.1259765625f, -2.5f), offset);
}
}  // namespace

int main() {
    const bool held = hold(16) & hold(32) & hold(64);
    const bool distinctive = distinguishes_masks();
    std::printf("distinct coverage inputs=%s\n", distinctive ? "yes" : "no");
    return held && distinctive ? EXIT_SUCCESS : EXIT_FAILURE;
}
