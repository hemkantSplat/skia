/*
 * The retained composition plan (CourseForge plan §4.4, wire format v1): one instance per (scope, epoch), fed by
 * validated packets and named only by registered (handle, generation) keys, never by raw object pointers.
 */
#ifndef CompositionPlan_DEFINED
#define CompositionPlan_DEFINED

#include "include/core/SkBlendMode.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkM44.h"
#include "include/core/SkPath.h"
#include "include/core/SkPicture.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <vector>

class SkCanvas;

namespace cfplan {

constexpr uint32_t kMagic = 0x4C504643;  // "CFPL", little-endian
constexpr uint16_t kAbiVersion = 1;
constexpr size_t kHeaderBytes = 32;
constexpr size_t kRecordBytes = 16;

constexpr uint32_t kRoot = 0;
constexpr uint32_t kNone = 0xFFFFFFFF;  // the append sentinel; also "no node" and an exhausted identity

constexpr uint32_t kMaxNodes = 1u << 20;
constexpr uint32_t kMaxDepth = 256;  // edges from node 0 to the deepest node
constexpr uint32_t kMaxRecords = 1u << 20;
constexpr uint32_t kMaxFloats = 1u << 24;
constexpr size_t kMaxPacketBytes = size_t(64) << 20;
constexpr uint32_t kMaxPathHandle = 0xFFFF;  // producer path handles are 1..2^16-1
constexpr uint32_t kMaxPaths = 1u << 16;

enum class Error : uint32_t {
    kOk = 0,
    kBadMagic = 1,
    kBadVersion = 2,
    kStaleSequence = 3,
    kBadInstance = 4,
    kUnknownNode = 5,
    kDuplicateNode = 6,
    kBadParent = 7,
    kCycle = 8,
    kBadPayload = 9,
    kLimit = 10,
    kUnknownPicture = 11,
    kUnknownPath = 12,
    kReplayActive = 13,
    kTokenOrder = 14,
    kLiveUnderRestore = 15,
    kBadGeneration = 16,
    kAlloc = 17,
    kDepth = 18,
    kRootImmutable = 19,
};

enum class Op : uint16_t {
    kInsert = 1,
    kRemove = 2,
    kMove = 3,
    kSetMatrix = 4,
    kSetClip = 5,
    kSetIsolation = 6,
    kSetPicture = 7,
    kSetLive = 8,
    kSetOutset = 9,
    kDefinePath = 10,
    kReleasePath = 11,
    kSetVisible = 12,
    kSetContain = 13,
    kSetSpace = 14,
};

enum class Kind : uint8_t { kGroup = 1, kPlacement = 2, kClip = 3, kIsolation = 4, kPicture = 5, kLive = 6 };

// A registered object's identity: (handle, generation).
using Key = uint64_t;
constexpr Key kNoKey = ~Key(0);
constexpr Key MakeKey(uint32_t handle, uint32_t gen) { return (Key(handle) << 32) | gen; }
constexpr uint32_t KeyHandle(Key key) { return uint32_t(key >> 32); }
constexpr uint32_t KeyGen(Key key) { return uint32_t(key); }

// `record` is the failing record's index, or kNone when the failure is the packet's (header, final topology, allocation).
struct ApplyResult {
    Error code;
    uint32_t record;
};

// ---- checked storage: every allocation apply makes is a malloc whose failure is a null, never an abort -------------

// Counts apply's allocations; a test may make one of them fail.
struct Allocator {
    uint64_t calls = 0;
    uint64_t failAt = 0;  // the call that returns null; 0: none

