/* The window host owns acquired textures; the caller owns HWND and frame scheduling. */
#include "cfsk_dawn_host.h"
#include "dawn/dawn_proc.h"
#include "tools/graphite/dawn/GraphiteDawnToggles.h"
#include <algorithm>
#include <mutex>

namespace {
std::once_flag procs_once;

cfsk_status make_instance(cfsk_dawn_host* host) {
    std::call_once(procs_once, [] { dawnProcSetProcs(&dawn::native::GetProcs()); });
    host->native = std::make_shared<cfsk::DawnDevice>();
    wgpu::InstanceDescriptor descriptor;
    static constexpr auto timed_wait = wgpu::InstanceFeatureName::TimedWaitAny;
    descriptor.requiredFeatureCount = 1;
    descriptor.requiredFeatures = &timed_wait;
    auto toggles = skiatest::graphite::GetInstanceToggles();
    descriptor.nextInChain = &toggles;
    host->native->instance = std::make_unique<dawn::native::Instance>(&descriptor);
    if (!host->native->instance->Get()) return cfsk::fail(CFSK_FAILED, "Dawn instance creation failed");
    return CFSK_OK;
}

cfsk_status make_device(cfsk_dawn_host* host) {
    wgpu::RequestAdapterOptions request;
    request.backendType = wgpu::BackendType::Vulkan;
    request.powerPreference = wgpu::PowerPreference::HighPerformance;
    request.featureLevel = wgpu::FeatureLevel::Core;
    request.compatibleSurface = host->surface;
    auto toggles = skiatest::graphite::GetAdapterToggles();
    request.nextInChain = &toggles;
    auto& native = *host->native;
    for (auto& adapter : native.instance->EnumerateAdapters(&request)) {
        wgpu::Adapter api_adapter = adapter.Get();
        wgpu::AdapterInfo info;
        api_adapter.GetInfo(&info);
        if (info.backendType != wgpu::BackendType::Vulkan || info.adapterType == wgpu::AdapterType::CPU) continue;
        std::vector<wgpu::FeatureName> features;
        skiatest::graphite::AddPreferredFeatures(api_adapter, features);
        wgpu::Limits limits;
        if (api_adapter.GetLimits(&limits) != wgpu::Status::Success)
            return cfsk::fail(CFSK_FAILED, "Dawn adapter limits unavailable");
        wgpu::DeviceDescriptor config;
        config.requiredFeatureCount = features.size();
        config.requiredFeatures = features.data();
        config.requiredLimits = &limits;
        config.nextInChain = &toggles;
        auto errors = native.errors;
        config.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
            [errors](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                if (reason == wgpu::DeviceLostReason::Destroyed ||
                    reason == wgpu::DeviceLostReason::CallbackCancelled) return;
                errors->lost.store(true);
                SkDebugf("CFSK_DAWN_DEVICE_LOST %.*s\n", int(message.length), message.data);
            });
        config.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message, cfsk::DawnErrors* errors) {
                errors->failed.store(true);
                SkDebugf("CFSK_DAWN_ERROR %.*s\n", int(message.length), message.data);
            }, errors.get());
        native.device = wgpu::Device::Acquire(adapter.CreateDevice(&config));
        if (!native.device) return cfsk::fail(CFSK_FAILED, "hardware Dawn Vulkan device creation failed");
        native.adapter = std::move(api_adapter);
        native.queue = native.device.GetQueue();
        return CFSK_OK;
    }
    return cfsk::unsupported("no hardware Dawn Vulkan adapter present");
}

template <typename T> bool contains(const T* values, size_t count, T value) {
    for (size_t i = 0; i < count; ++i) if (values[i] == value) return true;
    return false;
}

