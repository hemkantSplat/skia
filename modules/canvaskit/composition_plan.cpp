/*
 * Plan instance: packet validation against a staged topology (pass 1), then commit (pass 2), and non-live replay.
 * Pass 1 never writes the committed state and makes every allocation the packet needs, each checked, so a rejected
 * packet (Alloc included) changes nothing; pass 2 only moves pass 1's results into place and never allocates.
 */
#include "modules/canvaskit/composition_plan.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathTypes.h"

#include <cmath>
#include <iterator>

namespace cfplan {
namespace {

// ---- wire --------------------------------------------------------------------------------------------------------

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

struct Record {
    uint16_t op, flags;
    uint32_t node, a, b;
};

Record ReadRecord(const uint8_t* packet, uint32_t index) {
    const uint8_t* p = packet + kHeaderBytes + kRecordBytes * index;
    return {ReadU16(p), ReadU16(p + 2), ReadU32(p + 4), ReadU32(p + 8), ReadU32(p + 12)};
}

// The f32 pool. True floats and u32 bit patterns (path encodings, a clip's path generation) share it.
class Pool {
public:
    Pool(const uint8_t* base, uint32_t count) : fBase(base), fCount(count) {}

    bool has(uint32_t offset, uint64_t n) const { return offset <= fCount && n <= uint64_t(fCount - offset); }
    float f(uint32_t i) const {
        float v;
        std::memcpy(&v, fBase + 4 * size_t(i), sizeof v);
        return v;
    }
    uint32_t u(uint32_t i) const { return ReadU32(fBase + 4 * size_t(i)); }
    bool finite(uint32_t offset, uint32_t n) const {
        for (uint32_t i = 0; i < n; i++) {
            if (!std::isfinite(f(offset + i))) return false;
        }
        return true;
    }
    void copy(uint32_t offset, uint32_t n, void* out) const { std::memcpy(out, fBase + 4 * size_t(offset), 4 * size_t(n)); }

private:
    const uint8_t* fBase;
    uint32_t fCount;
};

constexpr uint16_t kInsertKindMask = 0xFF, kInsertBypass = 1 << 8;
constexpr uint16_t kClipKindMask = 0xFF, kClipAntialias = 1 << 8, kClipSoft = 1 << 9;
constexpr uint16_t kPictureDeviceDependent = 1 << 0;
constexpr uint16_t kLiveFlags = 0x7;

// ---- payload decoders: one validation for both passes; `out` is null in pass 1 ------------------------------------

Error DecodeFloats(const Pool& pool, uint32_t a, uint32_t n, float* out) {
    if (!pool.has(a, n) || !pool.finite(a, n)) return Error::kBadPayload;
    if (out) {
        for (uint32_t i = 0; i < n; i++) out[i] = pool.f(a + i);
    }
    return Error::kOk;
}

Error DecodeOutset(const Pool& pool, uint32_t a, float* out) {
    float reach[4];
    if (DecodeFloats(pool, a, 4, reach) != Error::kOk) return Error::kBadPayload;
    for (float r : reach) {
        if (r < 0) return Error::kBadPayload;
    }
    if (out) std::memcpy(out, reach, sizeof reach);
    return Error::kOk;
}

Error DecodeClip(const Record& r, const Pool& pool, ClipParams* out, Key* pathKey) {
    if (r.flags & ~(kClipKindMask | kClipAntialias | kClipSoft)) return Error::kBadPayload;
    const auto kind = ClipKind(r.flags & kClipKindMask);
    *pathKey = kNoKey;
    ClipParams clip;
    clip.kind = kind;
    clip.antialias = r.flags & kClipAntialias;
    clip.soft = r.flags & kClipSoft;
    switch (kind) {
        case ClipKind::kNone:
            if (r.flags != 0 || r.a != 0 || r.b != 0) return Error::kBadPayload;
            break;
        case ClipKind::kRect:
        case ClipKind::kRRect:
            if (r.b != 0) return Error::kBadPayload;
            if (DecodeFloats(pool, r.a, kind == ClipKind::kRect ? 4 : 12, clip.floats) != Error::kOk) {
                return Error::kBadPayload;
            }
            break;
        case ClipKind::kPath: {
            if (!pool.has(r.a, 1)) return Error::kBadPayload;
            const uint32_t gen = pool.u(r.a);
            if (gen == kNone) return Error::kLimit;
            *pathKey = MakeKey(r.b, gen);
            break;
        }
        default:
            return Error::kBadPayload;
    }
    if (out) *out = clip;
    return Error::kOk;
}

Error DecodeIsolation(const Record& r, const Pool& pool, IsolationParams* out) {
    if (r.flags & kIsolationClear) {
        if (r.flags != kIsolationClear || r.a != 0 || r.b != 0) return Error::kBadPayload;
        if (out) *out = IsolationParams();
        return Error::kOk;
    }
    constexpr uint16_t kStored = kIsolationInitWithPrevious | kIsolationLinearF16 | kIsolationFold;
    if (r.flags & ~kStored) return Error::kBadPayload;
    if (r.b > uint32_t(SkBlendMode::kLastMode) || !pool.has(r.a, 5)) return Error::kBadPayload;
    IsolationParams iso;
    iso.set = true;
    iso.alpha = pool.f(r.a);
    iso.blend = SkBlendMode(r.b);
    iso.flags = r.flags;
    if (!(iso.alpha >= 0 && iso.alpha <= 1)) return Error::kBadPayload;
    // Bounds hint: all four finite, or all four NaN (derive).
    int nan = 0;
    for (int i = 0; i < 4; i++) {
        iso.bounds[i] = pool.f(r.a + 1 + i);
        if (std::isnan(iso.bounds[i])) nan++;
        else if (!std::isfinite(iso.bounds[i])) return Error::kBadPayload;
    }
    if (nan != 0 && nan != 4) return Error::kBadPayload;
    // A fold opens no layer: only alpha multiplies, so it takes no layer flags and no blend but SrcOver.
    if ((iso.flags & kIsolationFold) &&
        ((iso.flags & ~kIsolationFold) || iso.blend != SkBlendMode::kSrcOver)) {
        return Error::kBadPayload;
    }
    if (out) *out = iso;
    return Error::kOk;
}

Error DecodeContain(const Record& r, const Pool& pool, ContainKind* kind, float* bounds) {
    if (r.flags > uint16_t(ContainKind::kStrict) || r.b != 0) return Error::kBadPayload;
    const auto k = ContainKind(r.flags);
    if (k == ContainKind::kNone) {
        if (r.a != 0) return Error::kBadPayload;
    } else if (DecodeFloats(pool, r.a, 4, bounds) != Error::kOk) {
        return Error::kBadPayload;
    }
    if (kind) *kind = k;
    return Error::kOk;
}

// DefinePath's pool layout: gen, fillType, verbCount, pointCount, weightCount (u32), verbs (u32), points, weights.
struct PathLayout {
    uint32_t gen, fillType, verbs, points, weights;
};

Error DecodePath(const Pool& pool, uint32_t a, PathLayout* layout) {
    if (!pool.has(a, 5)) return Error::kBadPayload;
    PathLayout l{pool.u(a), pool.u(a + 1), pool.u(a + 2), pool.u(a + 3), pool.u(a + 4)};
    if (l.gen == kNone) return Error::kLimit;
    if (l.fillType > uint32_t(SkPathFillType::kInverseEvenOdd)) return Error::kBadPayload;
    if (!pool.has(a, 5 + uint64_t(l.verbs) + 2 * uint64_t(l.points) + l.weights)) return Error::kBadPayload;
    const uint32_t verbs = a + 5, points = verbs + l.verbs;
    uint64_t needPoints = 0, needWeights = 0;
    for (uint32_t i = 0; i < l.verbs; i++) {
        static constexpr uint8_t kPointsPerVerb[] = {1, 1, 2, 2, 3, 0};
        const uint32_t verb = pool.u(verbs + i);
        if (verb > 5 || (i == 0 && verb != 0)) return Error::kBadPayload;
        needPoints += kPointsPerVerb[verb];
        needWeights += verb == 3;
    }
    if (needPoints != l.points || needWeights != l.weights) return Error::kBadPayload;
    if (!pool.finite(points, 2 * l.points + l.weights)) return Error::kBadPayload;
    *layout = l;
    return Error::kOk;
}

// Copies a validated encoding into checked memory; false when the allocation fails.
bool StorePath(Allocator& alloc, const Pool& pool, uint32_t a, const PathLayout& l, PathData* out) {
    const size_t floats = 2 * size_t(l.points) + l.weights, bytes = 4 * floats + l.verbs;
    auto* block = static_cast<uint8_t*>(alloc.allocate(std::max<size_t>(bytes, 1)));
    if (!block) return false;
    const uint32_t verbs = a + 5;
    pool.copy(verbs + l.verbs, uint32_t(floats), block);
    for (uint32_t i = 0; i < l.verbs; i++) block[4 * floats + i] = uint8_t(pool.u(verbs + i));
    *out = {block, l.verbs, l.points, l.weights, uint8_t(l.fillType)};
    return true;
}

void FreePathData(PathData& data) {
    std::free(data.bytes);
    data = PathData();
}

SkPath BuildPath(const PathData& d) {
    const auto* floats = static_cast<const float*>(d.bytes);
    const auto* verbs = static_cast<const uint8_t*>(d.bytes) + 4 * (2 * size_t(d.points) + d.weights);
    const float* weights = floats + 2 * size_t(d.points);
    SkPath path;
    path.setFillType(SkPathFillType(d.fillType));
    path.incReserve(int(d.points));
    auto pt = [&](size_t i) { return SkPoint::Make(floats[2 * i], floats[2 * i + 1]); };
    size_t p = 0;
    for (uint32_t i = 0; i < d.verbs; i++) {
        switch (verbs[i]) {
            case 0: path.moveTo(pt(p)); p += 1; break;
            case 1: path.lineTo(pt(p)); p += 1; break;
            case 2: path.quadTo(pt(p), pt(p + 1)); p += 2; break;
            case 3: path.conicTo(pt(p), pt(p + 1), *weights++); p += 2; break;
            case 4: path.cubicTo(pt(p), pt(p + 1), pt(p + 2)); p += 3; break;
            case 5: path.close(); break;
        }
    }
    return path;
}

// ---- subtree heights ---------------------------------------------------------------------------------------------

uint32_t LowerBound(const HeightList& l, uint32_t height) {
    return uint32_t(std::lower_bound(l.data, l.data + l.size, height,
                                     [](const HeightCount& c, uint32_t h) { return c.height < h; }) - l.data);
}

bool AddChildHeight(Allocator& alloc, HeightList& l, uint32_t height) {
    const uint32_t i = LowerBound(l, height);
    if (i < l.size && l.data[i].height == height) {
        l.data[i].count++;
        return true;
    }
    if (l.size == l.cap) {
        const uint32_t cap = std::max<uint32_t>(4, 2 * l.cap);
        auto* data = static_cast<HeightCount*>(alloc.allocate(sizeof(HeightCount) * size_t(cap)));
        if (!data) return false;
        if (l.size) std::memcpy(data, l.data, sizeof(HeightCount) * l.size);
        std::free(l.data);
        l.data = data;
        l.cap = cap;
    }
    std::memmove(l.data + i + 1, l.data + i, sizeof(HeightCount) * (l.size - i));
    l.data[i] = {height, 1};
    l.size++;
    return true;
}

void RemoveChildHeight(HeightList& l, uint32_t height) {
    const uint32_t i = LowerBound(l, height);
    if (--l.data[i].count == 0) {
        std::memmove(l.data + i, l.data + i + 1, sizeof(HeightCount) * (l.size - i - 1));
        l.size--;
    }
}

uint32_t HeightFromChildren(const HeightList& l) { return l.size ? l.data[l.size - 1].height + 1 : 0; }

bool CopyHeights(Allocator& alloc, const HeightList& from, HeightList* to) {
    *to = HeightList();
    if (!from.size) return true;
    auto* data = static_cast<HeightCount*>(alloc.allocate(sizeof(HeightCount) * size_t(from.size)));
    if (!data) return false;
    std::memcpy(data, from.data, sizeof(HeightCount) * from.size);
    *to = {data, from.size, from.size};
    return true;
}

void FreeHeights(HeightList& l) {
    std::free(l.data);
    l = HeightList();
}

// ---- pass 1's copy-on-write overlay over the committed state; every allocation is checked -----------------------

class StagedStore {
public:
    StagedStore(const PlanState& live, StagedState& staged, Allocator& alloc)
            : fLive(live), fStaged(staged), fAlloc(alloc) {}