    void* allocate(size_t bytes) {
        if (++calls == failAt) {
            failAt = 0;
            return nullptr;
        }
        return std::malloc(bytes);
    }
};

// A growable array of trivially copyable T in checked memory; growth reports failure instead of aborting.
template <typename T> class Buffer {
    static_assert(std::is_trivially_copyable<T>::value, "Buffer relocates by memcpy");

public:
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() { std::free(fData); }

    uint32_t size() const { return fSize; }
    uint32_t capacity() const { return fCap; }
    T& operator[](uint32_t i) { return fData[i]; }
    const T& operator[](uint32_t i) const { return fData[i]; }
    T& back() { return fData[fSize - 1]; }

    bool reserve(Allocator& alloc, uint32_t n) {
        if (n <= fCap) return true;
        uint64_t cap = std::max<uint64_t>(n, std::max<uint64_t>(uint64_t(fCap) * 2, 8));
        if (cap > UINT32_MAX || cap > SIZE_MAX / sizeof(T)) cap = n;
        if (cap > SIZE_MAX / sizeof(T)) return false;
        T* data = static_cast<T*>(alloc.allocate(size_t(cap) * sizeof(T)));
        if (!data) return false;
        if (fSize) std::memcpy(static_cast<void*>(data), fData, sizeof(T) * fSize);
        std::free(fData);
        fData = data;
        fCap = uint32_t(cap);
        return true;
    }
    // Within reserved capacity only.
    void push(const T& value) {
        SkASSERT_RELEASE(fSize < fCap);
        new (fData + fSize++) T(value);
    }
    void pop() { fSize--; }
    void swap(Buffer& other) { std::swap(fData, other.fData); std::swap(fSize, other.fSize); std::swap(fCap, other.fCap); }
    void clear() { fSize = 0; }

private:
    T* fData = nullptr;
    uint32_t fSize = 0, fCap = 0;
};

// Open addressing from K to a u32 in checked memory. A slot is live while its stamp equals the 64-bit epoch, which never
// wraps, so clear() is O(1) whatever capacity a past packet reached; erase shifts later entries back, leaving no tombstones.
template <typename K> class Index {
public:
    Index() = default;
    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;
    ~Index() { std::free(fSlots); }

    uint32_t size() const { return fCount; }

    const uint32_t* find(K key) const {
        if (!fSlots) return nullptr;
        for (uint32_t i = Home(key);; i = (i + 1) & fMask) {
            const Slot& s = fSlots[i];
            if (s.stamp != fEpoch) return nullptr;
            if (s.key == key) return &s.value;
        }
    }
    uint32_t* find(K key) { return const_cast<uint32_t*>(static_cast<const Index*>(this)->find(key)); }

    // Room for `n` keys at a load of at most one half.
    bool reserve(Allocator& alloc, uint32_t n) {
        const uint64_t need = std::max<uint64_t>(16, uint64_t(n) * 2);
        if (fSlots && need <= uint64_t(fMask) + 1) return true;
        uint64_t cap = 16;
        while (cap < need) cap *= 2;
        if (cap > UINT32_MAX || cap > SIZE_MAX / sizeof(Slot)) return false;
        Slot* slots = static_cast<Slot*>(alloc.allocate(size_t(cap) * sizeof(Slot)));
        if (!slots) return false;
        std::memset(static_cast<void*>(slots), 0, size_t(cap) * sizeof(Slot));
        Slot* old = fSlots;
        const uint32_t oldCap = fSlots ? fMask + 1 : 0;
        const uint64_t epoch = fEpoch;
        fSlots = slots;
        fMask = uint32_t(cap - 1);
        fEpoch = 1;
        fCount = 0;
        for (uint32_t i = 0; i < oldCap; i++) {
            if (old[i].stamp == epoch) insert(old[i].key) = old[i].value;
        }
        std::free(old);
        return true;
    }
    // `key` must be absent and room reserved.
    uint32_t& insert(K key) {
        SkASSERT_RELEASE(fSlots && uint64_t(fCount + 1) * 2 <= uint64_t(fMask) + 1);
        uint32_t i = Home(key);
        while (fSlots[i].stamp == fEpoch) i = (i + 1) & fMask;
        fSlots[i] = {key, 0, fEpoch};
        fCount++;
        return fSlots[i].value;
    }
    void erase(K key) {
        uint32_t i = Home(key);
        while (fSlots[i].key != key || fSlots[i].stamp != fEpoch) i = (i + 1) & fMask;
        // An entry later in the run moves back unless its home lies cyclically in (i, j].
        for (uint32_t j = (i + 1) & fMask; fSlots[j].stamp == fEpoch; j = (j + 1) & fMask) {
            const uint32_t home = Home(fSlots[j].key);
            const bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
            if (!stays) {
                fSlots[i] = fSlots[j];
                i = j;
            }
        }
        fSlots[i].stamp = 0;
        fCount--;
    }
    void clear() {
        fCount = 0;
        ++fEpoch;
        SkASSERT_RELEASE(fEpoch != 0);  // 2^64 clears: unreachable in any instance's lifetime
    }
    uint64_t epoch() const { return fEpoch; }
    // Test-only: moves an empty index's epoch forward, as if that many clears had run.
    void setEpochForTesting(uint64_t epoch) {
        SkASSERT_RELEASE(fCount == 0 && epoch >= fEpoch);
        fEpoch = epoch;
    }

private:
    struct Slot {
        K key;
        uint32_t value;
        uint64_t stamp;
    };
    uint32_t Home(K key) const { return uint32_t((uint64_t(key) * 0x9E3779B97F4A7C15ull) >> 32) & fMask; }

    Slot* fSlots = nullptr;
    uint32_t fMask = 0, fCount = 0;
    uint64_t fEpoch = 1;
};

// Items of T in dense checked slots found through an Index; addresses hold until the next reserve.
template <typename K, typename T> class SlotMap {
public:
    uint32_t size() const { return fIndex.size(); }
    T* find(K key) {
        const uint32_t* at = fIndex.find(key);
        return at ? &fItems[*at] : nullptr;
    }
    const T* find(K key) const {
        const uint32_t* at = fIndex.find(key);
        return at ? &fItems[*at] : nullptr;
    }
    // Room for `n` items, so insert and erase never allocate.
    bool reserve(Allocator& alloc, uint32_t n) {
        return fIndex.reserve(alloc, n) && fItems.reserve(alloc, n) && fFree.reserve(alloc, fItems.capacity());
    }
    // Within reserved room; the item starts as T().
    T& insert(K key) {
        uint32_t slot;
        if (fFree.size()) {
            slot = fFree.back();
            fFree.pop();
        } else {
            slot = fItems.size();
            fItems.push(T());
        }
        fIndex.insert(key) = slot;
        fItems[slot] = T();
        return fItems[slot];
    }
    // The caller has freed what the item owns.
    void erase(K key) {
        const uint32_t slot = *fIndex.find(key);
        fIndex.erase(key);
        fItems[slot] = T();
        fFree.push(slot);
    }
    // Every slot, free ones holding T().
    template <typename F> void forEachSlot(F f) {
        for (uint32_t i = 0; i < fItems.size(); i++) f(fItems[i]);
    }

private:
    Index<K> fIndex;
    Buffer<T> fItems;
    Buffer<uint32_t> fFree;
};

// Entries at stable addresses: fixed-size chunks that never move, so a reference survives later growth.
template <typename T> class Chunks {
    static_assert(std::is_trivially_copyable<T>::value, "chunks hold plain entries");
    static constexpr uint32_t kShift = 10, kChunk = 1u << kShift;

public:
    Chunks() = default;
    Chunks(const Chunks&) = delete;
    Chunks& operator=(const Chunks&) = delete;
    ~Chunks() {
        for (uint32_t i = 0; i < fChunks.size(); i++) std::free(fChunks[i]);
    }

    uint32_t size() const { return fSize; }
    T& operator[](uint32_t i) { return fChunks[i >> kShift][i & (kChunk - 1)]; }
    const T& operator[](uint32_t i) const { return fChunks[i >> kShift][i & (kChunk - 1)]; }

    // Null when an allocation fails.
    T* push(Allocator& alloc, const T& value) {
        if (fSize == uint64_t(fChunks.size()) * kChunk) {
            if (!fChunks.reserve(alloc, fChunks.size() + 1)) return nullptr;
            T* chunk = static_cast<T*>(alloc.allocate(sizeof(T) * kChunk));
            if (!chunk) return nullptr;
            fChunks.push(chunk);
        }
        T* at = &(*this)[fSize++];
        new (at) T(value);
        return at;
    }
    void clear() { fSize = 0; }

private:
    Buffer<T*> fChunks;
    uint32_t fSize = 0;
};

// Staged counts by key (picture references, path generations per handle): dense entries found through an Index.
template <typename K> class Counts {
public:
    struct Entry {
        K key;
        uint32_t count;
    };

    uint32_t size() const { return fEntries.size(); }
    const Entry& operator[](uint32_t i) const { return fEntries[i]; }
    const uint32_t* find(K key) const {
        const uint32_t* at = fIndex.find(key);
        return at ? &fEntries[*at].count : nullptr;
    }
    bool set(Allocator& alloc, K key, uint32_t count) {
        if (const uint32_t* at = fIndex.find(key)) {
            fEntries[*at].count = count;
            return true;
        }
        if (!fIndex.reserve(alloc, fIndex.size() + 1) || !fEntries.reserve(alloc, fEntries.size() + 1)) return false;
        fIndex.insert(key) = fEntries.size();
        fEntries.push({key, count});
        return true;
    }
    void clear() {
        fIndex.clear();
        fEntries.clear();
    }
    Index<K>& index() { return fIndex; }

private:
    Index<K> fIndex;
    Buffer<Entry> fEntries;
};

// ---- the plan's nodes and registrations --------------------------------------------------------------------------

struct HeightCount {
    uint32_t height;
    uint32_t count;
};

// Children per subtree height, ascending by height; its storage is freed by whoever owns the structure.
struct HeightList {
    HeightCount* data = nullptr;
    uint32_t size = 0, cap = 0;
};

// What pass 1 stages per node: topology, subtree height and the registrations the node references.
struct Structure {
    Kind kind = Kind::kGroup;
    bool bypass = false;
    uint32_t parent = kNone, first = kNone, last = kNone, prev = kNone, next = kNone;
    uint32_t height = 0;  // edges to the deepest descendant
    HeightList childHeights;
    Key picture = kNoKey;
    Key clipPath = kNoKey;
};

enum class ClipKind : uint8_t { kNone = 0, kRect = 1, kRRect = 2, kPath = 3 };
enum class ContainKind : uint8_t { kNone = 0, kPaint = 1, kLayout = 2, kStrict = 3 };

// SetIsolation flag bits (wire bits 0, 1, 3; bit 2 clears and is never stored).
constexpr uint16_t kIsolationInitWithPrevious = 1 << 0;
constexpr uint16_t kIsolationLinearF16 = 1 << 1;
constexpr uint16_t kIsolationClear = 1 << 2;
constexpr uint16_t kIsolationFold = 1 << 3;

// A path clip names its path by Structure::clipPath.
struct ClipParams {
    ClipKind kind = ClipKind::kNone;
    bool antialias = false;
    bool soft = false;  // bypass children replay before the clip
    float floats[12] = {};
};

struct IsolationParams {
    bool set = false;
    float alpha = 1;
    float bounds[4] = {};  // all NaN: derive
    SkBlendMode blend = SkBlendMode::kSrcOver;
    uint16_t flags = 0;
};

struct LiveParams {
    bool set = false;
    uint32_t producer = 0;
    uint32_t inputsGen = 0;
    uint16_t flags = 0;  // bit 0 unbounded, bit 1 device-dependent, bit 2 clock-bound
};

struct Params {
    bool visible = true;
    bool hasMatrix = false;
    SkMatrix matrix;
    float matrixFloats[16] = {};
    bool hasM44 = false;
    SkM44 matrix44;
    bool hasSpace = false;
    float space[18] = {};  // flatten, cull backface, row-major child perspective
    ClipParams clip;
    IsolationParams isolation;
    const SkPicture* picture = nullptr;  // owned by its registration, which cannot release while a node names it
    bool pictureDeviceDependent = false;
    LiveParams live;
    bool hasOutset = false;
    float outset[4] = {};
    ContainKind contain = ContainKind::kNone;
    float containBounds[4] = {};
};

// Conservative local bounds: a rect (possibly empty), or unbounded.
struct Bounds {
    SkRect rect = SkRect::MakeEmpty();
    bool unbounded = false;

