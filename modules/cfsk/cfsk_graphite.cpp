/* Tickets share callback storage, so dropping a ticket never frees a pending callback. */
#include "cfsk_internal.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/PrecompileContext.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "include/gpu/graphite/PersistentPipelineStorage.h"
#include "cfsk_dawn_host.h"
#include "include/core/SkTiledImageUtils.h"
#include "include/gpu/graphite/ImageProvider.h"
#include "src/core/SkLRUCache.h"
#include <cstring>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "src/core/SkScopeExit.h"
#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace graphite = skgpu::graphite;
constexpr size_t kPipelineKeyBudget = 64 * 1024 * 1024;
class PipelineStorage final : public graphite::PersistentPipelineStorage {
public:
    sk_sp<SkData> data;
    sk_sp<SkData> load() override { return data; }
    void store(const SkData& value) override { data = SkData::MakeWithCopy(value.data(), value.size()); }
};
namespace {
// The archive disables C++ exceptions, so thread creation must return failure explicitly.
class PrecompileThread {
public:
    template <typename Work>
    bool start(Work* work) {
#if defined(_WIN32)
        fHandle = reinterpret_cast<HANDLE>(_beginthreadex(
                nullptr, 0, &Run<Work>, work, 0, nullptr));
        return fHandle != nullptr;
#else
        fStarted = pthread_create(&fHandle, nullptr, &Run<Work>, work) == 0;
        return fStarted;
#endif
    }

    void join() {
#if defined(_WIN32)
        if (fHandle) {
            WaitForSingleObject(fHandle, INFINITE);
            CloseHandle(fHandle);
            fHandle = nullptr;
        }
#else
        if (fStarted) {
            pthread_join(fHandle, nullptr);
            fStarted = false;
        }
#endif
    }

private:
#if defined(_WIN32)
    using Exit = unsigned;
    HANDLE fHandle = nullptr;
#else
    using Exit = void*;
    pthread_t fHandle{};
    bool fStarted = false;
#endif

