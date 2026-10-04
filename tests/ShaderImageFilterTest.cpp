/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkGraphics.h"
#include "src/core/SkImageFilter_Base.h"
#include "src/core/SkImageFilterCache.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypes.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "tests/CtsEnforcement.h"
#include "tests/Test.h"
#include "tools/EncodeUtils.h"
#include "tools/ToolUtils.h"

#include <vector>

struct GrContextOptions;

static void test_unscaled(skiatest::Reporter* reporter) {
    static const int kWidth = 10;
    static const int kHeight = 10;

    SkIRect ir = SkIRect::MakeWH(kWidth, kHeight);

    SkBitmap filterResult, paintResult;

    filterResult.allocN32Pixels(kWidth, kHeight);
    SkCanvas canvasFilter(filterResult);
    canvasFilter.clear(0x00000000);

    paintResult.allocN32Pixels(kWidth, kHeight);
    SkCanvas canvasPaint(paintResult);
    canvasPaint.clear(0x00000000);

    SkPoint center = SkPoint::Make(SkIntToScalar(5), SkIntToScalar(5));
    SkColor colors[] = {SK_ColorBLUE, SK_ColorRED, SK_ColorGREEN};
    SkScalar pos[] = {0, SK_ScalarHalf, SK_Scalar1};
    SkScalar radius = SkIntToScalar(5);

    sk_sp<SkShader> gradient = SkGradientShader::MakeRadial(
            center, radius, colors, pos, std::size(colors), SkTileMode::kClamp);

    // Test using the image filter
    {
        SkPaint paint;
        paint.setImageFilter(SkImageFilters::Shader(gradient, &ir));
        canvasFilter.drawRect(SkRect::Make(ir), paint);
    }

    // Test using the paint directly
    {
        SkPaint paint;
        paint.setShader(gradient);
        canvasPaint.drawRect(SkRect::Make(ir), paint);
    }

    // Assert that both paths yielded the same result
    for (int y = 0; y < kHeight; ++y) {
        const SkPMColor* filterPtr = filterResult.getAddr32(0, y);
        const SkPMColor* paintPtr = paintResult.getAddr32(0, y);
        for (int x = 0; x < kWidth; ++x, ++filterPtr, ++paintPtr) {
            REPORTER_ASSERT(reporter, *filterPtr == *paintPtr);
        }
    }
}

static void test_scaled(skiatest::Reporter* reporter) {
    static const int kWidth = 10;
    static const int kHeight = 10;

    SkIRect ir = SkIRect::MakeWH(kWidth, kHeight);

    SkBitmap filterResult, paintResult;

    filterResult.allocN32Pixels(kWidth, kHeight);
    SkCanvas canvasFilter(filterResult);
    canvasFilter.clear(0x00000000);

    paintResult.allocN32Pixels(kWidth, kHeight);
    SkCanvas canvasPaint(paintResult);
    canvasPaint.clear(0x00000000);

    SkPoint center = SkPoint::Make(SkIntToScalar(5), SkIntToScalar(5));
    SkColor colors[] = {SK_ColorBLUE, SK_ColorRED, SK_ColorGREEN};
    SkScalar pos[] = {0, SK_ScalarHalf, SK_Scalar1};
    SkScalar radius = SkIntToScalar(5);

    sk_sp<SkShader> gradient = SkGradientShader::MakeRadial(
        center, radius, colors, pos, std::size(colors), SkTileMode::kClamp);

    // Test using the image filter
    {
        SkPaint paint;
        paint.setImageFilter(SkImageFilters::Shader(gradient, &ir));
        canvasFilter.scale(SkIntToScalar(2), SkIntToScalar(2));
        canvasFilter.drawRect(SkRect::Make(ir), paint);
    }

    // Test using the paint directly
    {
        SkPaint paint;
        paint.setShader(gradient);
        canvasPaint.scale(SkIntToScalar(2), SkIntToScalar(2));
        canvasPaint.drawRect(SkRect::Make(ir), paint);
    }

    // Assert that both paths yielded the same result
    if (!ToolUtils::equal_pixels(filterResult, paintResult)) {
        SkString encoded;
        SkString errString("Image filter doesn't match paint reference");
        errString.append("\nExpected: ");
        if (ToolUtils::BitmapToBase64DataURI(paintResult, &encoded)) {
            errString.append(encoded);
        } else {
            errString.append("failed to encode");
        }

        errString.append("\nActual: ");
        if (ToolUtils::BitmapToBase64DataURI(filterResult, &encoded)) {
            errString.append(encoded);
        } else {
            errString.append("failed to encode");
        }

        ERRORF(reporter, "%s\n", errString.c_str());
    }
}

