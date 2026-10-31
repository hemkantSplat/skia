/* A picture retains decoded resources; serialization embeds CPU pixels and fonts. */
#include "cfsk_internal.h"
#include "include/core/SkSerialProcs.h"
#include "include/encode/SkPngEncoder.h"

namespace {
sk_sp<const SkData> serialize_image(SkImage* image, void* context) {
    auto data = SkPngEncoder::Encode(nullptr, image, {});
    if (!data) *static_cast<bool*>(context) = false;
    return data;
}
sk_sp<const SkData> serialize_typeface(SkTypeface* typeface, void* context) {
    auto data = typeface->serialize(SkTypeface::SerializeBehavior::kDoIncludeData);
    if (!data) *static_cast<bool*>(context) = false;
    return data;
}
}

cfsk_status cfsk_picture_deserialize(cfsk_bytes bytes, cfsk_picture** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(cfsk::valid(bytes));
    auto picture = SkPicture::MakeFromData(bytes.data, bytes.length);
    if (!picture) return cfsk::fail(CFSK_FAILED, "invalid or truncated SkPicture data");
    return cfsk::resource(output, std::move(picture));
}

cfsk_status cfsk_picture_serialize(const cfsk_picture* picture, cfsk_data** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    CFSK_REQUIRE(picture);
    bool complete = true;
    SkSerialProcs procs;
    procs.fImageProc = serialize_image;
    procs.fImageCtx = &complete;
    procs.fTypefaceProc = serialize_typeface;
    procs.fTypefaceCtx = &complete;
    auto data = picture->value->serialize(&procs);
    if (!complete) return cfsk::fail(CFSK_FAILED, "picture resource serialization failed");
    return cfsk::resource(output, std::move(data));
}

void cfsk_picture_unref(cfsk_picture* value) {
    cfsk_picture_destroy(value);
}