    template <typename Work>
#if defined(_WIN32)
    static Exit __stdcall Run(void* data) {
#else
    static Exit Run(void* data) {
#endif
        std::unique_ptr<Work> work(static_cast<Work*>(data));
        (*work)();
        return Exit{};
    }
};
}  // namespace

struct cfsk_context {
    std::shared_ptr<cfsk::DawnDevice> native_host;
    PipelineStorage storage;
    uint32_t inline_count = 0, precompiled_count = 0, unkeyable_count = 0;
    std::mutex pipeline_mutex;
    std::deque<sk_sp<SkData>> pipelines;
    size_t queued_key_bytes = 0, indexed_key_bytes = 0;
    std::unordered_multimap<uint32_t, sk_sp<SkData>> keys_by_hash;
    std::unordered_set<uint32_t> captured_hashes;
    std::vector<sk_sp<SkData>> captured_keys;
    std::atomic<bool> capturing{false};
    std::atomic<bool> precompile_busy{false}, precompile_stop{false};
    PrecompileThread precompile_worker;
    std::thread::id owner = std::this_thread::get_id();
    graphite::DawnBackendContext dawn;
    std::unique_ptr<graphite::Context> value;
    ~cfsk_context() {
        precompile_stop.store(true);
        precompile_worker.join();
        if (value) value->submit(skgpu::graphite::SyncToCpu::kYes);
        // Destroy Graphite before the pipeline callback storage and Dawn references.
        value.reset();
    }
};
struct cfsk_recorder {
    std::unique_ptr<graphite::Recorder> value;
    cfsk_context* context;
    std::thread::id owner;
};
struct cfsk_recording { std::unique_ptr<graphite::Recording> value; cfsk_context* context; };
struct Completion {
    std::atomic<bool> complete{false};
    cfsk_status status = CFSK_PENDING;
    cfsk_completion result{};
};
struct cfsk_submission { std::shared_ptr<Completion> value; cfsk_context* context; };
struct Readback {
    std::atomic<bool> complete{false};
    std::unique_ptr<const SkImage::AsyncReadResult> value;
};
struct cfsk_readback {
    std::shared_ptr<Readback> state;
    cfsk_context* context;
    SkImageInfo info;
};
namespace {
cfsk_status context_ready(cfsk_context* context) {
    if (!context) return cfsk::fail(CFSK_INVALID_ARGUMENT, "null context");
    if (context->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "Context submit owner mismatch");
    if (context->value->isDeviceLost()) return cfsk::fail(CFSK_DEVICE_LOST, "Dawn device lost");
    return CFSK_OK;
}
cfsk_status recorder_ready(cfsk_recorder* recorder) {
    if (!recorder) return cfsk::fail(CFSK_INVALID_ARGUMENT, "null recorder");
    if (recorder->owner == std::thread::id{}) recorder->owner = std::this_thread::get_id();
    if (recorder->owner != std::this_thread::get_id()) return cfsk::fail(CFSK_WRONG_THREAD, "Recorder record owner mismatch");
    return CFSK_OK;
}
cfsk_status progress(cfsk_context* context) {
    auto status = context_ready(context);
    if (status != CFSK_OK) return status;
    context->dawn.fInstance.ProcessEvents();
    context->value->checkAsyncWorkCompletion();
    return context_ready(context);
}
void finished(void* opaque, skgpu::CallbackResult result, const skgpu::GpuStats& stats) {
    std::unique_ptr<std::shared_ptr<Completion>> state(static_cast<std::shared_ptr<Completion>*>(opaque));
    (*state)->status = result == skgpu::CallbackResult::kSuccess ? CFSK_OK : CFSK_FAILED;
    (*state)->result.complete = 1;
    (*state)->result.has_gpu_time =
            stats.validFlags & skgpu::GpuStatsFlags::kElapsedTime;
    (*state)->result.gpu_time_ns = stats.elapsedTime;
    (*state)->complete.store(true, std::memory_order_release);
}
void read_finished(void* opaque, std::unique_ptr<const SkImage::AsyncReadResult> result) {
    std::unique_ptr<std::shared_ptr<Readback>> state(static_cast<std::shared_ptr<Readback>*>(opaque));
    (*state)->value = std::move(result);
    (*state)->complete.store(true, std::memory_order_release);
}
template <typename Source> cfsk_status readback(cfsk_context* context, const Source* source,
        const cfsk_image_info* info, cfsk_irect source_rect, cfsk_readback** output) {
    CFSK_REQUIRE(source && cfsk::valid(info));
    if (auto status = context_ready(context); status != CFSK_OK) return status;
    CFSK_REQUIRE(source->gpu_context == context && !cfsk::irect(source_rect).isEmpty() &&
                 SkIRect::MakeWH(source->value->width(), source->value->height()).contains(cfsk::irect(source_rect)));
    auto state = std::make_shared<Readback>();
    auto callback = new (std::nothrow) std::shared_ptr<Readback>(state);
    auto ticket = new (std::nothrow) cfsk_readback{state, context, cfsk::image_info(*info)};
    if (!callback || !ticket) { delete callback; delete ticket; return cfsk::fail(CFSK_OUT_OF_MEMORY, "readback allocation failed"); }
    context->value->asyncRescaleAndReadPixels(source->value.get(), ticket->info, cfsk::irect(source_rect),
        SkImage::RescaleGamma::kSrc, SkImage::RescaleMode::kNearest, read_finished, callback);
    *output = ticket;
    return CFSK_OK;
}
}
#define CONTEXT_READY(c) do { auto status = context_ready(c); if (status != CFSK_OK) return status; } while (0)
#define RECORDER_READY(r) do { auto status = recorder_ready(r); if (status != CFSK_OK) return status; } while (0)

cfsk_status cfsk::mesh_recorder_identity(cfsk_recorder* recorder, const void** output) {
    if (!recorder) { *output = nullptr; return CFSK_OK; }
    RECORDER_READY(recorder);
    *output = recorder->value.get();
    return CFSK_OK;
}

static cfsk_status validate_persistent(cfsk_bytes data) {
    CFSK_REQUIRE(cfsk::valid(data) && data.length <= 64 * 1024 * 1024);
    if (!data.length) return CFSK_OK;
    CFSK_REQUIRE(data.length >= 4 && !std::memcmp(data.data, "CFP1", 4));
    for (size_t offset = 4; offset < data.length;) {
        CFSK_REQUIRE(data.length - offset >= 4);
        uint32_t size = 0;
        for (int shift = 0; shift < 32; shift += 8) size |= uint32_t(data.data[offset++]) << shift;
        CFSK_REQUIRE(size && size <= data.length - offset);
        offset += size;
    }
    return CFSK_OK;
}

static cfsk_status make_dawn(const cfsk_context_options* options, cfsk_bytes persistent_data,
        const std::vector<sk_sp<SkRuntimeEffect>>& known_effects, uint32_t coverage,
        cfsk_context** output) {
    CFSK_BEGIN;
    CFSK_OUTPUT(output);
    if (auto status = validate_persistent(persistent_data); status != CFSK_OK) return status;
    CFSK_REQUIRE(options && options->dawn_instance && options->dawn_device && options->dawn_queue &&
        (options->internal_samples == 1 || options->internal_samples == 4) &&
        (!options->known_effect_count || options->known_effects));
    std::vector<sk_sp<SkRuntimeEffect>> effects;
    for (size_t i = 0; i < options->known_effect_count; ++i) {
        CFSK_REQUIRE(options->known_effects[i]);
        effects.push_back(options->known_effects[i]->value);
    }
    std::unique_ptr<cfsk_context> context(new (std::nothrow) cfsk_context);
    effects.insert(effects.end(), known_effects.begin(), known_effects.end());
    if (!context) return cfsk::fail(CFSK_OUT_OF_MEMORY, "context allocation failed");
    context->dawn.fInstance = wgpu::Instance(static_cast<WGPUInstance>(options->dawn_instance));
    context->dawn.fDevice = wgpu::Device(static_cast<WGPUDevice>(options->dawn_device));
    context->dawn.fQueue = wgpu::Queue(static_cast<WGPUQueue>(options->dawn_queue));
    graphite::ContextOptions policy;
    policy.fRequireOrderedRecordings = true;
    // Match raster strike selection for large outline glyphs.
    policy.fGlyphsAsPathsFontSize = 256;
    if (coverage == CFSK_COVERAGE_CPU_MATCHED) {
        policy.fMinimumPathSizeForMSAA = SK_ScalarInfinity;
    }
    policy.fInternalMultisampleCount = graphite::ToSampleCount(options->internal_samples);
    policy.fGpuBudgetInBytes = options->gpu_budget_bytes;
    policy.fUserDefinedKnownRuntimeEffects = SkSpan<sk_sp<SkRuntimeEffect>>(effects);
    context->storage.data = SkData::MakeWithCopy(persistent_data.data, persistent_data.length);
    policy.fPersistentPipelineStorage = &context->storage;
    policy.fPipelineCallbackContext = context.get();
    policy.fPipelineCachingCallback = [](void* opaque, graphite::ContextOptions::PipelineCacheOp op,
            const std::string&, uint32_t hash, bool from_precompile, sk_sp<SkData> data) {
        auto owner = static_cast<cfsk_context*>(opaque);
        const bool adding = op == graphite::ContextOptions::PipelineCacheOp::kAddingPipeline;
        const bool capture = owner->capturing.load(std::memory_order_relaxed) &&
                             owner->owner == std::this_thread::get_id();
        if (!adding && !capture) return;
        std::lock_guard<std::mutex> lock(owner->pipeline_mutex);
        if (adding) {
            if (from_precompile) ++owner->precompiled_count;
            else { ++owner->inline_count; if (!data) ++owner->unkeyable_count; }
            if (data) {
                if (data->size() <= kPipelineKeyBudget) {
                    while (owner->queued_key_bytes + data->size() > kPipelineKeyBudget) {
                        owner->queued_key_bytes -= owner->pipelines.front()->size();
                        owner->pipelines.pop_front();
                    }
                    owner->queued_key_bytes += data->size();
                    owner->pipelines.push_back(data);
                }
                auto [first, last] = owner->keys_by_hash.equal_range(hash);
                auto same = [&](const auto& item) {
                    return item.second->size() == data->size() &&
                           !std::memcmp(item.second->data(), data->data(), data->size());
                };
                if (std::find_if(first, last, same) == last &&
                    owner->indexed_key_bytes + data->size() <= kPipelineKeyBudget) {
                    owner->indexed_key_bytes += data->size();
                    owner->keys_by_hash.emplace(hash, data);
                    if (capture && owner->captured_hashes.count(hash)) owner->captured_keys.push_back(data);
                }
            }
        }
        if (capture && owner->captured_hashes.insert(hash).second) {
            auto [first, last] = owner->keys_by_hash.equal_range(hash);
            for (; first != last; ++first) owner->captured_keys.push_back(first->second);
        }
    };
    context->value = graphite::ContextFactory::MakeDawn(context->dawn, policy);
    if (!context->value) return cfsk::fail(CFSK_FAILED, "Graphite MakeDawn failed");
    auto saved = context->storage.load();
    if (saved && saved->size()) {
        auto bytes = static_cast<const uint8_t*>(saved->data());
        CFSK_REQUIRE(saved->size() >= 4 && !std::memcmp(bytes, "CFP1", 4));
        auto precompile = context->value->makePrecompileContext();
        for (size_t offset = 4; offset < saved->size();) {
            CFSK_REQUIRE(saved->size() - offset >= 4);
            uint32_t size = 0;
            for (int shift = 0; shift < 32; shift += 8) size |= uint32_t(bytes[offset++]) << shift;
            CFSK_REQUIRE(size && size <= saved->size() - offset);
            if (!precompile->precompile(SkData::MakeWithCopy(bytes + offset, size)))
                return cfsk::fail(CFSK_FAILED, ("persistent pipeline key rejected at byte " + std::to_string(offset)).c_str());
            offset += size;
        }
    }
    *output = context.release();
    return CFSK_OK;
}
cfsk_status cfsk_context_make_dawn(const cfsk_context_options* options, cfsk_context** output) {
    return make_dawn(options, {}, {}, CFSK_COVERAGE_CPU_MATCHED, output);
}
cfsk_status cfsk_context_make_dawn_persistent(cfsk_dawn_host* host, const cfsk_picture_resources* resources,
        cfsk_bytes persistent_data, cfsk_context** output) {
    return cfsk_context_make_dawn_persistent_with_coverage(
            host, resources, persistent_data, CFSK_COVERAGE_CPU_MATCHED, output);
}
cfsk_status cfsk_context_make_dawn_persistent_with_coverage(cfsk_dawn_host* host,
        const cfsk_picture_resources* resources, cfsk_bytes persistent_data, uint32_t coverage,
        cfsk_context** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output);
    CFSK_REQUIRE(coverage == CFSK_COVERAGE_GPU || coverage == CFSK_COVERAGE_CPU_MATCHED);
    if (auto status = cfsk::dawn_host_ready(host); status != CFSK_OK) return status;
    auto options = cfsk::dawn_context_options(host);
    auto known_effects = cfsk::picture_known_effects(resources);
    auto status = make_dawn(&options, persistent_data, known_effects, coverage, output);
    if (status == CFSK_OK) (*output)->native_host = host->native;
    return status;
}
cfsk_status cfsk_context_make_vulkan(const cfsk_picture_resources* resources, cfsk_bytes persistent_data, cfsk_context** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); CFSK_REQUIRE(cfsk::valid(persistent_data));
    cfsk_dawn_host* host = nullptr;
    if (auto status = cfsk_dawn_host_make(&host); status != CFSK_OK) return status;
    auto status = cfsk_context_make_dawn_persistent(host, resources, persistent_data, output);
    // Preserve the construction diagnostic while disposing the temporary host wrapper.
    auto diagnostic = cfsk::error;
    cfsk_dawn_host_release(&host);
    cfsk::error = std::move(diagnostic);
    return status;
}
cfsk_status cfsk_context_pipeline_cache(cfsk_context* context, cfsk_data** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); CONTEXT_READY(context);
    context->value->syncPipelineData(64 * 1024 * 1024);
    return cfsk::resource(output, context->storage.data);
}
cfsk_status cfsk_context_pipeline_stats(cfsk_context* context, uint32_t* inline_count,
        uint32_t* precompiled_count, uint32_t* unkeyable_count) {
    CFSK_BEGIN; CONTEXT_READY(context); CFSK_REQUIRE(inline_count && precompiled_count && unkeyable_count);
    std::lock_guard<std::mutex> lock(context->pipeline_mutex);
    *inline_count = context->inline_count;
    *precompiled_count = context->precompiled_count;
    *unkeyable_count = context->unkeyable_count;
    return CFSK_OK;
}
namespace {
class RecorderImageProvider : public skgpu::graphite::ImageProvider {
public:
    RecorderImageProvider() : fCache(kDefaultNumCachedImages) {}

