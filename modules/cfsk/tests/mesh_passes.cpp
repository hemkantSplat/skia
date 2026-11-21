/* Standalone Dawn Graphite check. The first argument names the readback directory. */
#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkMesh.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSurface.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "src/gpu/graphite/ContextOptionsPriv.h"
#include "src/gpu/graphite/RendererProvider.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace graphite = skgpu::graphite;

static const DawnProcTable& kDawn = dawn::native::GetProcs();
static std::unordered_map<WGPUTextureView, uint32_t> gSamplesByView;
static std::vector<uint32_t>* gPassSamples = nullptr;

static WGPUTextureView create_view(WGPUTexture texture, const WGPUTextureViewDescriptor* desc) {
    auto view = kDawn.textureCreateView(texture, desc);
    gSamplesByView[view] = kDawn.textureGetSampleCount(texture);
    return view;
}

static WGPURenderPassEncoder begin_pass(WGPUCommandEncoder encoder,
                                       const WGPURenderPassDescriptor* desc) {
    if (gPassSamples && desc->colorAttachmentCount) {
        gPassSamples->push_back(gSamplesByView[desc->colorAttachments[0].view]);
    }
    return kDawn.commandEncoderBeginRenderPass(encoder, desc);
}

static SkMesh make_mesh() {
    using Attribute = SkMeshSpecification::Attribute;
    const Attribute attributes[] = {
        {Attribute::Type::kFloat2, 0, SkString("pos")},
        {Attribute::Type::kFloat2, 0, SkString("shift"), Attribute::Rate::kInstance},
    };
    auto spec = SkMeshSpecification::Make(
            attributes, sizeof(SkPoint), {},
            SkString("Varyings main(const Attributes a) {"
                     "    Varyings v; v.position = a.pos + a.shift; return v; }"),
            SkString("float2 main(const Varyings v) { return v.position; }"),
            SkColorSpace::MakeSRGB(), kPremul_SkAlphaType, sizeof(SkPoint));
    if (!spec.specification) {
        std::fprintf(stderr, "%s\n", spec.error.c_str());
        return {};
    }
    const SkPoint vertices[] = {{16, 16}, {112, 16}, {16, 112}, {112, 112}};
    const SkPoint shift{0, 0};
    const SkMesh::Instances instances{SkMeshes::MakeVertexBuffer(&shift, sizeof(shift)), 0, 1};
    auto mesh = SkMesh::Make(std::move(spec.specification), SkMesh::Mode::kTriangleStrip,
                             SkMeshes::MakeVertexBuffer(vertices, sizeof(vertices)),
                             std::size(vertices), 0, nullptr, {}, SkRect::MakeLTRB(16, 16, 112, 112),
                             &instances);
    if (!mesh.mesh.isValid()) {
        std::fprintf(stderr, "%s\n", mesh.error.c_str());
    }
    return std::move(mesh.mesh);
}