    Allocator& alloc() { return fAlloc; }

    const Structure* find(uint32_t id) const {
        if (const uint32_t* at = fStaged.nodeIndex.find(id)) {
            const StagedNode& e = fStaged.nodes[*at];
            return e.exists ? &e.structure : nullptr;
        }
        const Node* node = fLive.nodes.find(id);
        return node ? &node->structure : nullptr;
    }
    // The staged copy of an existing node, made on first touch; null when an allocation fails.
    Structure* edit(uint32_t id) {
        StagedNode* e = touch(id, true);
        return e ? &e->structure : nullptr;
    }
    bool create(uint32_t id, Kind kind, bool bypass, uint32_t record) {
        StagedNode* e = touch(id, false);
        if (!e) return false;
        FreeHeights(e->structure.childHeights);
        e->structure = Structure();
        e->structure.kind = kind;
        e->structure.bypass = bypass;
        e->exists = true;
        e->born = record;
        fStaged.nodeCount++;
        return true;
    }
    bool destroy(uint32_t id) {
        StagedNode* e = touch(id, false);
        if (!e) return false;
        FreeHeights(e->structure.childHeights);
        e->structure = Structure();
        e->exists = false;
        fStaged.nodeCount--;
        return true;
    }

    uint32_t pictureRefs(Key key) const {
        const uint32_t* refs = fStaged.pictureRefs.find(key);
        return refs ? *refs : fLive.pictures.find(key)->second.refs;
    }
    bool retainPicture(Key key) { return fStaged.pictureRefs.set(fAlloc, key, pictureRefs(key) + 1); }
    bool releasePicture(Key key) { return fStaged.pictureRefs.set(fAlloc, key, pictureRefs(key) - 1); }