DEF_TEST(ShaderImageFilter, reporter) {
    test_unscaled(reporter);
    test_scaled(reporter);
}

static void test_runtime_shader(skiatest::Reporter* r, SkSurface* surface) {
    sk_sp<SkRuntimeEffect> effect = SkRuntimeEffect::MakeForShader(SkString(R"(
        uniform shader child;
        vec4 main(vec2 coord) {
            return child.eval(coord) * 0.5;
        }
    )"))
                                            .effect;
    SkRuntimeShaderBuilder builder(effect);

    // create a red image filter to feed as input into the SkImageFilters::RuntimeShader
    sk_sp<SkImageFilter> input = SkImageFilters::Shader(SkShaders::Color(SK_ColorRED));

    // Create the different variations of SkImageFilters::RuntimeShader
    // All variations should produce the same pixel output
    std::vector<sk_sp<SkImageFilter>> filters = {
            SkImageFilters::RuntimeShader(builder, /*childShaderName=*/"", input),
            SkImageFilters::RuntimeShader(builder, /*childShaderName=*/"child", input)};

    for (auto&& filter : filters) {
        auto canvas = surface->getCanvas();

        // clear to transparent
        SkPaint paint;
        paint.setColor(SK_ColorTRANSPARENT);
        paint.setBlendMode(SkBlendMode::kSrc);
        canvas->drawPaint(paint);

        SkPaint filterPaint;
        // the green color will be ignored by the filter within the runtime shader
        filterPaint.setColor(SK_ColorGREEN);
        filterPaint.setImageFilter(filter);
        canvas->saveLayer(nullptr, &filterPaint);
        // the blue color will be ignored by the filter because the input to the image filter is not
        // null
        canvas->drawColor(SK_ColorBLUE);
        canvas->restore();

        // This is expected to read back the half transparent red pixel produced by the image filter
        SkBitmap bitmap;
        REPORTER_ASSERT(r, bitmap.tryAllocPixels(surface->imageInfo()));
        REPORTER_ASSERT(r,
                        surface->readPixels(bitmap.info(),
                                            bitmap.getPixels(),
                                            bitmap.rowBytes(),
                                            /*srcX=*/0,
                                            /*srcY=*/0));
        SkColor color = bitmap.getColor(/*x=*/0, /*y=*/0);

        // check alpha with a small tolerance
        SkAlpha alpha = SkColorGetA(color);
        REPORTER_ASSERT(r, alpha >= 127 && alpha <= 129, "Expected: %d Actual: %d", 128, alpha);

        // check each color channel
        color = SkColorSetA(color, 255);
        REPORTER_ASSERT(r, SK_ColorRED == color, "Expected: %08x Actual: %08x", SK_ColorRED, color);
    }
}


DEF_TEST(RuntimeImageFilterDevicePolicy, r) {
    using Policy = SkImageFilters::RuntimeShaderRasterPolicy;
    auto effect = SkRuntimeEffect::MakeForShader(SkString(
            "uniform shader source; half4 main(float2 p) { return source.eval(p); }")).effect;
    SkRuntimeShaderBuilder builder(effect);
    std::string_view name = "source";
    sk_sp<SkImageFilter> input;
    auto legacy = SkImageFilters::RuntimeShader(builder, 0.f, &name, &input, 1);
    auto device = SkImageFilters::RuntimeShader(builder, 0.f, &name, &input, 1, Policy::kDevice);
    REPORTER_ASSERT(r, legacy && device);
    REPORTER_ASSERT(r, as_IFB(legacy)->uniqueID() != as_IFB(device)->uniqueID());
    REPORTER_ASSERT(r, as_IFB(device)->getCTMCapability() == skif::MatrixCapability::kComplex);
    REPORTER_ASSERT(r, as_IFB(legacy)->getCTMCapability() == skif::MatrixCapability::kTranslate);
    input = SkImageFilters::Blur(1.f, 2.f, SkTileMode::kClamp, nullptr);
    auto graph = SkImageFilters::RuntimeShader(builder, 0.f, &name, &input, 1, Policy::kDevice);
    REPORTER_ASSERT(r, as_IFB(graph)->getCTMCapability() == skif::MatrixCapability::kComplex);

#if !defined(SK_DISABLE_EFFECT_DESERIALIZATION)
    for (auto filter : {legacy, device, graph}) {
        auto data = filter->serialize();
        auto restored = SkImageFilter::Deserialize(data->data(), data->size());
        REPORTER_ASSERT(r, restored);
        REPORTER_ASSERT(r, as_IFB(restored)->getCTMCapability() == as_IFB(filter)->getCTMCapability());
        auto roundtrip = restored->serialize();
        REPORTER_ASSERT(r, data->equals(roundtrip.get()));
    }
    REPORTER_ASSERT(r, !legacy->serialize()->equals(device->serialize().get()));
#endif

    auto render = [&](const sk_sp<SkImageFilter>& filter, float scale) {
        auto info = SkImageInfo::Make(64, 64, kRGBA_F16_SkColorType, kPremul_SkAlphaType);
        auto surface = SkSurfaces::Raster(info);
        auto canvas = surface->getCanvas();
        SkPaint paint;
        for (int x = 0; x < 64; x += 4) {
            paint.setColor(x % 8 ? SK_ColorWHITE : SK_ColorBLACK);
            canvas->drawRect(SkRect::MakeXYWH(x, 0, 4, 64), paint);
        }
        canvas->scale(scale, scale);
        if (filter) {
            SkCanvas::SaveLayerRec rec;
            rec.fBackdrop = filter.get();
            rec.fSaveLayerFlags = SkCanvas::kF16ColorType;
            canvas->saveLayer(rec);
            canvas->restore();
        }
        SkBitmap pixels;
        pixels.allocPixels(info);
        REPORTER_ASSERT(r, surface->readPixels(pixels, 0, 0));
        return pixels;
    };
    auto expected = render(nullptr, 1.f);
    for (float scale : {.5f, 1.f, 1.5f, 2.f, 3.f}) {
        auto actual = render(device, scale);
        REPORTER_ASSERT(r, ToolUtils::equal_pixels(actual, expected));
#if !defined(SK_DISABLE_EFFECT_DESERIALIZATION)
        auto data = device->serialize();
        auto restored = SkImageFilter::Deserialize(data->data(), data->size());
        actual = render(restored, scale);
        REPORTER_ASSERT(r, ToolUtils::equal_pixels(actual, expected));
#endif
    }
    REPORTER_ASSERT(r, !ToolUtils::equal_pixels(render(legacy, 2.f), expected));
}

DEF_TEST(RuntimeImageFilterMappingCache, r) {
    SkImageFilterCacheKey base(7, SkMatrix::I(), SkIRect::MakeWH(64,64), 9,
                             SkIRect::MakeWH(64,64), kRGBA_F16_SkColorType, nullptr);
    SkImageFilterCacheKey rotated(7, SkMatrix::I(), SkIRect::MakeWH(64,64), 9,
                             SkIRect::MakeWH(64,64), kRGBA_F16_SkColorType, nullptr,
                             SkMatrix::RotateDeg(30));
    REPORTER_ASSERT(r, !(base == rotated));
    auto cache = SkImageFilterCache::Create(4096);
    auto filter = SkImageFilters::Blur(1, 1, nullptr);
    cache->set(base, filter.get(), {});
    skif::FilterResult result;
    REPORTER_ASSERT(r, cache->get(base, &result));
    REPORTER_ASSERT(r, !cache->get(rotated, &result));
}

DEF_TEST(SkRuntimeShaderImageFilter_CPU, r) {
    const SkImageInfo info = SkImageInfo::MakeN32Premul(/*width=*/1, /*height=*/1);
    sk_sp<SkSurface> surface(SkSurfaces::Raster(info));
    test_runtime_shader(r, surface.get());
}

DEF_GANESH_TEST_FOR_RENDERING_CONTEXTS(SkRuntimeShaderImageFilter_GPU,
                                       r,
                                       ctxInfo,
                                       CtsEnforcement::kApiLevel_T) {
    const SkImageInfo info = SkImageInfo::MakeN32Premul(/*width=*/1, /*height=*/1);
    sk_sp<SkSurface> surface(
            SkSurfaces::RenderTarget(ctxInfo.directContext(), skgpu::Budgeted::kNo, info));
    test_runtime_shader(r, surface.get());
}