    sk_sp<SkImage> findOrCreate(skgpu::graphite::Recorder* recorder,
                                const SkImage* image,
                                SkImage::RequiredProperties requiredProps) override {
        if (!requiredProps.fMipmapped) {
            // If no mipmaps are required, check to see if we have a mipmapped version anyway -
            // since it can be used in that case.
            ImageKey mipMappedKey(image, /* mipmapped= */ true);
            auto result = fCache.find(mipMappedKey);
            if (result) {
                return *result;
            }
        }

        ImageKey key(image, requiredProps.fMipmapped);

        auto result = fCache.find(key);
        if (result) {
            return *result;
        }

        sk_sp<SkImage> newImage = SkImages::TextureFromImage(recorder, image, requiredProps);
        if (!newImage) {
            return nullptr;
        }

        result = fCache.insert(key, std::move(newImage));
        SkASSERT(result);

        return *result;
    }

private:
    static constexpr int kDefaultNumCachedImages = 256;

    class ImageKey {
    public:
        ImageKey(const SkImage* image, bool mipmapped) {
            uint32_t flags = mipmapped ? 0x1 : 0x0;
            SkTiledImageUtils::GetImageKeyValues(image, &fValues[1]);
            fValues[kNumValues - 1] = flags;
            fValues[0] = SkChecksum::Hash32(&fValues[1], (kNumValues - 1) * sizeof(uint32_t));
        }

