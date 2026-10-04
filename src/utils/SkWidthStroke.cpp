#include "include/utils/SkWidthStroke.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace {
struct Point { double x, y; };
struct Mesh {
    std::vector<Point> positions, uv;
    std::vector<uint32_t> indices, cuts;
};
using Triangle = std::array<Point, 3>;

std::vector<std::array<double, 2>> bounded_offsets(const float* rows, size_t n, double e) {
    std::vector<std::array<double, 2>> offsets(n, {-e, e});
    for (size_t i = 0; i + 1 < n; ++i) {
        double dx = double(rows[i+1]) - rows[i], dy = double(rows[n+i+1]) - rows[n+i];
        double ax = rows[2*n+i], ay = rows[3*n+i], bx = rows[2*n+i+1], by = rows[3*n+i+1];
        double cross = ax*by - ay*bx;
        if (std::abs(cross) < 1e-9) { continue; }
        double distances[] = {(dx*by-dy*bx)/cross, (dx*ay-dy*ax)/cross};
        for (size_t j = 0; j < 2; ++j) {
            double q = distances[j];
            if (q > 0) { offsets[i+j][1] = std::min(offsets[i+j][1], q*(1-1e-6)); }
            else { offsets[i+j][0] = std::max(offsets[i+j][0], q*(1-1e-6)); }
        }
    }
    return offsets;
}

std::vector<uint32_t> deposit_cuts(const float* rows, size_t n) {
    std::vector<uint32_t> cuts;
    for (size_t j = 2; j + 1 < n; ++j) {
        for (size_t i = 0; i + 1 < j; ++i) {
            double ax = double(rows[i+1])-rows[i], ay = double(rows[n+i+1])-rows[n+i];
            double bx = double(rows[j+1])-rows[j], by = double(rows[n+j+1])-rows[n+j];
            double cross = ax*by-ay*bx;
            if (std::abs(cross) < 1e-8) { continue; }
            double dx = double(rows[j])-rows[i], dy = double(rows[n+j])-rows[n+i];
            double a = (dx*by-dy*bx)/cross, b = (dx*ay-dy*ax)/cross;
            if (a >= 0 && a < 1 && b >= 0 && b < 1) {
                cuts.push_back(static_cast<uint32_t>((j+1)*6));
                break;
            }
        }
    }
    return cuts;
}

void subdivide(Mesh& mesh) {
    std::unordered_map<uint64_t, uint32_t> cache;
    std::vector<uint32_t> indices;
    indices.reserve(mesh.indices.size()*4);
    auto midpoint = [&](uint32_t a, uint32_t b) {
        uint64_t key = (uint64_t(std::min(a,b)) << 32) | std::max(a,b);
        auto found = cache.find(key);
        if (found != cache.end()) { return found->second; }
        uint32_t index = static_cast<uint32_t>(mesh.positions.size());
        for (auto* values : {&mesh.positions, &mesh.uv}) {
            values->push_back({((*values)[a].x+(*values)[b].x)/2,
                               ((*values)[a].y+(*values)[b].y)/2});
        }
        cache.emplace(key, index);
        return index;
    };
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        uint32_t a=mesh.indices[i], b=mesh.indices[i+1], c=mesh.indices[i+2];
        uint32_t ab=midpoint(a,b), bc=midpoint(b,c), ca=midpoint(c,a);
        indices.insert(indices.end(), {a,ab,ca, ab,b,bc, ca,bc,c, ab,bc,ca});
    }
    mesh.indices = std::move(indices);
    for (auto& cut : mesh.cuts) { cut *= 4; }
}

bool overlaps(const Triangle& a, const Triangle& b) {
    for (const auto* polygon : {&a, &b}) {
        for (size_t i = 0; i < 3; ++i) {
            Point p=(*polygon)[i], next=(*polygon)[(i+1)%3];
            double nx=p.y-next.y, ny=next.x-p.x;
            double amin=INFINITY, amax=-INFINITY, bmin=INFINITY, bmax=-INFINITY;
            for (size_t j=0; j<3; ++j) {
                double av=nx*a[j].x+ny*a[j].y, bv=nx*b[j].x+ny*b[j].y;
                amin=std::min(amin,av); amax=std::max(amax,av);
                bmin=std::min(bmin,bv); bmax=std::max(bmax,bv);
            }
            if (amin >= bmax-1e-7 || bmin >= amax-1e-7) { return false; }
        }
    }
    return true;
}