cfsk_status surface_policy(cfsk_dawn_host* host) {
    wgpu::SurfaceCapabilities caps;
    if (host->surface.GetCapabilities(host->native->adapter, &caps) != wgpu::Status::Success)
        return cfsk::fail(CFSK_FAILED, "Dawn surface capabilities unavailable");
    auto& config = host->config;
    config.device = host->native->device;
    config.format = wgpu::TextureFormat::Undefined;
    for (auto format : {wgpu::TextureFormat::BGRA8Unorm, wgpu::TextureFormat::RGBA8Unorm}) {
        if (contains(caps.formats, caps.formatCount, format)) { config.format = format; break; }
    }
    config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                   wgpu::TextureUsage::TextureBinding;
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    config.presentMode = wgpu::PresentMode::Fifo;
    if (config.format == wgpu::TextureFormat::Undefined || (caps.usages & config.usage) != config.usage ||
        !contains(caps.alphaModes, caps.alphaModeCount, config.alphaMode) ||
        !contains(caps.presentModes, caps.presentModeCount, config.presentMode))
        return cfsk::unsupported("Dawn surface needs 8-bit unorm, Opaque, Fifo and readback usages");
    return CFSK_OK;
}
}

cfsk_status cfsk::dawn_host_ready(cfsk_dawn_host* host) {
    CFSK_REQUIRE(host);
    if (host->owner != std::this_thread::get_id()) return fail(CFSK_WRONG_THREAD, "Dawn host owner mismatch");
    dawn::native::InstanceProcessEvents(host->native->instance->Get());
    if (host->native->errors->lost.load()) return fail(CFSK_DEVICE_LOST, "Dawn host device lost");
    if (host->native->errors->failed.load()) return fail(CFSK_FAILED, "Dawn host uncaptured error");
    return CFSK_OK;
}

cfsk_context_options cfsk::dawn_context_options(cfsk_dawn_host* host) {
    cfsk_context_options options{};
    options.dawn_instance = host->native->instance->Get();
    options.dawn_device = host->native->device.Get();
    options.dawn_queue = host->native->queue.Get();
    options.internal_samples = 4;
    options.gpu_budget_bytes = 256 * 1024 * 1024;
    return options;
}

cfsk_status cfsk_dawn_host_make(cfsk_dawn_host** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output);
    std::unique_ptr<cfsk_dawn_host> host(new (std::nothrow) cfsk_dawn_host);
    if (!host) return cfsk::fail(CFSK_OUT_OF_MEMORY, "Dawn host allocation failed");
    if (auto status = make_instance(host.get()); status != CFSK_OK) return status;
    if (auto status = make_device(host.get()); status != CFSK_OK) return status;
    *output = host.release();
    return CFSK_OK;
}

cfsk_status cfsk_dawn_host_make_win32(void* hinstance, void* hwnd, cfsk_dawn_host** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output);
#if defined(_WIN32)
    CFSK_REQUIRE(hinstance && hwnd);
    std::unique_ptr<cfsk_dawn_host> host(new (std::nothrow) cfsk_dawn_host);
    if (!host) return cfsk::fail(CFSK_OUT_OF_MEMORY, "Dawn host allocation failed");
    if (auto status = make_instance(host.get()); status != CFSK_OK) return status;
    wgpu::SurfaceSourceWindowsHWND source;
    source.hinstance = hinstance;
    source.hwnd = hwnd;
    wgpu::SurfaceDescriptor descriptor;
    descriptor.nextInChain = &source;
    wgpu::Instance instance(host->native->instance->Get());
    host->surface = instance.CreateSurface(&descriptor);
    if (!host->surface) return cfsk::fail(CFSK_FAILED, "Dawn Win32 surface creation failed");
    if (auto status = make_device(host.get()); status != CFSK_OK) return status;
    if (auto status = surface_policy(host.get()); status != CFSK_OK) return status;
    *output = host.release();
    return CFSK_OK;
#else
    (void)hinstance; (void)hwnd;
    return cfsk::unsupported("Dawn window host is Win32 only");
#endif
}