        uint32_t hash() const { return fValues[0]; }

        bool operator==(const ImageKey& other) const {
            for (int i = 0; i < kNumValues; ++i) {
                if (fValues[i] != other.fValues[i]) {
                    return false;
                }
            }

            return true;
        }
        bool operator!=(const ImageKey& other) const { return !(*this == other); }

    private:
        static const int kNumValues = SkTiledImageUtils::kNumImageKeyValues + 2;

        uint32_t fValues[kNumValues];
    };

    struct ImageHash {
        size_t operator()(const ImageKey& key) const { return key.hash(); }
    };

    SkLRUCache<ImageKey, sk_sp<SkImage>, ImageHash> fCache;
};

}  // namespace

cfsk_status cfsk_context_make_recorder(cfsk_context* context, cfsk_recorder** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); CONTEXT_READY(context);
    graphite::RecorderOptions options;
    options.fImageProvider = sk_make_sp<RecorderImageProvider>();
    auto recorder = context->value->makeRecorder(options);
    if (!recorder) return cfsk::fail(CFSK_FAILED, "makeRecorder failed");
    *output = new (std::nothrow) cfsk_recorder{std::move(recorder), context, {}};
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "recorder allocation failed");
}
cfsk_status cfsk_recorder_snap(cfsk_recorder* recorder, cfsk_recording** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); RECORDER_READY(recorder);
    auto recording = recorder->value->snap();
    if (!recording) return cfsk::fail(CFSK_FAILED, "recorder snap failed");
    *output = new (std::nothrow) cfsk_recording{std::move(recording), recorder->context};
    return *output ? CFSK_OK : cfsk::fail(CFSK_OUT_OF_MEMORY, "recording allocation failed");
}
cfsk_status cfsk_context_insert(cfsk_context* context, cfsk_recording** recording, uint8_t gpu_time, cfsk_submission** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); CONTEXT_READY(context);
    CFSK_REQUIRE(recording && *recording && (*recording)->context == context && gpu_time <= 1);
    if (gpu_time && !(context->value->supportedGpuStats() & skgpu::GpuStatsFlags::kElapsedTime))
        return cfsk::unsupported("GPU elapsed-time statistics unavailable on this device");
    auto state = std::make_shared<Completion>();
    auto ticket = new (std::nothrow) cfsk_submission{state, context};
    auto callback = new (std::nothrow) std::shared_ptr<Completion>(state);
    if (!ticket || !callback) { delete ticket; delete callback; return cfsk::fail(CFSK_OUT_OF_MEMORY, "submission allocation failed"); }
    graphite::InsertRecordingInfo info;
    info.fRecording = (*recording)->value.get();
    info.fFinishedContext = callback;
    info.fFinishedWithStatsProc = finished;
    info.fGpuStatsFlags = gpu_time ? skgpu::GpuStatsFlags::kElapsedTime : skgpu::GpuStatsFlags::kNone;
    auto result = context->value->insertRecording(info);
    if (static_cast<graphite::InsertStatus::V>(result) != graphite::InsertStatus::kSuccess) {
        // Graphite invokes its completion callback even when insertion fails.
        delete ticket;
        return cfsk::fail(context->value->isDeviceLost() ? CFSK_DEVICE_LOST : CFSK_FAILED, "insertRecording failed; caller retains Recording");
    }
    delete *recording; *recording = nullptr;
    *output = ticket;
    return CFSK_OK;
}
cfsk_status cfsk_context_submit(cfsk_context* context, uint8_t wait_for_cpu) {
    CFSK_BEGIN; CONTEXT_READY(context); CFSK_REQUIRE(wait_for_cpu <= 1);
    if (!context->value->submit(wait_for_cpu ? graphite::SyncToCpu::kYes : graphite::SyncToCpu::kNo))
        return cfsk::fail(context->value->isDeviceLost() ? CFSK_DEVICE_LOST : CFSK_FAILED, "Graphite submit failed");
    return CFSK_OK;
}
#if !defined(__EMSCRIPTEN__)
extern "C" cfsk_status cfsk_context_submit_fenced(cfsk_context* context, uint64_t* fence) {
    CFSK_BEGIN; CONTEXT_READY(context); CFSK_REQUIRE(fence);
    if (auto status = cfsk_context_submit(context, 0); status != CFSK_OK) return status;
    *fence = context->dawn.fQueue.OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowProcessEvents, [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {}).id;
    return CFSK_OK;
}
extern "C" cfsk_status cfsk_context_wait_fence(cfsk_context* context, uint64_t fence, uint64_t timeout_ns) {
    CFSK_BEGIN; CONTEXT_READY(context); CFSK_REQUIRE(fence);
    wgpu::FutureWaitInfo info{};
    info.future.id = fence;
    switch (context->dawn.fInstance.WaitAny(1, &info, timeout_ns)) {
        case wgpu::WaitStatus::Success: return progress(context);
        case wgpu::WaitStatus::TimedOut: return CFSK_PENDING;
        default: return cfsk::fail(CFSK_FAILED, "Dawn completion wait failed");
    }
}
extern "C" void cfsk_context_abandon(cfsk_context* context) {
    if (context && context->owner == std::this_thread::get_id()) {
        context->dawn.fDevice.Destroy();
    }
}
#endif
cfsk_status cfsk_context_poll(cfsk_context* context) { CFSK_BEGIN; return progress(context); }
cfsk_status cfsk_submission_poll(cfsk_submission* submission, cfsk_completion* output) {
    CFSK_BEGIN; CFSK_REQUIRE(submission && output);
    if (auto status = progress(submission->context); status != CFSK_OK) return status;
    if (!submission->value->complete.load(std::memory_order_acquire)) return CFSK_PENDING;
    if (submission->value->status != CFSK_OK) return cfsk::fail(CFSK_FAILED, "GPU recording failed");
    *output = submission->value->result;
    return CFSK_OK;
}
cfsk_status cfsk_context_cache_usage(cfsk_context* context, cfsk_cache_usage* output) {
    CFSK_BEGIN; CONTEXT_READY(context); CFSK_REQUIRE(output);
    *output = {context->value->currentBudgetedBytes(), context->value->currentPurgeableBytes(), context->value->maxBudgetedBytes()};
    return CFSK_OK;
}
cfsk_status cfsk_context_set_cache_limit(cfsk_context* context, size_t bytes) {
    CFSK_BEGIN; CONTEXT_READY(context); context->value->setMaxBudgetedBytes(bytes); return CFSK_OK;
}
cfsk_status cfsk_context_capture_pipelines(cfsk_context* context, cfsk_data** output) {
    CFSK_BEGIN; CONTEXT_READY(context);
    std::lock_guard<std::mutex> lock(context->pipeline_mutex);
    context->capturing.store(!output, std::memory_order_relaxed);
    cfsk_status status = CFSK_OK;
    if (output) {
        *output = nullptr;
        std::vector<uint8_t> bytes{'C', 'F', 'P', '1'};
        for (const auto& key : context->captured_keys) {
            if (key->size() + bytes.size() + 4 > 64 * 1024 * 1024) break;
            for (int shift = 0; shift < 32; shift += 8) bytes.push_back(uint8_t(key->size() >> shift));
            const auto* data = static_cast<const uint8_t*>(key->data());
            bytes.insert(bytes.end(), data, data + key->size());
        }
        status = cfsk::resource(output, SkData::MakeWithCopy(bytes.data(), bytes.size()));
    }
    context->captured_hashes.clear();
    context->captured_keys.clear();
    return status;
}

