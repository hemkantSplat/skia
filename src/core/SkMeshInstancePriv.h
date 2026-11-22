/*
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkMeshInstancePriv_DEFINED
#define SkMeshInstancePriv_DEFINED

#include "include/core/SkMesh.h"

#include <utility>

struct SkMeshInstancePriv {
    static SkMesh WithRecords(const SkMesh& model, sk_sp<SkMesh::VertexBuffer> records,
                              size_t count, const SkRect& bounds) {
        SkMesh draw = model;
        draw.fInstances = {std::move(records), 0, count};
        draw.fBounds = bounds;
        return draw;
    }
};

#endif
