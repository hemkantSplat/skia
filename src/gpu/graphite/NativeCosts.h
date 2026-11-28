/*
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef skgpu_graphite_NativeCosts_DEFINED
#define skgpu_graphite_NativeCosts_DEFINED

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace skgpu::graphite {

enum class CostPhase { kRecord, kSnap, kInsert, kCount };
enum class NativeCost {
    kGlyphMask,
    kAtlasInsert,
    kAtlasUpload,
    kImplicitUpload,
    kTargetAllocation,
    kPipelineCompile,
    kPipelineWait,
    kCount
};

struct MeasuredCost {
    uint64_t nanos = 0;
    uint64_t count = 0;
};

struct PhaseCosts {
    uint64_t totalNanos = 0;
    std::array<MeasuredCost, static_cast<size_t>(NativeCost::kCount)> costs{};
};

using NativeCosts = std::array<PhaseCosts, static_cast<size_t>(CostPhase::kCount)>;

class NativeCostScope;
struct NativeCostState {
    NativeCosts costs{};
    CostPhase phase = CostPhase::kRecord;
    NativeCostScope* parent = nullptr;
};

// The diagnostic is confined to the thread that owns the current Graphite work.
// Async pipeline work is charged only when this thread waits for it.
inline thread_local NativeCostState* gNativeCostState = nullptr;

using NativeCostClock = std::chrono::steady_clock;

inline uint64_t native_cost_ns(NativeCostClock::duration elapsed) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}

class NativeCostPhase {
public:
    explicit NativeCostPhase(CostPhase phase) : fState(gNativeCostState) {
        if (fState) {
            fPrevious = fState->phase;
            fState->phase = phase;
            fStart = NativeCostClock::now();
        }
    }

    ~NativeCostPhase() {
        if (fState) {
            fState->costs[static_cast<size_t>(fState->phase)].totalNanos +=
                    native_cost_ns(NativeCostClock::now() - fStart);
            fState->phase = fPrevious;
        }
    }

private:
    NativeCostState* fState;
    CostPhase fPrevious;
    NativeCostClock::time_point fStart;
};

class NativeCostScope {
public:
    explicit NativeCostScope(NativeCost cost, uint64_t count = 1)
            : fState(gNativeCostState), fCost(cost), fCount(count) {
        if (fState) {
            fStart = NativeCostClock::now();
            fParent = fState->parent;
            fState->parent = this;
        }
    }

    ~NativeCostScope() {
        if (fState) {
            const uint64_t elapsed = native_cost_ns(NativeCostClock::now() - fStart);
            auto& phase = fState->costs[static_cast<size_t>(fState->phase)];
            auto& cost = phase.costs[static_cast<size_t>(fCost)];
            cost.nanos += elapsed - fChildNanos;
            cost.count += fCount;
            if (fParent) {
                fParent->fChildNanos += elapsed;
            }
            fState->parent = fParent;
        }
    }

private:
    NativeCostState* fState;
    NativeCost fCost;
    uint64_t fCount;
    NativeCostScope* fParent;
    uint64_t fChildNanos = 0;
    NativeCostClock::time_point fStart;
};

inline void count_native_cost(NativeCost cost) {
    if (auto* state = gNativeCostState) {
        ++state->costs[static_cast<size_t>(state->phase)].costs[static_cast<size_t>(cost)].count;
    }
}

}  // namespace skgpu::graphite
#endif
