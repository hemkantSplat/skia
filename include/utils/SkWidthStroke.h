#ifndef SkWidthStroke_DEFINED
#define SkWidthStroke_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/core/SkVertices.h"
#include <cstddef>
#include <cstdint>
#include <vector>

class SK_API SkWidthStroke final : public SkRefCnt {
public:
    enum class Batches { None, Disjoint };
    struct Options {
        double envelope;
        double pathLength;
        bool mergeJoins;
        Batches batches;
        bool subdivide;
    };
    struct Range { uint32_t start, count; };
    struct Chunk { Range range; uint32_t batch; sk_sp<SkVertices> vertices; };

    static sk_sp<SkWidthStroke> Make(const float* rows, size_t length, const Options&);
    const std::vector<SkPoint>& positions() const { return fPositions; }
    const std::vector<SkPoint>& texCoords() const { return fTexCoords; }
    const std::vector<uint32_t>& indices() const { return fIndices; }
    const std::vector<Range>& batches() const { return fBatches; }
    const std::vector<uint32_t>& depositCuts() const { return fDepositCuts; }
    const std::vector<Chunk>& chunks() const { return fChunks; }
    const SkRect& bounds() const { return fBounds; }
    size_t vertexCount() const { return fPositions.size(); }
    sk_sp<SkVertices> vertices(size_t chunk) const;

private:
    std::vector<SkPoint> fPositions, fTexCoords;
    std::vector<uint32_t> fIndices, fDepositCuts;
    std::vector<Range> fBatches;
    std::vector<Chunk> fChunks;
    SkRect fBounds = SkRect::MakeEmpty();
};
#endif