std::vector<SkWidthStroke::Range> disjoint_batches(const Mesh& mesh) {
    using Key = std::pair<double, double>;
    std::map<Key, std::vector<size_t>> grid;
    std::vector<Triangle> triangles;
    std::vector<size_t> wide;
    std::vector<SkWidthStroke::Range> batches;
    uint32_t start = 0;
    for (uint32_t i = 0; i < mesh.indices.size(); i += 3) {
        Triangle t = {mesh.positions[mesh.indices[i]], mesh.positions[mesh.indices[i+1]],
                      mesh.positions[mesh.indices[i+2]]};
        double left=INFINITY, top=INFINITY, right=-INFINITY, bottom=-INFINITY;
        for (Point p : t) {
            left=std::min(left,p.x); right=std::max(right,p.x);
            top=std::min(top,p.y); bottom=std::max(bottom,p.y);
        }
        left=std::floor(left/64); right=std::floor(right/64);
        top=std::floor(top/64); bottom=std::floor(bottom/64);
        std::vector<Key> keys;
        // Huge finite envelopes use the same SAT test without enumerating an unbounded grid.
        bool large = (right-left+1)*(bottom-top+1) > 4096 ||
                     std::max({std::abs(left),std::abs(right),std::abs(top),std::abs(bottom)}) > 1e12;
        std::set<size_t> candidates(wide.begin(),wide.end());
        if (large) {
            for (size_t j=0; j<triangles.size(); ++j) { candidates.insert(j); }
        } else {
            for (double y=top; y<=bottom; ++y) for (double x=left; x<=right; ++x) {
                Key key{x,y}; keys.push_back(key);
                auto found=grid.find(key);
                if (found!=grid.end()) { candidates.insert(found->second.begin(),found->second.end()); }
            }
        }
        bool collision = std::any_of(candidates.begin(),candidates.end(),
                                    [&](size_t j) { return overlaps(t,triangles[j]); });
        if (collision) {
            batches.push_back({start,i-start}); start=i;
            grid.clear(); triangles.clear(); wide.clear();
        }
        size_t index=triangles.size(); triangles.push_back(t);
        if (large) { wide.push_back(index); }
        for (Key key : keys) { grid[key].push_back(index); }
    }
    if (start < mesh.indices.size()) {
        batches.push_back({start,static_cast<uint32_t>(mesh.indices.size()-start)});
    }
    return batches;
}

bool retain_chunks(const std::vector<SkPoint>& positions, const std::vector<SkPoint>& uv,
                   const std::vector<uint32_t>& indices,
                   const std::vector<SkWidthStroke::Range>& batches,
                   const std::vector<uint32_t>& cuts, std::vector<SkWidthStroke::Chunk>& chunks) {
    std::unordered_map<uint32_t,uint16_t> remap;
    std::vector<SkPoint> localPositions, localUV;
    std::vector<uint16_t> localIndices;
    size_t cutIndex=0;
    for (size_t batch=0; batch<batches.size(); ++batch) {
        auto range=batches[batch];
        uint32_t start=range.start;
        auto flush = [&]() {
            if (localIndices.empty()) { return true; }
            auto vertices=SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode,
                static_cast<int>(localPositions.size()),localPositions.data(),localUV.data(),nullptr,
                static_cast<int>(localIndices.size()),localIndices.data());
            if (!vertices) { return false; }
            chunks.push_back({{start,static_cast<uint32_t>(localIndices.size())},
                              static_cast<uint32_t>(batch),std::move(vertices)});
            start+=static_cast<uint32_t>(localIndices.size());
            remap.clear(); localPositions.clear(); localUV.clear(); localIndices.clear();
            return true;
        };
        for (uint32_t i=range.start; i<range.start+range.count; i+=3) {
            while (cutIndex<cuts.size() && cuts[cutIndex]<i) { ++cutIndex; }
            size_t added=0;
            for (size_t j=0;j<3;++j) { added+=remap.count(indices[i+j])==0; }
            if (remap.size()+added>65535 || (cutIndex<cuts.size() && cuts[cutIndex]==i)) {
                if (!flush()) { return false; }
            }
            for (size_t j=0;j<3;++j) {
                uint32_t global=indices[i+j];
                auto inserted=remap.emplace(global,static_cast<uint16_t>(remap.size()));
                if (inserted.second) {
                    localPositions.push_back(positions[global]); localUV.push_back(uv[global]);
                }
                localIndices.push_back(inserted.first->second);
            }
        }
        if (!flush()) { return false; }
    }
    return true;
}
}  // namespace

