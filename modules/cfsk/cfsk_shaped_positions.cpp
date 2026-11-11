/* Exact OT shaping positions; each call owns its HarfBuzz font and buffer. */
#include "cfsk_internal.h"
#include "include/core/SkStream.h"
#include "third_party/externals/harfbuzz/src/hb.h"  // nogncheck: already in the cfsk archive closure.
#include <mutex>
#include <unordered_set>

namespace {
struct LiveTypefaces {
    std::mutex mutex;
    std::unordered_set<const cfsk_typeface*> faces;
};

LiveTypefaces& live_typefaces() {
    static LiveTypefaces live;
    return live;
}

// Decode strictly, retaining UTF-16 cluster indices used by harfbuzzjs Buffer.addText.
bool decode_utf8(cfsk_string text, std::vector<uint16_t>& output) {
    for (size_t i = 0; i < text.length;) {
        uint32_t c = static_cast<uint8_t>(text.data[i++]);
        unsigned trailing = 0;
        uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { c &= 0x1f; trailing = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 0x0f; trailing = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 0x07; trailing = 3; minimum = 0x10000; }
        else if (c >= 0x80) return false;
        if (text.length - i < trailing) return false;
        for (unsigned j = 0; j < trailing; ++j) {
            uint8_t next = static_cast<uint8_t>(text.data[i++]);
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        if (c > 0xffff) {
            c -= 0x10000;
            output.push_back(static_cast<uint16_t>(0xd800 + (c >> 10)));
            output.push_back(static_cast<uint16_t>(0xdc00 + (c & 0x3ff)));
        } else output.push_back(static_cast<uint16_t>(c));
    }
    return true;
}

bool printable_ascii(cfsk_string text) {
    if (!cfsk::valid(text)) return false;
    for (size_t i = 0; i < text.length; ++i) {
        uint8_t c = static_cast<uint8_t>(text.data[i]);
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

cfsk_status make_font(const sk_sp<SkTypeface>& face, hb_font_t** output) {
    int index = 0;
    auto stream = face->openStream(&index);
    if (!stream || index < 0) return cfsk::unsupported("typeface has no font bytes");
    CFSK_REQUIRE(stream->getLength() <= UINT_MAX);
    auto data = SkData::MakeFromStream(stream.get(), stream->getLength());
    if (!data) return cfsk::fail(CFSK_FAILED, "font bytes unreadable");
    SkData* bytes = data.release();
    hb_blob_t* blob = hb_blob_create(static_cast<const char*>(bytes->data()),
        static_cast<unsigned>(bytes->size()), HB_MEMORY_MODE_READONLY, bytes,
        [](void* bytes) { static_cast<SkData*>(bytes)->unref(); });
    if (!hb_blob_get_length(blob)) {
        hb_blob_destroy(blob);
        return cfsk::fail(CFSK_OUT_OF_MEMORY, "HarfBuzz blob allocation failed");
    }
    hb_face_t* hb = hb_face_create(blob, static_cast<unsigned>(index));
    hb_blob_destroy(blob);
    if (!hb_face_get_glyph_count(hb)) {
        hb_face_destroy(hb);
        return cfsk::fail(CFSK_FAILED, "HarfBuzz could not read the font");
    }
    unsigned upem = hb_face_get_upem(hb);
    *output = hb_font_create(hb);
    hb_face_destroy(hb);
    if (*output == hb_font_get_empty()) return cfsk::fail(CFSK_OUT_OF_MEMORY, "HB font allocation failed");
    hb_font_set_scale(*output, static_cast<int>(upem * 64), static_cast<int>(upem * 64));
    return CFSK_OK;
}
}  // namespace

cfsk_typeface::cfsk_typeface(sk_sp<SkTypeface> face) : value(std::move(face)) {
    auto& live = live_typefaces();
    std::lock_guard<std::mutex> lock(live.mutex);
    live.faces.insert(this);
}

cfsk_typeface::~cfsk_typeface() {
    auto& live = live_typefaces();
    std::lock_guard<std::mutex> lock(live.mutex);
    live.faces.erase(this);
}

sk_sp<SkTypeface> cfsk_typeface::retain_live(const cfsk_typeface* face) {
    auto& live = live_typefaces();
    std::lock_guard<std::mutex> lock(live.mutex);
    return live.faces.find(face) != live.faces.end() ? face->value : nullptr;
}

cfsk_status cfsk_typeface_shaped_positions(
    const cfsk_typeface* face,
    const cfsk_axis_coordinate* coordinates, size_t coordinate_count,
    const cfsk_font_feature* features, size_t feature_count,
    cfsk_string utf8, uint32_t direction, uint32_t script, cfsk_string language,
    cfsk_glyph_position* output, size_t capacity, size_t* required) {
    CFSK_BEGIN;
    CFSK_REQUIRE(required);
    *required = 0;
    CFSK_REQUIRE((output || !capacity) && cfsk::valid(utf8) && utf8.length <= INT_MAX);
    CFSK_REQUIRE(coordinate_count <= UINT_MAX && (!coordinate_count || coordinates));
    CFSK_REQUIRE(feature_count <= UINT_MAX && (!feature_count || features));
    CFSK_REQUIRE((direction == 0 || (direction >= 4 && direction <= 7)) && language.length <= INT_MAX);
    CFSK_REQUIRE(printable_ascii(language));
    auto retained = cfsk_typeface::retain_live(face);
    CFSK_REQUIRE(retained);
    std::vector<uint16_t> text;
    CFSK_REQUIRE(decode_utf8(utf8, text));
    std::vector<hb_variation_t> variations;
    for (size_t i = 0; i < coordinate_count; ++i) {
        CFSK_REQUIRE(cfsk::finite(coordinates[i].value));
        variations.push_back({coordinates[i].tag, coordinates[i].value});
    }
    std::vector<hb_feature_t> requested;
    for (size_t i = 0; i < feature_count; ++i) {
        const auto& feature = features[i];
        if (feature.tag.length != 4 || !printable_ascii(feature.tag)) {
            return cfsk::unsupported("feature tag must contain four printable ASCII bytes");
        }
        CFSK_REQUIRE(feature.value >= 0);
        requested.push_back({hb_tag_from_string(feature.tag.data, 4),
            static_cast<uint32_t>(feature.value), HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
    }
    hb_font_t* raw_font = nullptr;
    if (cfsk_status status = make_font(retained, &raw_font); status != CFSK_OK) return status;
    std::unique_ptr<hb_font_t, decltype(&hb_font_destroy)> font(raw_font, hb_font_destroy);
    hb_font_set_variations(font.get(), variations.data(), static_cast<unsigned>(variations.size()));
    std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)> buffer(hb_buffer_create(), hb_buffer_destroy);
    hb_buffer_add_utf16(buffer.get(), text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    if (direction) hb_buffer_set_direction(buffer.get(), static_cast<hb_direction_t>(direction));
    if (script) hb_buffer_set_script(buffer.get(), hb_script_from_iso15924_tag(script));
    if (language.length) hb_buffer_set_language(buffer.get(),
        hb_language_from_string(language.data, static_cast<int>(language.length)));
    hb_buffer_guess_segment_properties(buffer.get());
    bool shaped = hb_shape_full(font.get(), buffer.get(), requested.data(),
        static_cast<unsigned>(requested.size()), nullptr);
    if (!hb_buffer_allocation_successful(buffer.get())) {
        return cfsk::fail(CFSK_OUT_OF_MEMORY, "HarfBuzz buffer allocation failed");
    }
    if (!shaped) return cfsk::fail(CFSK_FAILED, "HarfBuzz shaping failed");
    unsigned count = 0;
    const auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), nullptr);
    *required = count;
    if (!output && !capacity) return CFSK_OK;
    if (capacity < count) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "position buffer too small");
    for (unsigned i = 0; i < count; ++i) {
        const auto& p = positions[i];
        output[i] = {infos[i].codepoint, infos[i].cluster, p.x_advance, p.y_advance, p.x_offset, p.y_offset};
    }
    return CFSK_OK;
}
