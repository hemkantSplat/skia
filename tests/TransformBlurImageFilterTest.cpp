/*
 * Copyright 2026 CourseForge
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkM44.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSamplingOptions.h"
#include "include/effects/SkImageFilters.h"
#include "include/private/base/SkDebug.h"
#include "src/effects/imagefilters/SkTransformBlurImageFilter.h"
#include "tests/Test.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

namespace {

constexpr int kSize = 448;
// A transparent border keeps image-edge coverage out of the comparison.
constexpr int kMargin = 12;
constexpr SkPoint kCentre = {kSize / 2.f, kSize / 2.f};
constexpr int kOracleSamples = 256;

using Colour = std::array<float, 4>;

// The content's transform at shutter time t in [0, 1]; the input is the content at t = 1.
using Motion = std::function<SkM44(float)>;

SkM44 about(SkPoint pivot, const SkM44& m) {
    return SkM44::Translate(pivot.fX, pivot.fY) * m * SkM44::Translate(-pivot.fX, -pivot.fY);
}

SkM44 rotate_deg(float degrees) { return SkM44::Rotate({0, 0, 1}, degrees * SK_ScalarPI / 180); }

float cubic_out(float t) { return 1 - std::pow(1 - t, 3.f); }

float cubic_in_out(float t) {
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2;
}

// A 16 px checker, stroked rings and a translucent bar: edges in every direction.
SkBitmap make_input() {
    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::Make(kSize, kSize, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType));
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas(bitmap);
    canvas.clipRect(SkRect::MakeLTRB(kMargin, kMargin, kSize - kMargin, kSize - kMargin));
    SkPaint paint;
    for (int y = kMargin; y < kSize - kMargin; y += 16) {
        for (int x = kMargin; x < kSize - kMargin; x += 16) {
            paint.setColor(((x + y) / 16) % 2 ? 0xFF2050D0 : 0xFFF0E0B0);
            canvas.drawRect(SkRect::MakeXYWH(x, y, 16, 16), paint);
        }
    }
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(3);
    paint.setColor(0xFFD02020);
    for (float r = 30; r < 200; r += 24) {
        canvas.drawCircle(kCentre, r, paint);
    }
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(0x80FFFFFF);
    canvas.drawRect(SkRect::MakeXYWH(kMargin, 180, kSize - 2 * kMargin, 40), paint);
    bitmap.setImmutable();
    return bitmap;
}

// Premultiplied bilinear taps with decal edges, texel centres at +0.5, as Skia samples.
Colour bilinear(const SkBitmap& bitmap, SkPoint p) {
    const float x = p.fX - 0.5f, y = p.fY - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    Colour c = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int tx = x0 + i, ty = y0 + j;
            if (tx < 0 || ty < 0 || tx >= bitmap.width() || ty >= bitmap.height()) {
                continue;
            }
            const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const uint8_t* texel = static_cast<const uint8_t*>(bitmap.getAddr(tx, ty));
            for (int k = 0; k < 4; ++k) {
                c[k] += w * texel[k] / 255.f;
            }
        }
    }
    return c;
}

// Where output pixel p samples the input at time t: current * plane(t)^-1.
struct Shutter {
    Motion motion;
    SkMatrix current;

    explicit Shutter(Motion m) : motion(std::move(m)), current(motion(1).asM33()) {}

    SkPoint toInput(float t, SkPoint p) const {
        SkMatrix inverse;
        SkAssertResult(motion(t).asM33().invert(&inverse));
        return SkMatrix::Concat(current, inverse).mapPoint(p);
    }

    std::vector<SkM44> path(int entries) const {
        std::vector<SkM44> path;
        for (int i = 0; i < entries; ++i) {
            path.push_back(motion(static_cast<float>(i) / (entries - 1)));
        }
        return path;
    }
};

// A pixel's input positions at the path entries: the straight-segment path the filter follows.
struct Polyline {
    std::vector<SkPoint> points;
    std::vector<float> lengths;
    float total = 0;

    Polyline(const Shutter& shutter, int entries, SkPoint p) {
        for (int i = 0; i < entries; ++i) {
            points.push_back(shutter.toInput(static_cast<float>(i) / (entries - 1), p));
            if (i) {
                lengths.push_back(SkPoint::Distance(points[i - 1], points[i]));
                total += lengths.back();
            }
        }
    }

    int samples(int minSamples, int maxSamples) const {
        return std::clamp(static_cast<int>(std::ceil(total)), minSamples, maxSamples);
    }

    SkPoint atTime(float t) const {
        const float scaled = t * lengths.size();
        const int i = std::min(static_cast<int>(scaled), static_cast<int>(lengths.size()) - 1);
        const float u = scaled - i;
        return points[i] + (points[i + 1] - points[i]) * u;
    }

    struct Slot {
        SkPoint centroid;
        float weight;
    };

    // 'count' slots equal in blend * arc / total + (1 - blend) * time, each at its time-weighted
    // centroid with the shutter time it spans. The filter's rule is kArcTimeBlend.
    std::vector<Slot> slots(int count, float blend) const {
        const float dt = 1.f / lengths.size(), slot = 1.f / count;
        std::vector<Slot> slots;
        SkPoint centroid = {0, 0};
        float weight = 0, start = 0;
        int closed = 0;
        auto close = [&] {
            if (weight > 0) {
                slots.push_back({centroid * (1 / weight), weight});
            }
            centroid = {0, 0};
            weight = 0;
        };
        for (size_t i = 0; i < lengths.size(); ++i) {
            const float len = lengths[i];
            float u0 = 0;
            while (true) {
                const float g0 = blend * start / total + (1 - blend) * i * dt;
                const float g1 = blend * (start + len) / total + (1 - blend) * (i + 1) * dt;
                const float boundary = (closed + 1) * slot;
                const bool closes = closed + 1 < count && boundary <= g1;
                const float u1 = closes && g1 > g0 ? (boundary - g0) / (g1 - g0) : 1;
                const float w = (u1 - u0) * dt;
                centroid += (points[i] + (points[i + 1] - points[i]) * (0.5f * (u0 + u1))) * w;
                weight += w;
                if (!closes) {
                    break;
                }
                close();
                ++closed;
                u0 = u1;
            }
            start += len;
        }
        close();
        return slots;
    }
};

using Estimator = std::function<Colour(SkPoint)>;

// Every pixel of an estimator over the output, premultiplied RGBA.
std::vector<Colour> render(const Estimator& estimate) {
    std::vector<Colour> out(kSize * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            out[y * kSize + x] = estimate({x + 0.5f, y + 0.5f});
        }
    }
    return out;
}

Colour accumulate(const SkBitmap& input, int count, const std::function<SkPoint(int)>& at,
                  const std::function<float(int)>& weight) {
    Colour c = {0, 0, 0, 0};
    for (int k = 0; k < count; ++k) {
        const Colour tap = bilinear(input, at(k));
        for (int i = 0; i < 4; ++i) {
            c[i] += tap[i] * weight(k);
        }
    }
    return c;
}

// The shutter integral: kOracleSamples taps uniform in time on the true motion.
Estimator oracle(const SkBitmap& input, const Shutter& shutter) {
    return [&input, &shutter](SkPoint p) {
        return accumulate(input, kOracleSamples,
                          [&](int k) { return shutter.toInput((k + 0.5f) / kOracleSamples, p); },
                          [](int) { return 1.f / kOracleSamples; });
    };
}

// The same K as the filter, taps uniform in time instead of in arc length.
Estimator uniform_in_time(const SkBitmap& input, const Shutter& shutter, int entries,
                          int minSamples, int maxSamples) {
    return [=, &input, &shutter](SkPoint p) {
        const Polyline line(shutter, entries, p);
        const int count = line.samples(minSamples, maxSamples);
        return accumulate(input, count, [&](int k) { return line.atTime((k + 0.5f) / count); },
                          [&](int) { return 1.f / count; });
    };
}

// The filter's rule on the CPU at 'blend' (1 is pure arc length): one tap per slot at its centroid.
Estimator slot_reference(const SkBitmap& input, const Shutter& shutter, int entries,
                         int minSamples, int maxSamples,
                         float blend = SkTransformBlurPriv::kArcTimeBlend) {
    return [=, &input, &shutter](SkPoint p) {
        const Polyline line(shutter, entries, p);
        if (line.total == 0) {
            return bilinear(input, p);
        }
        const std::vector<Polyline::Slot> slots = line.slots(line.samples(minSamples, maxSamples), blend);
        return accumulate(input, static_cast<int>(slots.size()),
                          [&](int k) { return slots[k].centroid; },
                          [&](int k) { return slots[k].weight; });
    };
}

std::vector<Colour> read(const SkBitmap& bitmap) {
    std::vector<Colour> out(kSize * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const uint8_t* texel = static_cast<const uint8_t*>(bitmap.getAddr(x, y));
            for (int k = 0; k < 4; ++k) {
                out[y * kSize + x][k] = texel[k] / 255.f;
            }
        }
    }
    return out;
}

SkBitmap filter_output(const SkBitmap& input, sk_sp<SkImageFilter> filter) {
    SkBitmap out;
    out.allocPixels(input.info());
    out.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas(out);
    SkPaint paint;
    paint.setImageFilter(std::move(filter));
    canvas.drawImage(input.asImage(), 0, 0, SkSamplingOptions(), &paint);
    return out;
}

struct Error {
    float max = 0;
    float mean = 0;
};

// Per channel, in 8-bit LSBs; the mean covers pixels either side draws.
Error compare(const std::vector<Colour>& a, const std::vector<Colour>& b) {
    Error e;
    double sum = 0;
    int covered = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i][3] == 0 && b[i][3] == 0) {
            continue;
        }
        ++covered;
        for (int k = 0; k < 4; ++k) {
            const float d = std::abs(a[i][k] - b[i][k]) * 255;
            e.max = std::max(e.max, d);
            sum += d;
        }
    }
    e.mean = covered ? static_cast<float>(sum / (4.0 * covered)) : 0;
    return e;
}

// Measured error plus headroom; docs/skia/canvaskit-custom-build.md (Motion blur) has the table.
struct Tolerance {
    float maxLSB;
    float meanLSB;
};

struct Case {
    const char* name;
    Motion motion;
    int entries;
    std::optional<Tolerance> tolerance;  // none: informational, e.g. 2 entries for a curved motion
};

std::vector<Case> cases() {
    return {
        {"translate-x", [](float t) { return SkM44::Translate(-40 * (1 - t), 0); }, 2,
         Tolerance{1, 0.25f}},
        {"rotate-45", [](float t) { return about(kCentre, rotate_deg(-45 * (1 - t))); }, 16,
         Tolerance{10, 0.75f}},
        {"rotate-45 (2 entries)",
         [](float t) { return about(kCentre, rotate_deg(-45 * (1 - t))); }, 2, {}},
        {"scale",
         [](float t) {
             const float s = 0.8f + 0.2f * t;
             return about(kCentre, SkM44::Scale(s, s));
         },
         16, Tolerance{10, 0.35f}},
        {"scale (2 entries)",
         [](float t) {
             const float s = 0.8f + 0.2f * t;
             return about(kCentre, SkM44::Scale(s, s));
         },
         2, {}},
        {"combined",
         [](float t) {
             const float s = 0.85f + 0.15f * t;
             return SkM44::Translate(-30 * (1 - t), 20 * (1 - t)) *
                    about({150, 260}, rotate_deg(-30 * (1 - t)) * SkM44::Scale(s, s));
         },
         16, Tolerance{10.5f, 0.6f}},
        {"cubic-out translate",
         [](float t) { return SkM44::Translate(-60 * (1 - cubic_out(t)), 0); }, 16,
         Tolerance{3, 0.4f}},
        {"ease-in-out rotate",
         [](float t) { return about(kCentre, rotate_deg(-40 * (1 - cubic_in_out(t)))); }, 16,
         Tolerance{13, 0.95f}},
    };
}

constexpr int kMinSamples = 2;
constexpr int kMaxSamples = 64;

bool within(const Error& e, const Tolerance& t) { return e.max <= t.maxLSB && e.mean <= t.meanLSB; }

}  // namespace

DEF_TEST(TransformBlurImageFilter_Contract, reporter) {
    const SkM44 a = SkM44::Translate(10, 0), b = SkM44::Translate(20, 5);
    const sk_sp<SkImageFilter> input = SkImageFilters::Offset(3, 4, nullptr);

    // Identity paths and singular entries leave the input itself.
    const std::array<SkM44, 3> still = {a, a, a};
    REPORTER_ASSERT(reporter, SkImageFilters::TransformBlur(still, 2, 64, input) == input);
    const std::array<SkM44, 2> singular = {SkM44::Scale(0, 1), b};
    REPORTER_ASSERT(reporter, SkImageFilters::TransformBlur(singular, 2, 64, input) == input);

    // Invalid arguments.
    const std::array<SkM44, 1> single = {a};
    REPORTER_ASSERT(reporter, !SkImageFilters::TransformBlur(single, 2, 64, input));
    const std::vector<SkM44> tooLong(17, a);
    REPORTER_ASSERT(reporter, !SkImageFilters::TransformBlur(tooLong, 2, 64, input));
    const std::array<SkM44, 2> moving = {a, b};
    REPORTER_ASSERT(reporter, !SkImageFilters::TransformBlur(moving, 0, 64, input));
    REPORTER_ASSERT(reporter, !SkImageFilters::TransformBlur(moving, 8, 4, input));
    SkM44 nan = a;
    nan.setRC(0, 3, SK_ScalarNaN);
    const std::array<SkM44, 2> notFinite = {nan, b};
    REPORTER_ASSERT(reporter, !SkImageFilters::TransformBlur(notFinite, 2, 64, input));
    REPORTER_ASSERT(reporter, SkImageFilters::TransformBlur(moving, 2, 64, input) != input);
}

DEF_TEST(TransformBlurImageFilter_MatchesShutterOracle, reporter) {
    const SkBitmap input = make_input();
    for (const Case& c : cases()) {
        const Shutter shutter(c.motion);
        const std::vector<SkM44> path = shutter.path(c.entries);
        const std::vector<Colour> truth = render(oracle(input, shutter));
        const std::vector<Colour> skia = read(filter_output(
                input, SkImageFilters::TransformBlur(path, kMinSamples, kMaxSamples, nullptr)));
        const Error filter = compare(skia, truth);
        const Error uniform = compare(
                render(uniform_in_time(input, shutter, c.entries, kMinSamples, kMaxSamples)),
                truth);
        const std::vector<Colour> cpuRule =
                render(slot_reference(input, shutter, c.entries, kMinSamples, kMaxSamples));
        const Error rule = compare(skia, cpuRule);
        const Error ruleFloat = compare(cpuRule, truth);
        const Error arcOnly = compare(
                render(slot_reference(input, shutter, c.entries, kMinSamples, kMaxSamples, 1)),
                truth);
        SkDebugf("TransformBlur %-22s entries %2d | filter vs oracle max %6.2f mean %6.3f | "
                 "rule(float) max %6.2f mean %6.3f | pure arc max %6.2f mean %6.3f | "
                 "uniform-in-time max %6.2f mean %6.3f | filter vs CPU rule max %5.2f\n",
                 c.name, c.entries, filter.max, filter.mean, ruleFloat.max, ruleFloat.mean,
                 arcOnly.max, arcOnly.mean, uniform.max, uniform.mean, rule.max);
        REPORTER_ASSERT(reporter, rule.max <= 1, "%s: %g", c.name, rule.max);
        if (c.tolerance) {
            REPORTER_ASSERT(reporter, within(filter, *c.tolerance), "%s: max %g mean %g", c.name,
                            filter.max, filter.mean);
        }
    }
}

DEF_TEST(TransformBlurImageFilter_BandingOnset, reporter) {
    const SkBitmap input = make_input();
    const Case rotate = cases()[1];
    int onset = 0;
    const Shutter shutter(rotate.motion);
    const std::vector<Colour> truth = render(oracle(input, shutter));
    for (int maxSamples : {64, 48, 32, 24, 16, 12, 8}) {
        const std::vector<Colour> skia = read(filter_output(
                input, SkImageFilters::TransformBlur(shutter.path(rotate.entries), kMinSamples,
                                                     maxSamples, nullptr)));
        const Error e = compare(skia, truth);
        SkDebugf("TransformBlur rotate-45 maxSamples %2d | filter vs oracle max %6.2f mean %6.3f\n",
                 maxSamples, e.max, e.mean);
        if (!onset && !within(e, *rotate.tolerance)) {
            onset = maxSamples;
        }
    }
    SkDebugf("TransformBlur rotate-45 banding onset: maxSamples %d exceeds tolerance\n", onset);
    REPORTER_ASSERT(reporter, onset < kMaxSamples, "the default cap must meet the tolerance");
}

DEF_TEST(TransformBlurImageFilter_SerializeRoundTrip, reporter) {
    const SkBitmap input = make_input();
    const Shutter shutter(cases()[1].motion);
    const sk_sp<SkImageFilter> filter = SkImageFilters::TransformBlur(
            shutter.path(16), 3, 48, SkImageFilters::Offset(2, 1, nullptr),
            SkRect::MakeLTRB(20, 20, 400, 400));
    const sk_sp<SkData> data = filter->serialize();
    const sk_sp<SkImageFilter> read = SkImageFilter::Deserialize(data->data(), data->size());
    REPORTER_ASSERT(reporter, read);
    if (!read) {
        return;
    }
    REPORTER_ASSERT(reporter, read->serialize()->equals(data.get()));
    const SkBitmap a = filter_output(input, filter), b = filter_output(input, read);
    REPORTER_ASSERT(reporter, memcmp(a.getPixels(), b.getPixels(), a.computeByteSize()) == 0);
}