    static Bounds Unbounded() { return {SkRect::MakeEmpty(), true}; }
    void join(const Bounds& other);
    void outset(const float reach[4]);  // l, t, r, b
    void intersect(const SkRect& clip);
};

// Per-node signed changes to the subtree counts an edit carries up its ancestry.
struct Tally {
    int32_t deviceDependent = 0;
    int32_t clockBound = 0;

    Tally operator-() const { return {-deviceDependent, -clockBound}; }
    Tally operator-(const Tally& o) const { return {deviceDependent - o.deviceDependent, clockBound - o.clockBound}; }
};

// What native derives per node and keeps incrementally (§4.4 output validity): maintained by one O(depth) ancestor walk
// per edit; content bounds are invalidated by that walk and recomputed lazily from the children's cached bounds.
struct Derived {
    uint64_t subtreeGen = 0;
    uint32_t deviceDependent = 0;  // device-dependent pictures and live nodes in the subtree, self included
    uint32_t clockBound = 0;       // clock-bound live nodes in the subtree, self included
    bool boundsDirty = true;
    Bounds content;  // picture, children, clip, outset and contain, in the node's local space; before its isolation
};

// A raster substitution: the subtree replays as this image, drawn into `dst` in the node's local space.
struct Raster {
    Key image = kNoKey;
    SkRect dst = SkRect::MakeEmpty();
};

struct Node {
    Structure structure;
    Params params;
    Derived derived;
    Raster raster;
};

struct PictureEntry {
    sk_sp<SkPicture> picture;
    uint32_t refs = 0;
};

// A defined path's encoding in checked memory: points and conic weights (floats), then verbs (bytes).
struct PathData {
    void* bytes = nullptr;
    uint32_t verbs = 0, points = 0, weights = 0;
    uint8_t fillType = 0;
};

// The SkPath is built at its first replay: replay may allocate, apply may not.
struct PathEntry {
    PathData data;
    uint32_t refs = 0;
    SkPath* path = nullptr;
};

struct ImageEntry {
    sk_sp<SkImage> image;
    uint32_t refs = 0;  // raster substitutions naming it
};

// The committed state of one instance.
struct PlanState {
    SlotMap<uint32_t, Node> nodes;
    SlotMap<Key, PathEntry> paths;
    Index<uint32_t> pathHandles;  // defined generations per path handle
    std::unordered_map<Key, PictureEntry> pictures;
    std::unordered_map<Key, ImageEntry> images;
    // Registered generations per handle, so a lookup can tell an unknown handle from a wrong generation.
    std::unordered_map<uint32_t, uint32_t> pictureHandles, imageHandles;
};

// Pass 1's copy-on-write view of the nodes and registrations one packet touches.
struct StagedNode {
    uint32_t id;
    bool exists;      // as the records read so far leave it
    bool live;        // committed when the packet began
    uint32_t born;    // the record of its latest Insert in this packet, or kNone
    Structure structure;
    Derived derived;
    Tally own;
    bool rasterDirty = false;  // content damage only; composite upkeep still advances Derived
};

struct StagedPath {
    Key key;
    bool defined;
    bool live;
    bool fresh;  // defined by this packet: `data` is the new encoding
    uint32_t refs;
    PathData data;
};

struct ClearedRaster { uint32_t node; Key image; };

struct StagedState {
    Buffer<ClearedRaster> clearedRasters;
    Index<uint32_t> nodeIndex;  // id → entry
    Chunks<StagedNode> nodes;
    Index<Key> pathIndex;
    Chunks<StagedPath> paths;
    Counts<Key> pictureRefs;
    Counts<uint32_t> pathHandles;
    Buffer<uint32_t> stack;  // Remove's subtree walk
    uint32_t nodeCount = 0;
    uint32_t pathCount = 0;
    uint64_t generation = 0;
    uint32_t upkeepVisits = 0;

