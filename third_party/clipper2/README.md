# Clipper2

Official upstream: https://github.com/AngusJohnson/Clipper2

Release: Clipper2_2.0.1
Commit: 21ebba05db8894f0c7217ad35ea518080f324946
License: Boost Software License 1.0 (LICENSE).

C++ library sources and headers from the pinned release, with the local patch below. SkPathOffset uses engine and offset;
rect clipping and triangulation are retained from the pinned release but not linked.

Local patches (apply from this directory with `patch -p1`):
- `patches/0001-miter-limit-bevel-fallback.patch`: adds the opt-in
  `ClipperOffset::MiterBevelFallback(bool)` option for SVG/Skia miter joins.
  Its default is false, preserving upstream behavior for other callers.
