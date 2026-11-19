/* The window host owns acquired textures; the caller owns HWND and frame scheduling. */
#include "cfsk_dawn_host.h"
#include "dawn/dawn_proc.h"
#include "dawn/native/VulkanBackend.h"
#include <cstdio>
#include <cstring>
#include <string_view>
#if defined(_WIN32)
#include <windows.h>
#endif
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

struct Candidate {
    dawn::native::Adapter native;
    cfsk_dawn_adapter_info info;
};

using Uuid = std::array<uint8_t, VK_UUID_SIZE>;
struct DeviceLuid { Uuid uuid; uint64_t luid; };

#if defined(_WIN32)
std::vector<DeviceLuid> vulkan_luids() {
    struct Library {
        HMODULE value = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ~Library() { if (value) FreeLibrary(value); }
    } library;
    if (!library.value) return {};
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(library.value, "vkGetInstanceProcAddr"));
    if (!get) return {};
    auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    if (!create) return {};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo desc{};
    desc.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    desc.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&desc, nullptr, &instance) != VK_SUCCESS) return {};
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get(instance, "vkGetPhysicalDeviceProperties2"));
    std::vector<DeviceLuid> result;
    uint32_t count = 0;
    if (enumerate && properties && enumerate(instance, &count, nullptr) == VK_SUCCESS && count) {
        std::vector<VkPhysicalDevice> devices(count);
        if (enumerate(instance, &count, devices.data()) == VK_SUCCESS) {
            for (const auto device : devices) {
                VkPhysicalDeviceIDProperties id{};
                id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                VkPhysicalDeviceProperties2 info{};
                info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                info.pNext = &id;
                properties(device, &info);
                if (!id.deviceLUIDValid) continue;
                DeviceLuid entry{};
                std::memcpy(entry.uuid.data(), id.deviceUUID, entry.uuid.size());
                static_assert(sizeof(entry.luid) == VK_LUID_SIZE);
                std::memcpy(&entry.luid, id.deviceLUID, sizeof(entry.luid));
                result.push_back(entry);
            }
        }
    }
    destroy(instance, nullptr);
    return result;
}
#else
std::vector<DeviceLuid> vulkan_luids() { return {}; }
#endif

template <size_t N> void copy_string(char (&dest)[N], wgpu::StringView view) {
    if (!view.data) return;
    size_t size = view.length == WGPU_STRLEN ? std::strlen(view.data) : view.length;
    std::memcpy(dest, view.data, std::min(size, N - 1));
}

std::vector<Candidate> candidates(cfsk_dawn_host* host) {
    wgpu::RequestAdapterOptions request;
    request.backendType = wgpu::BackendType::Vulkan;
    request.powerPreference = wgpu::PowerPreference::HighPerformance;
    request.featureLevel = wgpu::FeatureLevel::Core;
    request.compatibleSurface = host->surface;
    auto toggles = skiatest::graphite::GetAdapterToggles();
    request.nextInChain = &toggles;
    std::vector<Candidate> result;
    auto luids = vulkan_luids();
    for (auto& adapter : host->native->instance->EnumerateAdapters(&request)) {
        wgpu::Adapter api_adapter = adapter.Get();
        dawn::native::vulkan::AdapterPropertiesVulkanUUIDs ids;
        wgpu::AdapterInfo info;
        info.nextInChain = &ids;
        if (api_adapter.GetInfo(&info) != wgpu::Status::Success ||
            info.backendType != wgpu::BackendType::Vulkan || info.adapterType == wgpu::AdapterType::CPU) continue;
        cfsk_dawn_adapter_info output{};
        if (info.adapterType == wgpu::AdapterType::DiscreteGPU) output.kind = 1;
        if (info.adapterType == wgpu::AdapterType::IntegratedGPU) output.kind = 2;
        output.vendor_id = info.vendorID;
        output.device_id = info.deviceID;
        copy_string(output.name, info.device);
        copy_string(output.driver, info.description);
        const auto luid = std::find_if(luids.begin(), luids.end(), [&](const DeviceLuid& item) {
            return item.uuid == ids.deviceUUID;
        });
        if (luid != luids.end()) {
            std::snprintf(output.id, sizeof(output.id), "0x%016llx", static_cast<unsigned long long>(luid->luid));
        } else {
            std::memcpy(output.id, "uuid:", 5);
            for (size_t n = 0; n < ids.deviceUUID.size(); ++n) {
                std::snprintf(output.id + 5 + n * 2, 3, "%02x", unsigned(ids.deviceUUID[n]));
            }
        }
        result.push_back({std::move(adapter), output});
    }
    std::stable_partition(result.begin(), result.end(), [](const Candidate& a) { return a.info.kind == 1; });
    return result;
}

