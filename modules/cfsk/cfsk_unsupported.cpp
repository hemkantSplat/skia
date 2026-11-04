/* C ABI validation and ownership stay at the boundary; Skia owns rendering. */
#include "cfsk_internal.h"
struct cfsk_skottie {};

cfsk_status cfsk_skottie_make(cfsk_string json, const cfsk_asset* assets, size_t asset_count, cfsk_skottie** output) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::valid(json));
    CFSK_OUTPUT(output);
    return cfsk::unsupported("cfsk_skottie_make: Skottie backend is not built");
}

cfsk_status cfsk_skottie_seek_frame(cfsk_skottie* animation, double frame, cfsk_rect* damage) {
    CFSK_BEGIN;
    CFSK_REQUIRE(cfsk::finite(frame));
    return cfsk::unsupported("cfsk_skottie_seek_frame: Skottie backend is not built");
}

cfsk_status cfsk_skottie_render(cfsk_skottie* animation, cfsk_canvas* canvas, const cfsk_rect* destination) {
    CFSK_BEGIN;
    if (canvas && canvas->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "canvas owner mismatch");
    return cfsk::unsupported("cfsk_skottie_render: Skottie backend is not built");
}

void cfsk_skottie_destroy(cfsk_skottie* value) {
    delete value;
}