    // Frees what the touched entries own and forgets them; returns the entries visited, never capacity.
    uint32_t clear();
};

struct Counters {
    uint64_t applied = 0, rejected = 0;
    uint32_t lastRecords = 0;
    uint32_t lastValidated = 0;      // pass 1 visits: records, ancestor steps and removed nodes
    uint32_t lastHeightUpdates = 0;  // pass 1 height propagation steps along affected ancestries
    uint32_t lastAllocations = 0;    // pass 1 checked allocations
    uint32_t lastCommitAllocations = 0;  // pass 2 checked allocations: always 0
    uint32_t lastCleanup = 0;        // staged entries freed after the apply
    uint32_t lastUpkeepVisits = 0;
    uint64_t rastersDrawn = 0, rastersFellBack = 0, boundsComputed = 0;
    uint64_t replays = 0, visited = 0, picturesDrawn = 0, layers = 0, clips = 0;
    uint64_t livePauses = 0, liveUnbalanced = 0;  // live slots handed to JS; of them, left with excess saves
};

// The target a replay draws for (§4.4 ReplayTarget). colorSpace is the wrapper's tag for the space's name.
enum class TargetColorType : uint32_t { kN32 = 0, kF16Linear = 1 };
struct ReplayTarget {
    TargetColorType colorType = TargetColorType::kN32;
    uint32_t colorSpace = 0;
    uint32_t sampleCount = 0;
    uint32_t pathSampleCount = 0;
    float outputScale = 1;
};

// What a live producer reads at a pause (§4.4 LivePaintContext): one slot per open token, read by JS in place.
// The layout is ABI: 4-byte little-endian words, mirrored by LIVE_CONTEXT in canvaskit-variant.ts.
struct LiveContext {
    uint32_t node, producer, inputsRev;
    float matrix[9];      // local-to-device at the pause, SkMatrix order
    float deviceClip[4];  // conservative device clip bounds l, t, r, b
    float foldedAlpha;
    uint32_t clipBypass;
    uint32_t containKind;  // ContainKind; 0 = none
    float containBounds[4];
    ReplayTarget target;
    int32_t baseSaveCount;
};
static_assert(sizeof(LiveContext) == 29 * 4, "LiveContext is a 29-word ABI view");

// Every legal nesting opens each token below the node its enclosing token is paused at, so plan depth bounds it.
constexpr uint32_t kMaxOpenTokens = kMaxDepth + 1;

class PlanInstance {
public:
    explicit PlanInstance(uint32_t id);
    ~PlanInstance();
    PlanInstance(const PlanInstance&) = delete;
    PlanInstance& operator=(const PlanInstance&) = delete;