cfsk_status make_device(cfsk_dawn_host* host, cfsk_bytes choice) {
    CFSK_REQUIRE(cfsk::valid(choice));
    std::string_view selected(choice.length ? reinterpret_cast<const char*>(choice.data) : "", choice.length);
    auto toggles = skiatest::graphite::GetAdapterToggles();
    auto& native = *host->native;
    for (auto& candidate : candidates(host)) {
        const auto& info = candidate.info;
        if (!(selected.empty() || selected == info.id ||
              (selected == "discrete" && info.kind == 1) ||
              (selected == "integrated" && info.kind == 2))) continue;
        auto& adapter = candidate.native;
        wgpu::Adapter api_adapter = adapter.Get();
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
        native.info = info;
        native.queue = native.device.GetQueue();
        return CFSK_OK;
    }
    return cfsk::unsupported(selected.empty() ? "no hardware Dawn Vulkan adapter present" :
                             "no hardware Dawn Vulkan adapter matches the requested choice");
}

template <typename T> bool contains(const T* values, size_t count, T value) {
    for (size_t i = 0; i < count; ++i) if (values[i] == value) return true;
    return false;
}

#if defined(_WIN32)
cfsk_status make_surface(cfsk_dawn_host* host, void* hinstance, void* hwnd) {
    CFSK_REQUIRE(hinstance && hwnd);
    wgpu::SurfaceSourceWindowsHWND source;
    source.hinstance = hinstance;
    source.hwnd = hwnd;
    wgpu::SurfaceDescriptor descriptor;
    descriptor.nextInChain = &source;
    wgpu::Instance instance(host->native->instance->Get());
    host->surface = instance.CreateSurface(&descriptor);
    if (!host->surface) return cfsk::fail(CFSK_FAILED, "Dawn Win32 surface creation failed");
    host->hinstance = hinstance;
    host->hwnd = hwnd;
    return CFSK_OK;
}
#endif

cfsk_status surface_policy(cfsk_dawn_host* host) {
    wgpu::SurfaceCapabilities caps;
    if (host->surface.GetCapabilities(host->native->adapter, &caps) != wgpu::Status::Success)
        return cfsk::unsupported("Dawn adapter is incompatible with the window");
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

cfsk_status cfsk_dawn_adapters(cfsk_dawn_adapter_info* output, size_t capacity, size_t* required) {
    CFSK_BEGIN; CFSK_REQUIRE(required && (output || !capacity));
    cfsk_dawn_host host;
    *required = 0;
    if (auto status = make_instance(&host); status != CFSK_OK) return status;
    auto available = candidates(&host);
    *required = available.size();
    if (!output) return CFSK_OK;
    if (capacity < available.size()) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "Dawn adapter array is too small");
    for (size_t i = 0; i < available.size(); ++i) output[i] = available[i].info;
    return CFSK_OK;
}

cfsk_status cfsk_dawn_host_adapter(cfsk_dawn_host* host, cfsk_dawn_adapter_info* output) {
    CFSK_BEGIN; CFSK_REQUIRE(output);
    if (auto status = cfsk::dawn_host_ready(host); status != CFSK_OK) return status;
    *output = host->native->info;
    return CFSK_OK;
}

cfsk_status cfsk_dawn_host_poll(cfsk_dawn_host* host) {
    CFSK_BEGIN; return cfsk::dawn_host_ready(host);
}

cfsk_status cfsk_dawn_host_adopt(cfsk_dawn_host* host) {
    CFSK_BEGIN; CFSK_REQUIRE(host && !host->surface && host->native.use_count() == 1);
    host->owner = std::this_thread::get_id();
    return CFSK_OK;
}

cfsk_status cfsk_dawn_host_make(cfsk_dawn_host** output) {
    return cfsk_dawn_host_make_selected({}, output);
}