    struct PathView {
        bool defined;
        uint32_t refs;
    };
    PathView path(Key key) const {
        if (const uint32_t* at = fStaged.pathIndex.find(key)) {
            const StagedPath& p = fStaged.paths[*at];
            return {p.defined, p.refs};
        }
        const PathEntry* live = fLive.paths.find(key);
        return {live != nullptr, live ? live->refs : 0};
    }
    uint32_t pathGenerations(uint32_t handle) const {
        if (const uint32_t* gens = fStaged.pathHandles.find(handle)) return *gens;
        const uint32_t* live = fLive.pathHandles.find(handle);
        return live ? *live : 0;
    }
    uint32_t pathCount() const { return fStaged.pathCount; }
    bool retainPath(Key key) {
        StagedPath* p = touchPath(key);
        if (!p) return false;
        p->refs++;
        return true;
    }
    bool releasePath(Key key) {
        StagedPath* p = touchPath(key);
        if (!p) return false;
        p->refs--;
        return true;
    }
    bool definePath(Key key, const Pool& pool, uint32_t a, const PathLayout& layout) {
        StagedPath* p = touchPath(key);
        if (!p || !fStaged.pathHandles.set(fAlloc, KeyHandle(key), pathGenerations(KeyHandle(key)) + 1)) return false;
        FreePathData(p->data);
        if (!StorePath(fAlloc, pool, a, layout, &p->data)) return false;
        p->defined = p->fresh = true;
        p->refs = 0;
        fStaged.pathCount++;
        return true;
    }
    bool undefinePath(Key key) {
        StagedPath* p = touchPath(key);
        if (!p || !fStaged.pathHandles.set(fAlloc, KeyHandle(key), pathGenerations(KeyHandle(key)) - 1)) return false;
        FreePathData(p->data);
        p->defined = p->fresh = false;
        p->refs = 0;
        fStaged.pathCount--;
        return true;
    }

private:
    StagedNode* touch(uint32_t id, bool copyHeights) {
        if (const uint32_t* at = fStaged.nodeIndex.find(id)) return &fStaged.nodes[*at];
        if (!fStaged.nodeIndex.reserve(fAlloc, fStaged.nodeIndex.size() + 1)) return nullptr;
        StagedNode entry{id, false, false, kNone, Structure()};
        if (const Node* node = fLive.nodes.find(id)) {
            entry.exists = entry.live = true;
            entry.structure = node->structure;
            entry.structure.childHeights = HeightList();
            if (copyHeights && !CopyHeights(fAlloc, node->structure.childHeights, &entry.structure.childHeights)) {
                return nullptr;
            }
        }
        StagedNode* e = fStaged.nodes.push(fAlloc, entry);
        if (!e) {
            FreeHeights(entry.structure.childHeights);
            return nullptr;
        }
        fStaged.nodeIndex.insert(id) = fStaged.nodes.size() - 1;
        return e;
    }
    StagedPath* touchPath(Key key) {
        if (const uint32_t* at = fStaged.pathIndex.find(key)) return &fStaged.paths[*at];
        if (!fStaged.pathIndex.reserve(fAlloc, fStaged.pathIndex.size() + 1)) return nullptr;
        const PathEntry* live = fLive.paths.find(key);
        StagedPath* p = fStaged.paths.push(fAlloc, {key, live != nullptr, live != nullptr, false, live ? live->refs : 0, PathData()});
        if (!p) return nullptr;
        fStaged.pathIndex.insert(key) = fStaged.paths.size() - 1;
        return p;
    }

    const PlanState& fLive;
    StagedState& fStaged;
    Allocator& fAlloc;
};

// ---- topology: every structural edit, on the overlay; false when an allocation fails -----------------------------

class Topology {
public:
    Topology(StagedStore& store, Buffer<uint32_t>& stack, uint32_t* heightUpdates, uint32_t* removed)
            : fStore(store), fStack(stack), fHeightUpdates(heightUpdates), fRemoved(removed) {}

    // Links a detached node before `before` (a child of `parent`), or last when `before` is kNone.
    bool attach(uint32_t id, uint32_t parent, uint32_t before) {
        Structure* c = fStore.edit(id);
        Structure* p = c ? fStore.edit(parent) : nullptr;
        if (!p) return false;
        const uint32_t prev = before == kNone ? p->last : fStore.find(before)->prev;
        c->parent = parent;
        c->prev = prev;
        c->next = before;
        if (prev == kNone) p->first = id;
        else if (Structure* s = fStore.edit(prev)) s->next = id;
        else return false;
        if (before == kNone) p->last = id;
        else if (Structure* s = fStore.edit(before)) s->prev = id;
        else return false;
        return childHeightChanged(parent, kNone, c->height);
    }

    bool detach(uint32_t id) {
        Structure* c = fStore.edit(id);
        Structure* p = c ? fStore.edit(c->parent) : nullptr;
        if (!p) return false;
        const uint32_t parent = c->parent;
        if (c->prev == kNone) p->first = c->next;
        else if (Structure* s = fStore.edit(c->prev)) s->next = c->next;
        else return false;
        if (c->next == kNone) p->last = c->prev;
        else if (Structure* s = fStore.edit(c->next)) s->prev = c->prev;
        else return false;
        c->parent = c->prev = c->next = kNone;
        return childHeightChanged(parent, c->height, kNone);
    }