cfsk_status cfsk_dawn_host_configure(cfsk_dawn_host* host, uint32_t width, uint32_t height) {
    CFSK_BEGIN;
    if (auto status = cfsk::dawn_host_ready(host); status != CFSK_OK) return status;
    if (!host->surface) return cfsk::unsupported("headless host has no surface");
    CFSK_REQUIRE(!host->texture);
    if (!width || !height) { host->suspended = true; return CFSK_PENDING; }
    wgpu::Limits limits;
    if (host->native->device.GetLimits(&limits) != wgpu::Status::Success)
        return cfsk::fail(CFSK_FAILED, "Dawn device limits unavailable");
    CFSK_REQUIRE(width <= limits.maxTextureDimension2D && height <= limits.maxTextureDimension2D);
    host->suspended = false;
    host->config.width = width;
    host->config.height = height;
    host->surface.Configure(&host->config);
    host->suboptimal = false;
    return cfsk::dawn_host_ready(host);
}

cfsk_status cfsk_dawn_host_acquire(cfsk_dawn_host* host, void** texture, cfsk_image_info* info) {
    CFSK_BEGIN; CFSK_OUTPUT(texture); CFSK_REQUIRE(info); *info = {};
    if (auto status = cfsk::dawn_host_ready(host); status != CFSK_OK) return status;
    if (!host->surface) return cfsk::unsupported("headless host has no surface");
    if (host->suspended) return CFSK_PENDING;
    CFSK_REQUIRE(!host->texture);
    if (host->suboptimal) host->surface.Configure(&host->config);
    wgpu::SurfaceTexture current;
    host->surface.GetCurrentTexture(&current);
    using Status = wgpu::SurfaceGetCurrentTextureStatus;
    switch (current.status) {
        case Status::SuccessOptimal: case Status::SuccessSuboptimal: break;
        case Status::Outdated: return cfsk::fail(CFSK_PENDING, "Dawn surface outdated; reconfigure");
        case Status::Timeout: return cfsk::fail(CFSK_PENDING, "Dawn surface acquisition timeout");
        case Status::Lost: return cfsk::fail(CFSK_DEVICE_LOST, "Dawn surface lost; recreate host");
        default: return cfsk::fail(CFSK_FAILED, "Dawn surface acquisition failed");
    }
    if (!current.texture) return cfsk::fail(CFSK_FAILED, "Dawn acquired null texture");
    host->suboptimal = current.status == Status::SuccessSuboptimal;
    host->texture = std::move(current.texture);
    *texture = host->texture.Get();
    auto color = host->config.format == wgpu::TextureFormat::BGRA8Unorm ?
                 CFSK_COLOR_BGRA_8888 : CFSK_COLOR_RGBA_8888;
    *info = {int32_t(host->config.width), int32_t(host->config.height), uint32_t(color),
        CFSK_ALPHA_PREMUL, nullptr};
    return CFSK_OK;
}

cfsk_status cfsk_dawn_host_present(cfsk_dawn_host* host) {
    CFSK_BEGIN;
    if (auto status = cfsk::dawn_host_ready(host); status != CFSK_OK) return status;
    if (!host->surface) return cfsk::unsupported("headless host has no surface");
    if (host->suspended) return CFSK_PENDING;
    CFSK_REQUIRE(host->texture);
    auto result = host->surface.Present();
    host->texture = nullptr;
    if (result != wgpu::Status::Success) return cfsk::fail(CFSK_FAILED, "Dawn surface present failed");
    return cfsk::dawn_host_ready(host);
}

cfsk_status cfsk_dawn_host_release(cfsk_dawn_host** host) {
    CFSK_BEGIN; CFSK_REQUIRE(host);
    if (!*host) return CFSK_OK;
    if ((*host)->owner != std::this_thread::get_id())
        return cfsk::fail(CFSK_WRONG_THREAD, "Dawn host release owner mismatch");
    (*host)->texture = nullptr;
    if ((*host)->surface && !(*host)->suspended) (*host)->surface.Unconfigure();
    delete *host;
    *host = nullptr;
    return CFSK_OK;
}