static bool run(graphite::Context* context, bool meshFirst, const std::filesystem::path& out) {
    auto mesh = make_mesh();
    if (!mesh.isValid()) {
        return false;
    }
    auto recorder = context->makeRecorder();
    const auto info = SkImageInfo::Make(128, 128, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    auto surface = SkSurfaces::RenderTarget(recorder.get(), info);
    if (!surface) {
        return false;
    }
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorBLACK);
    SkPaint meshPaint;
    meshPaint.setColor(SK_ColorRED);
    SkPaint pathPaint;
    pathPaint.setColor(SK_ColorBLUE);
    pathPaint.setAntiAlias(true);
    const auto path = SkPathBuilder().moveTo(8.7f, 30.3f).lineTo(110.2f, 23.5f)
                             .lineTo(122.6f, 94.6f).lineTo(79.1f, 56.5f)
                             .lineTo(45.4f, 115.7f).lineTo(27.3f, 73.1f).close().detach();
    if (meshFirst) {
        canvas->drawMesh(mesh, nullptr, meshPaint);
    }
    canvas->drawPath(path, pathPaint);
    if (!meshFirst) {
        canvas->drawMesh(mesh, nullptr, meshPaint);
    }
    auto recording = recorder->snap();
    if (!recording) {
        return false;
    }
    graphite::InsertRecordingInfo insert;
    insert.fRecording = recording.get();
    std::vector<uint32_t> passSamples;
    gPassSamples = &passSamples;
    const bool inserted = context->insertRecording(insert);
    const bool submitted = context->submit(graphite::SyncToCpu::kYes);
    gPassSamples = nullptr;
    if (!inserted || !submitted) {
        return false;
    }

    std::unique_ptr<const SkImage::AsyncReadResult> pixels;
    context->asyncRescaleAndReadPixels(surface.get(), info, SkIRect::MakeWH(128, 128),
                                      SkImage::RescaleGamma::kSrc, SkImage::RescaleMode::kNearest,
        [](void* state, std::unique_ptr<const SkImage::AsyncReadResult> result) {
            *static_cast<decltype(pixels)*>(state) = std::move(result);
        }, &pixels);
    if (!context->submit(graphite::SyncToCpu::kYes) || !pixels) {
        return false;
    }
    const char* name = meshFirst ? "mesh-then-path" : "path-then-mesh";
    std::ofstream output(out / (std::string(name) + ".rgba"), std::ios::binary);
    for (int y = 0; y < info.height(); ++y) {
        const auto* row = static_cast<const char*>(pixels->data(0)) + y * pixels->rowBytes(0);
        output.write(row, info.minRowBytes());
    }
    std::printf("%s: %zu pass(es), samples:", name, passSamples.size());
    for (auto count : passSamples) {
        std::printf(" %u", count);
    }
    std::puts("");
    // The request omits Dawn load/resolve features, so Skia emits a single-sample resolve blit.
    return output.good() && passSamples == (meshFirst ? std::vector<uint32_t>{1, 4, 1}
                                                      : std::vector<uint32_t>{4, 1, 1});
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const auto out = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(out);
    DawnProcTable wrapped = kDawn;
    wrapped.textureCreateView = create_view;
    wrapped.commandEncoderBeginRenderPass = begin_pass;
    dawnProcSetProcs(&wrapped);

    wgpu::InstanceDescriptor instanceOptions;
    constexpr auto timedWait = wgpu::InstanceFeatureName::TimedWaitAny;
    instanceOptions.requiredFeatureCount = 1;
    instanceOptions.requiredFeatures = &timedWait;
    constexpr const char* unsafeApis = "allow_unsafe_apis";
    wgpu::DawnTogglesDescriptor toggles;
    toggles.enabledToggleCount = 1;
    toggles.enabledToggles = &unsafeApis;
    instanceOptions.nextInChain = &toggles;
    dawn::native::Instance native(&instanceOptions);
    wgpu::RequestAdapterOptions adapterOptions;
    adapterOptions.backendType = wgpu::BackendType::Vulkan;
    for (auto& adapter : native.EnumerateAdapters(&adapterOptions)) {
        wgpu::Device device = wgpu::Device::Acquire(adapter.CreateDevice());
        if (!device) {
            continue;
        }
        graphite::DawnBackendContext backend;
        backend.fInstance = wgpu::Instance(native.Get());
        backend.fDevice = device;
        backend.fQueue = device.GetQueue();
        graphite::ContextOptions options;
        graphite::ContextOptionsPriv tuning;
        tuning.fPathRendererStrategy = graphite::PathRendererStrategy::kTessellation;
        options.fOptionsPriv = &tuning;
        auto context = graphite::ContextFactory::MakeDawn(backend, options);
        if (!context) {
            continue;
        }
        const bool forward = run(context.get(), true, out);
        const bool reverse = run(context.get(), false, out);
        return !(forward && reverse);
    }
    std::fprintf(stderr, "No Vulkan adapter available for the Graphite test\n");
    return 2;
}