    // Detaches `id` and destroys its subtree, dropping the registrations each node referenced.
    bool remove(uint32_t id) {
        if (!detach(id)) return false;
        fStack.clear();
        if (!fStack.reserve(fStore.alloc(), 1)) return false;
        fStack.push(id);
        while (fStack.size()) {
            const uint32_t n = fStack.back();
            fStack.pop();
            const Structure& s = *fStore.find(n);
            for (uint32_t c = s.first; c != kNone; c = fStore.find(c)->next) {
                if (!fStack.reserve(fStore.alloc(), fStack.size() + 1)) return false;
                fStack.push(c);
            }
            if (s.picture != kNoKey && !fStore.releasePicture(s.picture)) return false;
            if (s.clipPath != kNoKey && !fStore.releasePath(s.clipPath)) return false;
            if (!fStore.destroy(n)) return false;
            ++*fRemoved;
        }
        return true;
    }

private:
    // A child of `parent` changed height (kNone: absent before or after); heights propagate while they change.
    bool childHeightChanged(uint32_t parent, uint32_t from, uint32_t to) {
        while (parent != kNone) {
            Structure* p = fStore.edit(parent);
            if (!p) return false;
            ++*fHeightUpdates;
            const uint32_t before = p->height;
            if (from != kNone) RemoveChildHeight(p->childHeights, from);
            if (to != kNone && !AddChildHeight(fStore.alloc(), p->childHeights, to)) return false;
            p->height = HeightFromChildren(p->childHeights);
            if (p->height == before) return true;
            from = before;
            to = p->height;
            parent = p->parent;
        }
        return true;
    }

    StagedStore& fStore;
    Buffer<uint32_t>& fStack;
    uint32_t* fHeightUpdates;
    uint32_t* fRemoved;
};

// ---- pass 1: every record checked against the staged topology as it is reached ------------------------------------

class Validator {
public:
    Validator(const PlanState& live, StagedState& staged, Allocator& alloc, const Pool& pool)
            : fLive(live), fStore(live, staged, alloc), fTopology(fStore, staged.stack, &heightUpdates, &validated),
              fPool(pool) {}

    uint32_t validated = 0;
    uint32_t heightUpdates = 0;

    // kAlloc means an allocation failed.
    Error check(const Record& r, uint32_t index) {
        validated++;
        fRecord = index;
        switch (Op(r.op)) {
            case Op::kInsert: return insert(r);
            case Op::kRemove: return remove(r);
            case Op::kMove: return move(r);
            case Op::kSetMatrix:
                return orFirst(existing(r.node), r.flags || r.b ? Error::kBadPayload : DecodeFloats(fPool, r.a, 9, nullptr));
            case Op::kSetClip: return clip(r);
            case Op::kSetIsolation: return orFirst(existing(r.node), DecodeIsolation(r, fPool, nullptr));
            case Op::kSetPicture: return picture(r);
            case Op::kSetLive: return live(r);
            case Op::kSetOutset:
                return orFirst(existing(r.node), r.flags || r.b ? Error::kBadPayload : DecodeOutset(fPool, r.a, nullptr));
            case Op::kDefinePath: return definePath(r);
            case Op::kReleasePath: return releasePath(r);
            case Op::kSetVisible:
                return orFirst(existing(r.node), r.flags || r.b || r.a > 1 ? Error::kBadPayload : Error::kOk);
            case Op::kSetContain: return orFirst(existing(r.node), DecodeContain(r, fPool, nullptr, nullptr));
        }
        return Error::kBadPayload;
    }

    const Structure& root() const { return *fStore.find(kRoot); }

private:
    static Error orFirst(Error first, Error second) { return first != Error::kOk ? first : second; }
    static Error allocated(bool ok) { return ok ? Error::kOk : Error::kAlloc; }

    Error existing(uint32_t node) const {
        if (node == kNone) return Error::kBadPayload;
        return fStore.find(node) ? Error::kOk : Error::kUnknownNode;
    }

    // The node an Insert, Remove or Move names: never the root or the sentinel.
    static Error structural(uint32_t node) {
        if (node == kRoot) return Error::kRootImmutable;
        return node == kNone ? Error::kBadPayload : Error::kOk;
    }

    // `before` must be a child of `parent` in the staged topology.
    Error placement(uint32_t parent, uint32_t before) const {
        if (!fStore.find(parent)) return Error::kBadParent;
        if (before == kNone) return Error::kOk;
        const Structure* b = fStore.find(before);
        return b && b->parent == parent ? Error::kOk : Error::kBadParent;
    }

    Error insert(const Record& r) {
        const uint32_t kind = r.flags & kInsertKindMask;
        if (Error e = structural(r.node); e != Error::kOk) return e;
        if ((r.flags & ~(kInsertKindMask | kInsertBypass)) || kind < 1 || kind > uint32_t(Kind::kLive)) {
            return Error::kBadPayload;
        }
        if (fStore.find(r.node)) return Error::kDuplicateNode;
        if (Error e = placement(r.a, r.b); e != Error::kOk) return e;
        return allocated(fStore.create(r.node, Kind(kind), r.flags & kInsertBypass, fRecord) &&
                         fTopology.attach(r.node, r.a, r.b));
    }

    Error remove(const Record& r) {
        if (Error e = structural(r.node); e != Error::kOk) return e;
        if (r.flags != 0 || r.a != 0 || r.b != 0) return Error::kBadPayload;
        if (!fStore.find(r.node)) return Error::kUnknownNode;
        return allocated(fTopology.remove(r.node));
    }

    Error move(const Record& r) {
        if (Error e = structural(r.node); e != Error::kOk) return e;
        if (r.flags != 0) return Error::kBadPayload;
        if (!fStore.find(r.node)) return Error::kUnknownNode;
        if (!fStore.find(r.a)) return Error::kBadParent;
        // Cycle: the node may not become a descendant of itself.
        for (uint32_t x = r.a; x != kNone; x = fStore.find(x)->parent) {
            validated++;
            if (x == r.node) return Error::kCycle;
        }
        if (r.b == r.node) return Error::kBadParent;
        if (Error e = placement(r.a, r.b); e != Error::kOk) return e;
        return allocated(fTopology.detach(r.node) && fTopology.attach(r.node, r.a, r.b));
    }