cfsk_status cfsk_context_precompile_deferred(cfsk_context* context, cfsk_bytes keys) {
    CFSK_BEGIN; CONTEXT_READY(context); CFSK_REQUIRE(cfsk::valid(keys));
    if (!keys.length) return CFSK_OK;
    CFSK_REQUIRE(keys.length >= 4 && keys.length <= 64 * 1024 * 1024 &&
                 !std::memcmp(keys.data, "CFP1", 4));
    if (context->precompile_busy.load()) return CFSK_PENDING;
    std::vector<sk_sp<SkData>> work;
    for (size_t offset = 4; offset < keys.length;) {
        CFSK_REQUIRE(keys.length - offset >= 4);
        uint32_t size = 0;
        for (int shift = 0; shift < 32; shift += 8) size |= uint32_t(keys.data[offset++]) << shift;
        CFSK_REQUIRE(size && size <= keys.length - offset);
        work.push_back(SkData::MakeWithCopy(keys.data + offset, size));
        offset += size;
    }
    if (work.empty()) return CFSK_OK;
    context->precompile_worker.join();
    auto precompile = context->value->makePrecompileContext();
    if (!precompile) return cfsk::unsupported("Graphite precompile is unavailable");
    auto run = [context, work = std::move(work), pc = std::move(precompile)] {
        SK_AT_SCOPE_EXIT(context->precompile_busy.store(false));
        for (const auto& key : work) {
            if (context->precompile_stop.load()) break;
            pc->precompile(key);
        }
    };
    auto task = std::make_unique<decltype(run)>(std::move(run));
    context->precompile_busy.store(true);
    if (!context->precompile_worker.start(task.get())) {
        context->precompile_busy.store(false);
        return cfsk::fail(CFSK_FAILED, "could not start Graphite precompile worker");
    }
    task.release();
    return CFSK_OK;
}