    uint32_t id() const { return fId; }
    uint32_t applied() const { return fApplied; }

    // A writable buffer for the next packet; nullptr beyond kMaxPacketBytes. Valid until the next call.
    uint8_t* packetBuffer(size_t bytes);
    // Validates the packet in the buffer (pass 1), then commits it (pass 2); a rejected packet changes nothing.
    ApplyResult apply(size_t bytes);

    Error registerPicture(uint32_t handle, uint32_t gen, sk_sp<SkPicture> picture);
    Error releasePicture(uint32_t handle, uint32_t gen);
    Error registerImage(uint32_t handle, uint32_t gen, sk_sp<SkImage> image);
    Error releaseImage(uint32_t handle, uint32_t gen);

    // Raster substitution (§4.4, main-local): `node`'s subtree replays as the image until an edit reaches it or its
    // subtree. The image holds the subtree before the node's own isolation; a layer reaching past its content
    // (init-with-previous, an extent-wide blend) or a linear F16 layer is refused.
    Error setRaster(uint32_t node, uint32_t handle, uint32_t gen, const SkRect& dst, double expectedSubtreeGen = -1);
    Error clearRaster(uint32_t node);
    const Buffer<ClearedRaster>& clearedRasters() const { return fClearedRasters; }
    void clearRasterReport() { fClearedRasters.clear(); }
    // The node's conservative bounds in its local space (before its own matrix, its isolation applied); lazy.
    Bounds bounds(uint32_t node);

