#include "include/utils/SkPathOffset.h"
#include "clipper2/clipper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
constexpr double kScale = 65536;
constexpr size_t kMaxPoints = 1 << 20;
using namespace Clipper2Lib;

struct Homogeneous {
    double x, y, w;
    Homogeneous midpoint(const Homogeneous& b) const {
        return {(x + b.x) * .5, (y + b.y) * .5, (w + b.w) * .5};
    }
};

class Flattener {
public:
    explicit Flattener(double tolerance) : fError(tolerance) {}

    bool append(const Homogeneous& p) {
        double x = p.x / p.w, y = p.y / p.w;
        if (!std::isfinite(x) || !std::isfinite(y) ||
            std::max(std::abs(x), std::abs(y)) > 1e9 || ++fCount > kMaxPoints) {
            return false;
        }
        Point64 point(std::llround(x * kScale), std::llround(y * kScale));
        if (fContour.empty() || point != fContour.back()) fContour.push_back(point);
        return true;
    }

    bool curve(std::array<Homogeneous, 4> p, int degree, int depth = 0) {
        const double ax = p[0].x / p[0].w, ay = p[0].y / p[0].w;
        const double dx = p[degree].x / p[degree].w - ax;
        const double dy = p[degree].y / p[degree].w - ay;
        const double length = dx * dx + dy * dy;
        double error = 0;
        for (int i = 1; i < degree; ++i) {
            double x = p[i].x / p[i].w - ax, y = p[i].y / p[i].w - ay;
            double t = length ? std::clamp((x * dx + y * dy) / length, 0.0, 1.0) : 0;
            error = std::max(error, std::hypot(x - t * dx, y - t * dy));
        }
        // Positive rational weights keep the curve inside this projected control hull.
        if (error <= fError) return append(p[degree]);
        if (depth == 24) return false;
        std::array<Homogeneous, 4> left{}, right{};
        left[0] = p[0]; right[degree] = p[degree];
        for (int level = degree; level > 0; --level) {
            for (int i = 0; i < level; ++i) p[i] = p[i].midpoint(p[i + 1]);
            left[degree - level + 1] = p[0]; right[level - 1] = p[level - 1];
        }
        return curve(left, degree, depth + 1) && curve(right, degree, depth + 1);
    }

    void close() {
        if (fContour.size() > 1 && fContour.front() == fContour.back()) fContour.pop_back();
        if (fContour.size() >= 3) paths.push_back(std::move(fContour));
        fContour.clear();
    }

    Paths64 paths;
private:
    double fError;
    size_t fCount = 0;
    Path64 fContour;
};
}

bool SkPathOffset(const SkPath& src, SkScalar distance, SkPathOffsetJoin join,
                  SkScalar miterLimit, SkScalar tolerance, SkPath* dst) {
    if (!dst || !std::isfinite(distance) || !std::isfinite(miterLimit) || miterLimit < 1 ||
        !std::isfinite(tolerance) || tolerance <= 0 || !src.isFinite() ||
        join < SkPathOffsetJoin::Round || join > SkPathOffsetJoin::Bevel) return false;
    if (distance == 0) { *dst = src; return true; }
    if (src.isInverseFillType() || std::abs(distance) > 1e9 || tolerance < 2 / kScale) return false;

    // Reserve one quarter each for flattening and arc sagitta; rounding uses the remainder.
    Flattener flat(tolerance * .25);
    SkPath::RawIter iter(src);
    SkPoint points[4];
    for (SkPath::Verb verb; (verb = iter.next(points)) != SkPath::kDone_Verb;) {
        if (verb == SkPath::kClose_Verb) { flat.close(); continue; }
        if (verb == SkPath::kMove_Verb) flat.close();
        int degree = verb == SkPath::kCubic_Verb ? 3 :
                     verb == SkPath::kQuad_Verb || verb == SkPath::kConic_Verb ? 2 : 0;
        if (!degree) {
            SkPoint p = points[verb == SkPath::kLine_Verb ? 1 : 0];
            if (!flat.append({p.x(), p.y(), 1})) return false;
            continue;
        }
        std::array<Homogeneous, 4> controls{};
        for (int i = 0; i <= degree; ++i) {
            double w = verb == SkPath::kConic_Verb && i == 1 ? iter.conicWeight() : 1;
            if (!(w > 0) || !std::isfinite(w)) return false;
            controls[i] = {points[i].x() * w, points[i].y() * w, w};
        }
        if (!flat.curve(controls, degree)) return false;
    }
    flat.close();
    const FillRule fill = src.getFillType() == SkPathFillType::kEvenOdd ?
                          FillRule::EvenOdd : FillRule::NonZero;
    // Resolve overlapping contours and orient counters before offsetting the fill boundary.
    thread_local Clipper64 clipper;
    clipper.Clear();
    clipper.AddSubject(flat.paths);
    Paths64 boundary;
    clipper.Execute(ClipType::Union, fill, boundary);
    clipper.Clear();
    JoinType type = join == SkPathOffsetJoin::Round ? JoinType::Round :
                    join == SkPathOffsetJoin::Miter ? JoinType::Miter : JoinType::Bevel;
    // Retain scratch capacity per thread, clearing all input geometry after each use.
    thread_local ClipperOffset offset;
    offset.Clear();
    offset.MiterLimit(miterLimit);
    offset.ArcTolerance(tolerance * .25 * kScale);
    offset.MiterBevelFallback(true);
    offset.AddPaths(boundary, type, EndType::Polygon);
    Paths64 result;
    offset.Execute(distance * kScale, result);
    offset.Clear();
    SkPath output;
    for (const auto& contour : result) {
        if (contour.empty()) continue;
        output.moveTo(contour.front().x / kScale, contour.front().y / kScale);
        for (size_t i = 1; i < contour.size(); ++i) {
            output.lineTo(contour[i].x / kScale, contour[i].y / kScale);
        }
        output.lineTo(contour.front().x / kScale, contour.front().y / kScale);
        output.close();
    }
    output.setFillType(SkPathFillType::kWinding);
    *dst = std::move(output);
    return true;
}