    Error clip(const Record& r) {
        if (Error e = existing(r.node); e != Error::kOk) return e;
        Key path;
        if (Error e = DecodeClip(r, fPool, nullptr, &path); e != Error::kOk) return e;
        if (path != kNoKey) {
            if (fStore.pathGenerations(KeyHandle(path)) == 0) return Error::kUnknownPath;
            if (!fStore.path(path).defined) return Error::kBadGeneration;
            if (!fStore.retainPath(path)) return Error::kAlloc;
        }
        Structure* s = fStore.edit(r.node);
        if (!s || (s->clipPath != kNoKey && !fStore.releasePath(s->clipPath))) return Error::kAlloc;
        s->clipPath = path;
        return Error::kOk;
    }

    Error picture(const Record& r) {
        if (Error e = existing(r.node); e != Error::kOk) return e;
        if (fStore.find(r.node)->kind != Kind::kPicture || (r.flags & ~kPictureDeviceDependent)) {
            return Error::kBadPayload;
        }
        if (r.a == kNone || r.b == kNone) return Error::kLimit;
        const Key key = MakeKey(r.a, r.b);
        if (!fLive.pictureHandles.count(r.a)) return Error::kUnknownPicture;
        if (!fLive.pictures.count(key)) return Error::kBadGeneration;
        if (!fStore.retainPicture(key)) return Error::kAlloc;
        Structure* s = fStore.edit(r.node);
        if (!s || (s->picture != kNoKey && !fStore.releasePicture(s->picture))) return Error::kAlloc;
        s->picture = key;
        return Error::kOk;
    }

    Error live(const Record& r) {
        if (Error e = existing(r.node); e != Error::kOk) return e;
        if (fStore.find(r.node)->kind != Kind::kLive || (r.flags & ~kLiveFlags)) return Error::kBadPayload;
        return r.b == kNone ? Error::kLimit : Error::kOk;
    }

    static Error pathHandle(const Record& r) {
        if (r.node == 0 || r.node > kMaxPathHandle || r.flags != 0 || r.b != 0) return Error::kBadPayload;
        return Error::kOk;
    }

    Error definePath(const Record& r) {
        if (Error e = pathHandle(r); e != Error::kOk) return e;
        PathLayout layout;
        if (Error e = DecodePath(fPool, r.a, &layout); e != Error::kOk) return e;
        const Key key = MakeKey(r.node, layout.gen);
        if (fStore.path(key).defined) return Error::kBadGeneration;
        if (fStore.pathCount() >= kMaxPaths) return Error::kLimit;
        return allocated(fStore.definePath(key, fPool, r.a, layout));
    }

    Error releasePath(const Record& r) {
        if (Error e = pathHandle(r); e != Error::kOk) return e;
        const Key key = MakeKey(r.node, r.a);
        if (fStore.pathGenerations(r.node) == 0) return Error::kUnknownPath;
        const StagedStore::PathView state = fStore.path(key);
        if (!state.defined) return Error::kBadGeneration;
        if (state.refs != 0) return Error::kBadPayload;  // still clipping a node
        return allocated(fStore.undefinePath(key));
    }