    // Replay of `root`'s subtree onto `canvas`. A token is positive; a failure is the negated Error.
    int32_t beginReplay(SkCanvas* canvas, uint32_t root, const ReplayTarget& target, bool contentOnly = false);
    // Replay of a paused live node's children in its local space; only legal while the innermost token is paused there.
    int32_t beginReplayChildren(SkCanvas* canvas, uint32_t live, float alpha);
    // 0 when the walk is complete, 1 when paused at a live node (its context in slot openTokens() - 1), else -Error.
    int32_t step(int32_t token);
    // Ends the innermost token and restores its canvas to the save count it began at.
    Error endReplay(int32_t token);
    uint32_t openTokens() const { return uint32_t(fTokens.size()); }
    // kMaxOpenTokens context slots, indexed by token depth; the address is fixed for the instance's life.
    const LiveContext* liveContexts() const { return fContexts.data(); }

    const Node* find(uint32_t id) const { return fState.nodes.find(id); }
    const PlanState& state() const { return fState; }
    const Counters& counters() const { return fCounters; }

    // Test-only: the n-th next checked allocation fails (0 disarms), and the sequence the next packet's base must equal.
    void setAllocFailureForTesting(uint32_t n) { fAlloc.failAt = n ? fAlloc.calls + n : 0; }
    void setSequenceForTesting(uint32_t applied) { fApplied = applied; }
    // Test-only: the four staged indexes' least epoch, and a jump of all four forward as if that many packets had run.
    uint64_t stagedEpochForTesting() {
        return std::min({fStaged.nodeIndex.epoch(), fStaged.pathIndex.epoch(), fStaged.pictureRefs.index().epoch(),
                         fStaged.pathHandles.index().epoch()});
    }
    void setStagedEpochForTesting(uint64_t epoch) {
        fStaged.nodeIndex.setEpochForTesting(epoch);
        fStaged.pathIndex.setEpochForTesting(epoch);
        fStaged.pictureRefs.index().setEpochForTesting(epoch);
        fStaged.pathHandles.index().setEpochForTesting(epoch);
    }

private:
    enum class Stage : uint8_t { kEnter, kBypassChildren, kContent, kChildren, kExit };
    // What a node's own params leave in force for its content and children: X3 replayGroup's folded alpha and contain.
    struct Scope {
        bool antialiasedClip = false;
        float foldedAlpha = 1;
        const Node* contain = nullptr;  // nearest node at or above with SetContain
    };
    struct Frame {
        Node* node;
        uint32_t id;
        uint32_t child;
        int saveCount;
        Stage stage;
        Scope scope;
        bool softClip;    // bypass children replay before the clip
        bool clipBypass;  // this node is a bypass child replayed before its parent's soft clip
        bool contentOnly = false;
    };
    struct Token {
        int32_t id;
        SkCanvas* canvas;
        int baseSaveCount;
        ReplayTarget target;
        int pauseSaveCount;  // 0 while running; the protected depth while paused at a live node
        std::vector<Frame> frames;
    };