cfsk_status cfsk_dawn_host_make_selected(cfsk_bytes choice, cfsk_dawn_host** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output);
    std::unique_ptr<cfsk_dawn_host> host(new (std::nothrow) cfsk_dawn_host);
    if (!host) return cfsk::fail(CFSK_OUT_OF_MEMORY, "Dawn host allocation failed");
    if (auto status = make_instance(host.get()); status != CFSK_OK) return status;
    if (auto status = make_device(host.get(), choice); status != CFSK_OK) return status;
    *output = host.release();
    return CFSK_OK;
}

cfsk_status cfsk_dawn_host_make_win32(void* hinstance, void* hwnd, cfsk_dawn_host** output) {
    return cfsk_dawn_host_make_win32_selected(hinstance, hwnd, {}, output);
}

cfsk_status cfsk_dawn_host_make_win32_selected(void* hinstance, void* hwnd, cfsk_bytes choice,
                                              cfsk_dawn_host** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output);
#if defined(_WIN32)
    std::unique_ptr<cfsk_dawn_host> host(new (std::nothrow) cfsk_dawn_host);
    if (!host) return cfsk::fail(CFSK_OUT_OF_MEMORY, "Dawn host allocation failed");
    if (auto status = make_instance(host.get()); status != CFSK_OK) return status;
    if (auto status = make_surface(host.get(), hinstance, hwnd); status != CFSK_OK) return status;
    if (auto status = make_device(host.get(), choice); status != CFSK_OK) return status;
    if (auto status = surface_policy(host.get()); status != CFSK_OK) return status;
    *output = host.release();
    return CFSK_OK;
#else
    (void)hinstance; (void)hwnd; (void)choice;
    return cfsk::unsupported("Dawn window host is Win32 only");
#endif
}

cfsk_status cfsk_dawn_host_bind_win32(cfsk_dawn_host* device_host, void* hinstance, void* hwnd,
                                     cfsk_dawn_host** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output);
#if defined(_WIN32)
    if (auto status = cfsk::dawn_host_ready(device_host); status != CFSK_OK) return status;
    std::unique_ptr<cfsk_dawn_host> host(new (std::nothrow) cfsk_dawn_host);
    if (!host) return cfsk::fail(CFSK_OUT_OF_MEMORY, "Dawn surface owner allocation failed");
    host->native = device_host->native;
    if (auto status = make_surface(host.get(), hinstance, hwnd); status != CFSK_OK) return status;
    if (auto status = surface_policy(host.get()); status != CFSK_OK) return status;
    *output = host.release();
    return CFSK_OK;
#else
    (void)device_host; (void)hinstance; (void)hwnd;
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
#if defined(_WIN32)
    if (host->surface_lost) {
        host->surface = nullptr;
        // Dawn defers Vulkan swapchain destruction until the next queue serial completes.
        auto& queue = host->native->queue;
        queue.Submit(0, nullptr);
        auto done = queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
        wgpu::Instance instance(host->native->instance->Get());
        if (instance.WaitAny(done, 5'000'000'000) != wgpu::WaitStatus::Success) {
            return cfsk::fail(CFSK_FAILED, "could not retire lost Dawn swapchain");
        }
        // Tick best-effort deletions after the user-visible fence has completed.
        queue.Submit(0, nullptr);
        if (auto status = cfsk::dawn_host_ready(host); status != CFSK_OK) return status;
        if (auto status = make_surface(host, host->hinstance, host->hwnd); status != CFSK_OK) return status;
        if (auto status = surface_policy(host); status != CFSK_OK) return status;
        host->surface_lost = false;
    }
#endif
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
    if (host->suboptimal) return cfsk::fail(CFSK_PENDING, "Dawn surface suboptimal; reconfigure");
    wgpu::SurfaceTexture current;
    host->surface.GetCurrentTexture(&current);
    using Status = wgpu::SurfaceGetCurrentTextureStatus;
    switch (current.status) {
        case Status::SuccessOptimal: case Status::SuccessSuboptimal: break;
        case Status::Outdated: return cfsk::fail(CFSK_PENDING, "Dawn surface outdated; reconfigure");
        case Status::Timeout: return cfsk::fail(CFSK_PENDING, "Dawn surface acquisition timeout");
        case Status::Lost:
            host->surface_lost = true;
            return cfsk::fail(CFSK_PENDING, "Dawn surface lost; recreate surface");
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