cfsk_status cfsk_context_precompile(cfsk_context* context, cfsk_bytes serialized_pipeline) {
    CFSK_BEGIN; CONTEXT_READY(context); CFSK_REQUIRE(cfsk::valid(serialized_pipeline) && serialized_pipeline.length);
    auto precompile = context->value->makePrecompileContext();
    if (!precompile || !precompile->precompile(SkData::MakeWithCopy(serialized_pipeline.data, serialized_pipeline.length)))
        return cfsk::fail(CFSK_FAILED, "pipeline precompile failed");
    return CFSK_OK;
}
cfsk_status cfsk_context_take_pipeline(cfsk_context* context, cfsk_data** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); CONTEXT_READY(context);
    std::lock_guard<std::mutex> lock(context->pipeline_mutex);
    if (context->pipelines.empty()) return CFSK_OK;
    auto status = cfsk::resource(output, context->pipelines.front());
    if (status == CFSK_OK) {
        context->queued_key_bytes -= context->pipelines.front()->size();
        context->pipelines.pop_front();
    }
    return status;
}
cfsk_status cfsk_surface_make_graphite(cfsk_recorder* recorder, const cfsk_image_info* info, uint8_t mipmapped, cfsk_surface** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); RECORDER_READY(recorder);
    CFSK_REQUIRE(cfsk::valid(info) && mipmapped <= 1);
    SkSurfaceProps props(SkSurfaceProps::kDynamicMSAA_Flag, kUnknown_SkPixelGeometry);
    auto status = cfsk::resource(output, SkSurfaces::RenderTarget(recorder->value.get(), cfsk::image_info(*info), static_cast<skgpu::Mipmapped>(mipmapped), &props));
    if (status == CFSK_OK) (*output)->gpu_context = recorder->context;
    return status;
}
cfsk_status cfsk_surface_wrap_texture(cfsk_recorder* recorder, void* dawn_texture, const cfsk_image_info* info, cfsk_surface** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); RECORDER_READY(recorder); CFSK_REQUIRE(dawn_texture && cfsk::valid(info));
    auto backend = graphite::BackendTextures::MakeDawn(static_cast<WGPUTexture>(dawn_texture));
    CFSK_REQUIRE(backend.dimensions() == SkISize::Make(info->width, info->height));
    auto status = cfsk::resource(output, SkSurfaces::WrapBackendTexture(recorder->value.get(), backend, static_cast<SkColorType>(info->color_type), cfsk::ref(info->color_space), nullptr));
    if (status == CFSK_OK) (*output)->gpu_context = recorder->context;
    return status;
}
cfsk_status cfsk_image_upload(cfsk_recorder* recorder, const cfsk_image* source, uint8_t mipmapped, cfsk_image** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); RECORDER_READY(recorder);
    CFSK_REQUIRE(source && mipmapped <= 1 && (!source->gpu_context || source->gpu_context == recorder->context));
    auto status = cfsk::resource(output, SkImages::TextureFromImage(recorder->value.get(), source->value, {static_cast<bool>(mipmapped)}));
    if (status == CFSK_OK) (*output)->gpu_context = recorder->context;
    return status;
}
cfsk_status cfsk_image_import_texture(cfsk_recorder* recorder, void* dawn_texture, const cfsk_image_info* info, uint32_t origin, uint8_t generate_mipmaps, cfsk_image** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); RECORDER_READY(recorder);
    CFSK_REQUIRE(dawn_texture && cfsk::valid(info) && origin <= 1 && generate_mipmaps <= 1);
    auto backend = graphite::BackendTextures::MakeDawn(static_cast<WGPUTexture>(dawn_texture));
    CFSK_REQUIRE(backend.dimensions() == SkISize::Make(info->width, info->height));
    auto status = cfsk::resource(output, SkImages::WrapTexture(recorder->value.get(), backend, static_cast<SkColorType>(info->color_type), static_cast<SkAlphaType>(info->alpha_type), cfsk::ref(info->color_space), static_cast<skgpu::Origin>(origin), static_cast<SkImages::GenerateMipmapsFromBase>(generate_mipmaps)));
    if (status == CFSK_OK) (*output)->gpu_context = recorder->context;
    return status;
}
cfsk_status cfsk_surface_readback(cfsk_context* context, cfsk_surface* surface, const cfsk_image_info* info, cfsk_irect source, cfsk_readback** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); return readback(context, surface, info, source, output);
}
cfsk_status cfsk_image_readback(cfsk_context* context, const cfsk_image* image, const cfsk_image_info* info, cfsk_irect source, cfsk_readback** output) {
    CFSK_BEGIN; CFSK_OUTPUT(output); return readback(context, image, info, source, output);
}
cfsk_status cfsk_readback_copy(cfsk_readback* readback, void* output, size_t output_bytes, size_t row_bytes, size_t* required_bytes) {
    CFSK_BEGIN; CFSK_REQUIRE(readback && required_bytes && (output || !output_bytes));
    if (auto status = progress(readback->context); status != CFSK_OK) return status;
    if (!readback->state->complete.load(std::memory_order_acquire)) return CFSK_PENDING;
    if (!readback->state->value) return cfsk::fail(CFSK_FAILED, "GPU readback failed");
    CFSK_REQUIRE(cfsk::extent(readback->info, row_bytes, SIZE_MAX, required_bytes));
    if (!output && !output_bytes) return CFSK_OK;
    if (output_bytes < *required_bytes) return cfsk::fail(CFSK_BUFFER_TOO_SMALL, "readback buffer too small");
    const auto& result = readback->state->value;
    for (int row = 0; row < readback->info.height(); ++row) {
        std::memcpy(static_cast<uint8_t*>(output) + row * row_bytes,
                    static_cast<const uint8_t*>(result->data(0)) + row * result->rowBytes(0), readback->info.minRowBytes());
    }
    return CFSK_OK;
}
void cfsk_context_destroy(cfsk_context* value) { delete value; }
void cfsk_recorder_destroy(cfsk_recorder* value) { delete value; }
void cfsk_recording_destroy(cfsk_recording* value) { delete value; }
void cfsk_submission_destroy(cfsk_submission* value) { delete value; }
void cfsk_readback_destroy(cfsk_readback* value) { delete value; }
