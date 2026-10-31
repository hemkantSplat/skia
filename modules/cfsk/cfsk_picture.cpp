/* A picture retains decoded resources; serialization embeds CPU pixels and fonts. */
#include "cfsk_internal.h"
#include "include/core/SkSerialProcs.h"
#include "include/encode/SkPngEncoder.h"

namespace {
sk_sp<SkTypeface> deserialize_typeface(SkStream& stream, void* context) {
    auto typeface = SkTypeface::MakeDeserialize(&stream, nullptr);
    if (!typeface) *static_cast<bool*>(context) = false;
    return typeface;
}
sk_sp<SkImage> deserialize_image(sk_sp<SkData> data, std::optional<SkAlphaType> alpha_type, void* context) {
    auto encoded = SkImages::DeferredFromEncodedData(std::move(data), alpha_type);
    auto image = encoded ? encoded->makeRasterImage() : nullptr;
    if (!image) *static_cast<bool*>(context) = false;
    return image;
}
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
    bool complete = true;
    SkDeserialProcs procs;
    procs.fImageDataProc = deserialize_image;
    procs.fImageCtx = &complete;
    procs.fTypefaceStreamProc = deserialize_typeface;
    procs.fTypefaceCtx = &complete;
    auto picture = SkPicture::MakeFromData(bytes.data, bytes.length, &procs);
    if (!picture || !complete) return cfsk::fail(CFSK_FAILED, "invalid SkPicture data or resource decode failure (image/typeface)");
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