sk_sp<SkWidthStroke> SkWidthStroke::Make(const float* rows, size_t length, const Options& options) {
    size_t n=length/8;
    // Subdivision has at most 8*n+13 vertices and 24*(n+1) indices, all addressed by uint32.
    if (!rows || length%8 || n<2 || n>(std::numeric_limits<uint32_t>::max()/24)-1 ||
        !std::isfinite(options.envelope) || options.envelope<=0 ||
        !std::isfinite(options.pathLength) || options.pathLength<0 ||
        (options.batches!=Batches::None && options.batches!=Batches::Disjoint)) { return nullptr; }
    for (size_t i=0;i<length;++i) { if (!std::isfinite(rows[i])) { return nullptr; } }
    Mesh mesh;
    auto offsets=bounded_offsets(rows,n,options.envelope);
    auto add = [&](size_t i,double u,double along) {
        double nx=rows[2*n+i], ny=rows[3*n+i];
        for (double q : offsets[i]) {
            mesh.positions.push_back({rows[i]+nx*q+ny*along,rows[n+i]+ny*q-nx*along});
            mesh.uv.push_back({u,q});
        }
    };
    add(0,-options.envelope,-options.envelope);
    for (size_t i=0;i<n;++i) { add(i,options.pathLength*i/(n-1),0); }
    add(n-1,options.pathLength+options.envelope,options.envelope);
    for (uint32_t i=0;i<n+1;++i) {
        uint32_t a=i*2; mesh.indices.insert(mesh.indices.end(),{a,a+1,a+2,a+1,a+3,a+2});
    }
    if (options.mergeJoins) { mesh.cuts=deposit_cuts(rows,n); }
    if (options.subdivide) { subdivide(mesh); }
    auto result=sk_sp<SkWidthStroke>(new SkWidthStroke);
    for (Point p : mesh.positions) {
        SkPoint point=SkPoint::Make(static_cast<float>(p.x),static_cast<float>(p.y));
        if (!point.isFinite()) { return nullptr; }
        result->fPositions.push_back(point);
    }
    for (Point p : mesh.uv) {
        SkPoint point=SkPoint::Make(static_cast<float>(p.x),static_cast<float>(p.y));
        if (!point.isFinite()) { return nullptr; }
        result->fTexCoords.push_back(point);
    }
    result->fBatches=options.batches==Batches::Disjoint ? disjoint_batches(mesh) :
        std::vector<Range>{{0,static_cast<uint32_t>(mesh.indices.size())}};
    result->fIndices=std::move(mesh.indices); result->fDepositCuts=std::move(mesh.cuts);
    result->fBounds.setBounds(result->fPositions.data(),static_cast<int>(result->fPositions.size()));
    if (!retain_chunks(result->fPositions,result->fTexCoords,result->fIndices,
                       result->fBatches,result->fDepositCuts,result->fChunks)) { return nullptr; }
    return result;
}

sk_sp<SkVertices> SkWidthStroke::vertices(size_t chunk) const {
    return chunk<fChunks.size() ? fChunks[chunk].vertices : nullptr;
}