    ApplyResult validate(const uint8_t* packet, uint32_t records, uint32_t floats);
    void commit(const uint8_t* packet, uint32_t records, uint32_t floats);
    const Bounds& contentBounds(Node& node);
    Bounds ownBounds(Node& node);
    void dirtyBounds(uint32_t id);
    // A layer's bounds for saveLayer: the hint, else the derived content bounds; none keeps the clip's extent.
    std::optional<SkRect> layerBounds(Node& node);
    void drawRaster(SkCanvas* canvas, const Node& node);
    Error canOpenToken(SkCanvas* canvas) const;
    Token& openToken(SkCanvas* canvas, const ReplayTarget& target);
    void pushNode(Token& token, uint32_t id, Scope scope, bool clipBypass);
    int32_t pause(Token& token, const Frame& frame);
    Error resume(Token& token);
    const SkPath& clipPath(Key key);

    const uint32_t fId;
    uint32_t fApplied = 0;  // the sequence the next packet's base must equal
    uint64_t fGeneration = 0;
    int32_t fNextToken = 1;
    Allocator fAlloc;
    PlanState fState;
    StagedState fStaged;
    Buffer<ClearedRaster> fClearedRasters;
    struct BoundsWork { Node* node; bool ready; };
    Buffer<BoundsWork> fBoundsWork;
    std::vector<uint8_t> fPacket;
    std::vector<Token> fTokens;
    std::vector<LiveContext> fContexts = std::vector<LiveContext>(kMaxOpenTokens);
    // An under-restore damaged canvas state no step can rebuild: every token refuses until all have ended.
    bool fUnderRestore = false;
    Counters fCounters;
};

}  // namespace cfplan

#endif  // CompositionPlan_DEFINED
