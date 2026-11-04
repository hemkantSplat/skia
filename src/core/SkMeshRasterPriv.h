/* Raster programs belong to the immutable specification, shared by its meshes. */
#ifndef SkMeshRasterPriv_DEFINED
#define SkMeshRasterPriv_DEFINED
#include "include/core/SkMesh.h"
#include <string>

class SkBitmapDevice;
class SkPaint;
struct SkMeshRasterProgram {
    sk_sp<SkRuntimeEffect> vertex;
    sk_sp<SkRuntimeEffect> fragment;
    std::string prefix;
    std::string error;
    int slots = 0;
};
struct SkMeshRasterPriv {
    static std::unique_ptr<const SkMeshRasterProgram> Make(const SkMeshSpecification&);
    static const SkMeshRasterProgram& Get(const SkMeshSpecification&);
    static void Draw(SkBitmapDevice*, const SkMesh&, sk_sp<SkBlender>, const SkPaint&);
};
#endif
