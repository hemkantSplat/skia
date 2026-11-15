/* Batch sampling retains the single-point SkContourMeasure arithmetic and caller order. */
#include "cfsk_internal.h"
#include <mutex>
#include <unordered_set>

namespace {
struct LiveContours {
    std::mutex mutex;
    std::unordered_set<const cfsk_contour*> contours;
};

LiveContours& live_contours() {
    static LiveContours live;
    return live;
}
}  // namespace

cfsk_contour::cfsk_contour(sk_sp<SkContourMeasure> contour) : value(std::move(contour)) {
    auto& live = live_contours();
    std::lock_guard<std::mutex> lock(live.mutex);
    live.contours.insert(this);
}

cfsk_contour::~cfsk_contour() {
    auto& live = live_contours();
    std::lock_guard<std::mutex> lock(live.mutex);
    live.contours.erase(this);
}

sk_sp<SkContourMeasure> cfsk_contour::retain_live(const cfsk_contour* contour) {
    auto& live = live_contours();
    std::lock_guard<std::mutex> lock(live.mutex);
    return live.contours.find(contour) != live.contours.end() ? contour->value : nullptr;
}

cfsk_status cfsk_contour_pos_tan_batch(
    const cfsk_contour* contour, const float* distances, size_t count,
    uint8_t force_closed, float* positions, float* tangents, uint8_t* success) {
    CFSK_BEGIN;
    CFSK_REQUIRE(force_closed <= 1);
    CFSK_REQUIRE(count <= INT_MAX && count <= PTRDIFF_MAX / (2 * sizeof(float)));
    CFSK_REQUIRE(!count || (distances && positions && tangents && success));
    auto measure = cfsk_contour::retain_live(contour);
    CFSK_REQUIRE(measure && (!force_closed || measure->isClosed()));
    for (size_t i = 0; i < count; ++i) {
        SkPoint p;
        SkVector t;
        success[i] = cfsk::finite(distances[i]) && measure->getPosTan(distances[i], &p, &t);
        if (success[i]) {
            positions[2 * i] = p.x();
            positions[2 * i + 1] = p.y();
            tangents[2 * i] = t.x();
            tangents[2 * i + 1] = t.y();
        }
    }
    return CFSK_OK;
}
