/* Frozen picture archives share immutable image/font objects across recordings. */
#include "cfsk_internal.h"
#include "include/core/SkSerialProcs.h"
#include "include/core/SkFontMgr.h"
#include <cstring>
#include <map>

struct cfsk_picture_resources {
    std::string directory;
    std::thread::id owner = std::this_thread::get_id();
    std::map<std::string, sk_sp<SkImage>> images;
    std::map<std::string, sk_sp<SkTypeface>> typefaces;
    bool complete = true;
};
namespace {
std::string resource_id(const void* data, size_t size, const char* prefix) {
    if (!data || size != 39 || std::memcmp(data, prefix, 7)) return {};
    std::string id(static_cast<const char*>(data) + 7, 32);
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    return id;
}
sk_sp<SkImage> decode_image(sk_sp<SkData> data) {
    if (!data) return nullptr;
    if (data->size() < 32 || std::memcmp(data->data(), "CFRAW1\0\0", 8)) {
        return SkImages::DeferredFromEncodedData(data);
    }
    uint32_t header[6];
    std::memcpy(header, static_cast<const char*>(data->data()) + 8, sizeof(header));
    size_t offset = size_t(32) + header[5];
    if (offset > data->size() || uint64_t(header[4]) * header[1] != data->size() - offset) return nullptr;
    auto space = header[5] ? SkColorSpace::Deserialize(static_cast<const char*>(data->data()) + 32, header[5]) : nullptr;
    if (header[5] && !space) return nullptr;
    auto info = SkImageInfo::Make(header[0], header[1], static_cast<SkColorType>(header[2]),
                                 static_cast<SkAlphaType>(header[3]), std::move(space));
    return SkImages::RasterFromData(info, SkData::MakeSubset(data.get(), offset, data->size() - offset), header[4]);
}
sk_sp<SkImage> image_proc(sk_sp<SkData> data, std::optional<SkAlphaType> alpha, void* opaque) {
    auto& context = *static_cast<cfsk_picture_resources*>(opaque);
    auto id = data ? resource_id(data->data(), data->size(), "CFIMG1:") : std::string();
    sk_sp<SkImage> image;
    if (id.empty()) {
        image = data ? SkImages::DeferredFromEncodedData(data, alpha) : nullptr;
    } else {
        auto found = context.images.find(id);
        if (found != context.images.end()) return found->second;
        image = decode_image(SkData::MakeFromFileName((context.directory + "/" + id + ".bin").c_str()));
        if (image) context.images.emplace(id, image);
    }
    if (!image) context.complete = false;
    return image;
}
sk_sp<SkTypeface> typeface_proc(SkStream& stream, void* opaque) {
    auto& context = *static_cast<cfsk_picture_resources*>(opaque);
    char token[39];
    auto id = stream.peek(token, sizeof(token)) == sizeof(token) ?
        resource_id(token, sizeof(token), "CFONT1:") : std::string();
    sk_sp<SkTypeface> face;
    if (id.empty()) {
        face = SkTypeface::MakeDeserialize(&stream, nullptr);
    } else {
        stream.read(token, sizeof(token));
        auto found = context.typefaces.find(id);
        if (found != context.typefaces.end()) return found->second;
        auto bytes = SkData::MakeFromFileName((context.directory + "/" + id + ".bin").c_str());
        if (bytes) {
            SkMemoryStream font_stream(bytes);
            face = SkTypeface::MakeDeserialize(&font_stream, nullptr);
        }
        if (face) context.typefaces.emplace(id, face);
    }
    if (!face) context.complete = false;
    return face;
}
}
cfsk_status cfsk_picture_resources_make(cfsk_bytes directory, cfsk_picture_resources** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); CFSK_REQUIRE(cfsk::valid(directory) && directory.length);
    std::string path(reinterpret_cast<const char*>(directory.data), directory.length);
    CFSK_REQUIRE(path.find('\0') == std::string::npos);
    auto resources = new (std::nothrow) cfsk_picture_resources;
    if (!resources) return cfsk::fail(CFSK_OUT_OF_MEMORY, "picture resource allocation failed");
    resources->directory = std::move(path);
    *output = resources;
    return CFSK_OK;
}
cfsk_status cfsk_picture_deserialize_shared(cfsk_picture_resources* resources, cfsk_bytes bytes,
        cfsk_picture** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); CFSK_REQUIRE(resources && cfsk::valid(bytes));
    if (resources->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "picture resource owner mismatch");
    resources->complete = true;
    SkDeserialProcs procs;
    procs.fImageDataProc = image_proc;
    procs.fImageCtx = resources;
    procs.fTypefaceStreamProc = typeface_proc;
    procs.fTypefaceCtx = resources;
    auto picture = SkPicture::MakeFromData(bytes.data, bytes.length, &procs);
    if (!picture || !resources->complete) return cfsk::fail(CFSK_FAILED, "shared picture resource decode failed");
    return cfsk::resource(output, std::move(picture));
}
void cfsk_picture_resources_destroy(cfsk_picture_resources* value) { delete value; }
