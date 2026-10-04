#ifndef SkPathOffset_DEFINED
#define SkPathOffset_DEFINED

#include "include/core/SkPath.h"
#include "include/core/SkScalar.h"
#include "include/core/SkTypes.h"

enum class SkPathOffsetJoin { Round, Miter, Bevel };

// Signed filled-region offset. Failure leaves dst unchanged; zero copies src exactly.
SK_API bool SkPathOffset(const SkPath& src, SkScalar distance, SkPathOffsetJoin join,
                         SkScalar miterLimit, SkScalar tolerance, SkPath* dst);

#endif
