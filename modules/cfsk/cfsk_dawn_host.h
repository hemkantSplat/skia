/* Native Dawn ownership and the single device policy shared by both context routes. */
#ifndef CFSK_DAWN_HOST_H
#define CFSK_DAWN_HOST_H
#include "cfsk_internal.h"
#include "dawn/native/DawnNative.h"
#include <atomic>
#include <memory>

namespace cfsk {
struct DawnErrors { std::atomic<bool> lost{false}, failed{false}; };
struct DawnDevice {
    std::shared_ptr<DawnErrors> errors = std::make_shared<DawnErrors>();
    std::unique_ptr<dawn::native::Instance> instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
};
}
struct cfsk_dawn_host {
    std::shared_ptr<cfsk::DawnDevice> native;
    std::thread::id owner = std::this_thread::get_id();
    wgpu::Surface surface;
    wgpu::Texture texture;
    wgpu::SurfaceConfiguration config;
    bool suspended = true, suboptimal = false;
};
namespace cfsk {
cfsk_status dawn_host_ready(cfsk_dawn_host* host);
cfsk_context_options dawn_context_options(cfsk_dawn_host* host);
}
#endif