    const PlanState& fLive;
    StagedStore fStore;
    Topology fTopology;
    const Pool& fPool;
    uint32_t fRecord = 0;
};

// ---- pass 2's parameter writes: a validated record on the node it names --------------------------------------------

void ApplyParams(const PlanState& state, Node& node, const Record& r, const Pool& pool) {
    Params& p = node.params;
    switch (Op(r.op)) {
        case Op::kSetMatrix: {
            DecodeFloats(pool, r.a, 9, p.matrixFloats);
            const float* m = p.matrixFloats;
            p.matrix = SkMatrix::MakeAll(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
            p.hasMatrix = true;
            break;
        }
        case Op::kSetClip: {
            Key path;
            DecodeClip(r, pool, &p.clip, &path);
            break;
        }
        case Op::kSetIsolation: DecodeIsolation(r, pool, &p.isolation); break;
        case Op::kSetPicture:
            p.picture = state.pictures.find(MakeKey(r.a, r.b))->second.picture.get();
            p.pictureDeviceDependent = r.flags & kPictureDeviceDependent;
            break;
        case Op::kSetLive: p.live = {true, r.a, r.b, r.flags}; break;
        case Op::kSetOutset:
            DecodeOutset(pool, r.a, p.outset);
            p.hasOutset = true;
            break;
        case Op::kSetVisible: p.visible = r.a == 1; break;
        case Op::kSetContain: DecodeContain(r, pool, &p.contain, p.containBounds); break;
        default: break;  // structure and paths commit from the staged state
    }
}

void FreePath(PathEntry& entry) {
    FreePathData(entry.data);
    delete entry.path;
    entry.path = nullptr;
}

// ---- replay ------------------------------------------------------------------------------------------------------

void OpenIsolation(SkCanvas* canvas, const IsolationParams& iso, Counters& counters) {
    if (!iso.set || (iso.flags & kIsolationFold)) return;
    SkPaint paint;
    paint.setAlphaf(iso.alpha);
    paint.setBlendMode(iso.blend);
    const SkRect bounds = SkRect::MakeLTRB(iso.bounds[0], iso.bounds[1], iso.bounds[2], iso.bounds[3]);
    SkCanvas::SaveLayerFlags flags = 0;
    if (iso.flags & kIsolationInitWithPrevious) flags |= SkCanvas::kInitWithPrevious_SaveLayerFlag;
    // A linear F16 layer resolves into the destination's space on restore (the fork's linear-light layers).
    sk_sp<SkColorSpace> linear;
    if (iso.flags & kIsolationLinearF16) {
        flags |= SkCanvas::kF16ColorType;
        linear = SkColorSpace::MakeSRGBLinear();
    }
    canvas->saveLayer(SkCanvas::SaveLayerRec(std::isnan(iso.bounds[0]) ? nullptr : &bounds, &paint, nullptr,
                                             linear.get(), flags));
    counters.layers++;
}

void ApplyClip(SkCanvas* canvas, const ClipParams& clip, const SkPath* path, Counters& counters) {
    const float* f = clip.floats;
    const SkRect rect = SkRect::MakeLTRB(f[0], f[1], f[2], f[3]);
    switch (clip.kind) {
        case ClipKind::kNone: return;
        case ClipKind::kRect: canvas->clipRect(rect, SkClipOp::kIntersect, clip.antialias); break;
        case ClipKind::kRRect: {
            const SkVector radii[4] = {{f[4], f[5]}, {f[6], f[7]}, {f[8], f[9]}, {f[10], f[11]}};
            SkRRect rrect;
            rrect.setRectRadii(rect, radii);
            canvas->clipRRect(rrect, SkClipOp::kIntersect, clip.antialias);
            break;
        }
        case ClipKind::kPath: canvas->clipPath(*path, SkClipOp::kIntersect, clip.antialias); break;
    }
    counters.clips++;
}

}  // namespace

uint32_t StagedState::clear() {
    const uint32_t work = nodes.size() + paths.size();
    for (uint32_t i = 0; i < nodes.size(); i++) FreeHeights(nodes[i].structure.childHeights);
    for (uint32_t i = 0; i < paths.size(); i++) FreePathData(paths[i].data);
    nodeIndex.clear();
    nodes.clear();
    pathIndex.clear();
    paths.clear();
    pictureRefs.clear();
    pathHandles.clear();
    stack.clear();
    return work;
}

PlanInstance::PlanInstance(uint32_t id) : fId(id) {
    // Construction is not apply: a failure here aborts like embind's own allocation of the instance.
    const bool reserved = fState.nodes.reserve(fAlloc, 1);
    SkASSERT_RELEASE(reserved);
    fState.nodes.insert(kRoot).structure.kind = Kind::kGroup;
}

PlanInstance::~PlanInstance() {
    fStaged.clear();
    fState.nodes.forEachSlot([](Node& node) { FreeHeights(node.structure.childHeights); });
    fState.paths.forEachSlot([](PathEntry& entry) { FreePath(entry); });
}

uint8_t* PlanInstance::packetBuffer(size_t bytes) {
    if (bytes > kMaxPacketBytes) return nullptr;
    if (fPacket.size() < bytes) fPacket.resize(bytes);
    return fPacket.data();
}

ApplyResult PlanInstance::apply(size_t bytes) {
    auto reject = [this](Error code, uint32_t record = kNone) {
        fCounters.rejected++;
        return ApplyResult{code, record};
    };
    if (!fTokens.empty()) return reject(Error::kReplayActive);
    if (bytes < kHeaderBytes || bytes > fPacket.size()) return reject(Error::kBadPayload);
    const uint8_t* p = fPacket.data();
    const uint16_t flags = ReadU16(p + 6);
    const uint32_t base = ReadU32(p + 12), next = ReadU32(p + 16);
    const uint32_t records = ReadU32(p + 20), floats = ReadU32(p + 24);
    if (ReadU32(p) != kMagic) return reject(Error::kBadMagic);
    if (ReadU16(p + 4) != kAbiVersion) return reject(Error::kBadVersion);
    if (ReadU32(p + 8) != fId) return reject(Error::kBadInstance);
    if (base != fApplied) return reject(Error::kStaleSequence);
    if (base == kNone) return reject(Error::kLimit);  // the sequence is exhausted: replace the instance
    // Flag bit 0 marks the instance's initial build, which is exactly its first packet.
    if (next != base + 1 || ReadU32(p + 28) != 0 || (flags & ~1u) || bool(flags & 1) != (base == 0)) {
        return reject(Error::kBadPayload);
    }
    if (records > kMaxRecords || floats > kMaxFloats) return reject(Error::kLimit);
    const uint64_t size = kHeaderBytes + uint64_t(kRecordBytes) * records + 4 * uint64_t(floats);
    if (size > kMaxPacketBytes) return reject(Error::kLimit);
    if (size != bytes) return reject(Error::kBadPayload);

    const uint64_t start = fAlloc.calls;
    const ApplyResult checked = validate(p, records, floats);
    fCounters.lastAllocations = uint32_t(fAlloc.calls - start);
    fCounters.lastCommitAllocations = 0;
    if (checked.code != Error::kOk) {
        fCounters.lastCleanup = fStaged.clear();
        return reject(checked.code, checked.record);
    }
    const uint64_t committing = fAlloc.calls;
    commit(p, records, floats);
    fCounters.lastCommitAllocations = uint32_t(fAlloc.calls - committing);
    // The packet that touched the staged entries pays for freeing them.
    fCounters.lastCleanup = fStaged.clear();
    fApplied = next;
    fCounters.applied++;
    return {Error::kOk, kNone};
}

ApplyResult PlanInstance::validate(const uint8_t* packet, uint32_t records, uint32_t floats) {
    fStaged.nodeCount = fState.nodes.size();
    fStaged.pathCount = fState.paths.size();
    const Pool pool(packet + kHeaderBytes + kRecordBytes * records, floats);
    Validator validator(fState, fStaged, fAlloc, pool);
    ApplyResult result{Error::kOk, kNone};
    for (uint32_t i = 0; i < records && result.code == Error::kOk; i++) {
        const Error e = validator.check(ReadRecord(packet, i), i);
        if (e != Error::kOk) result = {e, e == Error::kAlloc ? kNone : i};
    }
    fCounters.lastRecords = records;
    fCounters.lastValidated = validator.validated;
    fCounters.lastHeightUpdates = validator.heightUpdates;
    if (result.code != Error::kOk) return result;
    // Limits hold on the final staged topology, so an intermediate state may exceed them (structuralDiff's order).
    const uint32_t nodes = fStaged.nodeCount - 1;  // node 0 is not counted
    if (nodes > kMaxNodes) return {Error::kLimit, kNone};
    if (validator.root().height > kMaxDepth) return {Error::kDepth, kNone};
    // Pass 2 removes before it inserts, so room for the final counts is all it needs.
    if (!fState.nodes.reserve(fAlloc, fStaged.nodeCount) || !fState.paths.reserve(fAlloc, fStaged.pathCount) ||
        !fState.pathHandles.reserve(fAlloc, fState.pathHandles.size() + fStaged.pathHandles.size())) {
        return {Error::kAlloc, kNone};
    }
    return {Error::kOk, kNone};
}

void PlanInstance::commit(const uint8_t* packet, uint32_t records, uint32_t floats) {
    StagedState& staged = fStaged;
    // Removed nodes and released paths first.
    for (uint32_t i = 0; i < staged.nodes.size(); i++) {
        const StagedNode& e = staged.nodes[i];
        if (!e.live || e.exists) continue;
        FreeHeights(fState.nodes.find(e.id)->structure.childHeights);
        fState.nodes.erase(e.id);
    }
    for (uint32_t i = 0; i < staged.paths.size(); i++) {
        const StagedPath& p = staged.paths[i];
        if (!p.live || p.defined) continue;
        FreePath(*fState.paths.find(p.key));
        fState.paths.erase(p.key);
    }
    // Final structures; a node inserted by this packet starts from default parameters.
    for (uint32_t i = 0; i < staged.nodes.size(); i++) {
        StagedNode& e = staged.nodes[i];
        if (!e.exists) continue;
        Node& node = e.live ? *fState.nodes.find(e.id) : fState.nodes.insert(e.id);
        if (e.born != kNone) node.params = Params();
        FreeHeights(node.structure.childHeights);
        node.structure = e.structure;
        e.structure.childHeights = HeightList();  // moved into the node
    }
    for (uint32_t i = 0; i < staged.paths.size(); i++) {
        StagedPath& p = staged.paths[i];
        if (!p.defined) continue;
        PathEntry& entry = p.live ? *fState.paths.find(p.key) : fState.paths.insert(p.key);
        if (p.fresh) {
            FreePath(entry);
            entry.data = p.data;
            p.data = PathData();  // moved into the entry
        }
        entry.refs = p.refs;
    }
    for (uint32_t i = 0; i < staged.pathHandles.size(); i++) {
        const auto& [handle, gens] = staged.pathHandles[i];
        uint32_t* live = fState.pathHandles.find(handle);
        if (!gens) {
            if (live) fState.pathHandles.erase(handle);
        } else if (live) {
            *live = gens;
        } else {
            fState.pathHandles.insert(handle) = gens;
        }
    }
    for (uint32_t i = 0; i < staged.pictureRefs.size(); i++) {
        fState.pictures.find(staged.pictureRefs[i].key)->second.refs = staged.pictureRefs[i].count;
    }
    // Parameters in record order, each on the incarnation its record named: none for a node removed by the end, and
    // none from before a node's latest Insert.
    const Pool pool(packet + kHeaderBytes + kRecordBytes * records, floats);
    for (uint32_t i = 0; i < records; i++) {
        const Record r = ReadRecord(packet, i);
        if (Op(r.op) <= Op::kMove || Op(r.op) == Op::kDefinePath || Op(r.op) == Op::kReleasePath) continue;
        if (const uint32_t* at = staged.nodeIndex.find(r.node)) {
            const StagedNode& e = staged.nodes[*at];
            if (!e.exists || (e.born != kNone && i < e.born)) continue;
        }
        ApplyParams(fState, *fState.nodes.find(r.node), r, pool);
    }
}

// ---- registrations -----------------------------------------------------------------------------------------------

namespace {

template <typename Entry>
Error Register(std::unordered_map<Key, Entry>& entries, std::unordered_map<uint32_t, uint32_t>& handles,
               uint32_t handle, uint32_t gen, Entry entry) {
    if (handle == kNone || gen == kNone) return Error::kLimit;
    if (!entries.try_emplace(MakeKey(handle, gen), std::move(entry)).second) return Error::kBadGeneration;
    handles[handle]++;
    return Error::kOk;
}

template <typename Entry, typename Referenced>
Error Release(std::unordered_map<Key, Entry>& entries, std::unordered_map<uint32_t, uint32_t>& handles,
              uint32_t handle, uint32_t gen, Error unknown, Referenced referenced) {
    auto it = entries.find(MakeKey(handle, gen));
    if (it == entries.end()) return handles.count(handle) ? Error::kBadGeneration : unknown;
    if (referenced(it->second)) return Error::kBadPayload;  // a node still names it
    entries.erase(it);
    if (--handles[handle] == 0) handles.erase(handle);
    return Error::kOk;
}

}  // namespace

Error PlanInstance::registerPicture(uint32_t handle, uint32_t gen, sk_sp<SkPicture> picture) {
    if (!picture) return Error::kBadPayload;
    return Register(fState.pictures, fState.pictureHandles, handle, gen, PictureEntry{std::move(picture), 0});
}

Error PlanInstance::releasePicture(uint32_t handle, uint32_t gen) {
    return Release(fState.pictures, fState.pictureHandles, handle, gen, Error::kUnknownPicture,
                   [](const PictureEntry& e) { return e.refs != 0; });
}

Error PlanInstance::registerImage(uint32_t handle, uint32_t gen, sk_sp<SkImage> image) {
    if (!image) return Error::kBadPayload;
    return Register(fState.images, fState.imageHandles, handle, gen, std::move(image));
}

Error PlanInstance::releaseImage(uint32_t handle, uint32_t gen) {
    return Release(fState.images, fState.imageHandles, handle, gen, Error::kUnknownPicture,
                   [](const sk_sp<SkImage>&) { return false; });
}

// ---- replay ------------------------------------------------------------------------------------------------------

// A token is one walk: a stack of frames over the plan, paused at each live node for its JS producer. Tokens nest
// strictly (LIFO); a paused token is resumed by the next step, which checks what the producer left on the canvas.

Error PlanInstance::canOpenToken(SkCanvas* canvas) const {
    if (!canvas) return Error::kBadPayload;
    if (fUnderRestore) return Error::kLiveUnderRestore;
    return fTokens.size() < kMaxOpenTokens ? Error::kOk : Error::kLimit;
}

PlanInstance::Token& PlanInstance::openToken(SkCanvas* canvas, const ReplayTarget& target) {
    const int32_t id = fNextToken;
    fNextToken = fNextToken == INT32_MAX ? 1 : fNextToken + 1;
    fTokens.push_back({id, canvas, canvas->getSaveCount(), target, 0, {}});
    fCounters.replays++;
    return fTokens.back();
}

int32_t PlanInstance::beginReplay(SkCanvas* canvas, uint32_t root, const ReplayTarget& target) {
    if (Error e = canOpenToken(canvas); e != Error::kOk) return -int32_t(e);
    if (!find(root)) return -int32_t(Error::kUnknownNode);
    if (!fTokens.empty()) {
        const Token& enclosing = fTokens.back();
        if (!enclosing.pauseSaveCount) return -int32_t(Error::kTokenOrder);
        const uint32_t live = enclosing.frames.back().id;
        uint32_t parent = find(root)->structure.parent;
        while (parent != kNone && parent != live) parent = find(parent)->structure.parent;
        if (parent != live) return -int32_t(Error::kTokenOrder);
    }
    if (target.colorType > TargetColorType::kF16Linear || !(target.outputScale > 0) ||
        !std::isfinite(target.outputScale)) {
        return -int32_t(Error::kBadPayload);
    }
    Token& token = openToken(canvas, target);
    pushNode(token, root, Scope(), false);
    return token.id;
}

int32_t PlanInstance::beginReplayChildren(SkCanvas* canvas, uint32_t live, float alpha) {
    if (Error e = canOpenToken(canvas); e != Error::kOk) return -int32_t(e);
    const Node* node = find(live);
    if (!node) return -int32_t(Error::kUnknownNode);
    if (node->structure.kind != Kind::kLive || !(alpha >= 0 && alpha <= 1)) return -int32_t(Error::kBadPayload);
    // The children belong to the innermost token's current pause, and replay inside it.
    if (fTokens.empty() || !fTokens.back().pauseSaveCount || fTokens.back().frames.back().id != live) {
        return -int32_t(Error::kTokenOrder);
    }
    const ReplayTarget target = fTokens.back().target;
    Token& token = openToken(canvas, target);
    // Only the children's own placements apply: the live node's params belong to the producer that called this.
    Scope scope;
    scope.foldedAlpha = alpha;
    token.frames.push_back({node, live, node->structure.first, canvas->getSaveCount(), Stage::kChildren, scope, false,
                            false});
    return token.id;
}

void PlanInstance::pushNode(Token& token, uint32_t id, Scope scope, bool clipBypass) {
    token.frames.push_back({fState.nodes.find(id), id, kNone, 0, Stage::kEnter, scope, false, clipBypass});
}

const SkPath& PlanInstance::clipPath(Key key) {
    PathEntry& entry = *fState.paths.find(key);
    if (!entry.path) entry.path = new SkPath(BuildPath(entry.data));
    return *entry.path;
}

int32_t PlanInstance::step(int32_t tokenId) {
    if (fTokens.empty() || fTokens.back().id != tokenId) return -int32_t(Error::kTokenOrder);
    if (fUnderRestore) return -int32_t(Error::kLiveUnderRestore);
    Token& token = fTokens.back();
    if (token.pauseSaveCount) {
        if (Error e = resume(token); e != Error::kOk) return -int32_t(e);
    }
    SkCanvas* canvas = token.canvas;
    while (!token.frames.empty()) {
        Frame& f = token.frames.back();
        const Node& n = *f.node;
        const Params& params = n.params;
        const bool live = n.structure.kind == Kind::kLive;
        switch (f.stage) {
            case Stage::kEnter:
                fCounters.visited++;
                if (!params.visible) {
                    token.frames.pop_back();
                    break;
                }
                f.saveCount = canvas->save();
                if (params.hasMatrix) canvas->concat(params.matrix);
                OpenIsolation(canvas, params.isolation, fCounters);
                if (params.isolation.set && (params.isolation.flags & kIsolationFold)) {
                    f.scope.foldedAlpha *= params.isolation.alpha;
                }
                if (params.contain != ContainKind::kNone) f.scope.contain = &n;
                // A live node's children are its producer's; the walk never enters them.
                f.softClip = params.clip.soft && !live;
                f.child = n.structure.first;
                f.stage = f.softClip ? Stage::kBypassChildren : Stage::kContent;
                break;
            case Stage::kBypassChildren: {
                // A soft clip's bypass children replay first, outside the clip.
                uint32_t c = f.child;
                while (c != kNone && !fState.nodes.find(c)->structure.bypass) c = fState.nodes.find(c)->structure.next;
                if (c == kNone) {
                    f.stage = Stage::kContent;
                    break;
                }
                f.child = fState.nodes.find(c)->structure.next;
                pushNode(token, c, f.scope, true);
                break;
            }
            case Stage::kContent:
                ApplyClip(canvas, params.clip,
                          params.clip.kind == ClipKind::kPath ? &clipPath(n.structure.clipPath) : nullptr, fCounters);
                if (params.picture) {
                    canvas->drawPicture(params.picture);
                    fCounters.picturesDrawn++;
                }
                if (live) {
                    f.stage = Stage::kExit;  // resumes with the live node's next sibling
                    return pause(token, f);
                }
                f.child = n.structure.first;
                f.stage = Stage::kChildren;
                break;
            case Stage::kChildren: {
                uint32_t c = f.child;
                while (c != kNone && f.softClip && fState.nodes.find(c)->structure.bypass) {
                    c = fState.nodes.find(c)->structure.next;
                }
                if (c == kNone) {
                    f.stage = Stage::kExit;
                    break;
                }
                f.child = fState.nodes.find(c)->structure.next;
                pushNode(token, c, f.scope, false);
                break;
            }
            case Stage::kExit:
                canvas->restoreToCount(f.saveCount);
                token.frames.pop_back();
                break;
        }
    }
    return 0;
}

// Hands the live node to JS on a protected canvas: the producer may push state, never pop what replay set.
int32_t PlanInstance::pause(Token& token, const Frame& frame) {
    SkCanvas* canvas = token.canvas;
    canvas->save();
    token.pauseSaveCount = canvas->getSaveCount();
    const Params& params = frame.node->params;
    LiveContext& c = fContexts[fTokens.size() - 1];
    c.node = frame.id;
    c.producer = params.live.producer;
    c.inputsRev = params.live.inputsGen;
    canvas->getLocalToDeviceAs3x3().get9(c.matrix);
    const SkIRect clip = canvas->getDeviceClipBounds();
    const float deviceClip[4] = {float(clip.fLeft), float(clip.fTop), float(clip.fRight), float(clip.fBottom)};
    std::memcpy(c.deviceClip, deviceClip, sizeof deviceClip);
    c.foldedAlpha = frame.scope.foldedAlpha;
    c.clipBypass = frame.clipBypass;
    const Params* contain = frame.scope.contain ? &frame.scope.contain->params : nullptr;
    c.containKind = contain ? uint32_t(contain->contain) : 0;
    if (contain) std::memcpy(c.containBounds, contain->containBounds, sizeof c.containBounds);
    else std::fill(std::begin(c.containBounds), std::end(c.containBounds), 0.f);
    c.target = token.target;
    c.baseSaveCount = token.pauseSaveCount;
    fCounters.livePauses++;
    return 1;
}

// Below the protected depth is a refusal, never a repair; above it, the producer's excess saves are dropped.
Error PlanInstance::resume(Token& token) {
    const int count = token.canvas->getSaveCount();
    if (count < token.pauseSaveCount) {
        fUnderRestore = true;
        return Error::kLiveUnderRestore;
    }
    if (count > token.pauseSaveCount) {
        token.canvas->restoreToCount(token.pauseSaveCount);
        fCounters.liveUnbalanced++;
    }
    token.canvas->restore();  // the protective save
    token.pauseSaveCount = 0;
    return Error::kOk;
}

Error PlanInstance::endReplay(int32_t tokenId) {
    if (fTokens.empty() || fTokens.back().id != tokenId) return Error::kTokenOrder;
    Token& token = fTokens.back();
    token.canvas->restoreToCount(token.baseSaveCount);
    fTokens.pop_back();
    if (fTokens.empty()) fUnderRestore = false;
    return Error::kOk;
}

}  // namespace cfplan
