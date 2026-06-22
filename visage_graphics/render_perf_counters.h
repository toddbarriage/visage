// Classic macro include guard (NOT #pragma once): this header is GLOB-amalgamated into
// visage/graphics.h AND includable raw as <visage_graphics/render_perf_counters.h>, so a
// single TU can see its text twice. A name-based guard dedupes that; #pragma once (file-
// identity based) would not, and the inline function would redefine.
#ifndef VISAGE_RENDER_PERF_COUNTERS_H
#define VISAGE_RENDER_PERF_COUNTERS_H

// Render-performance per-site transient-buffer attribution (compile-flagged via the
// RENDER_PERF_DIAG CMake option — see CMakeLists.txt). Every consumer of bgfx's shared
// transient vertex/index pool increments these counters at its alloc site during frame
// submission; canvas.cpp snapshots + resets them once per bgfx::frame() flush and folds
// them into the 1 Hz CSV. This turns the aggregate "transientVbUsed" number from
// bgfx::getStats() into a ranked breakdown (path vs quad vs post), which is what tells
// us WHICH primitive class is flooding the pool and whether it is stationary chrome we
// are needlessly re-submitting every frame.
//
// There are exactly FOUR transient-pool consumers in the fleet:
//   1. path.cpp          — tessellated path shapes (knob arcs, keyboard keys, waveform,
//                          pitch trail, dividers, the fairy) — the fat-VB primitive class
//                          and the one Todd observes flickering when the pool overflows.
//   2. shape_batcher.cpp — quad batches (rects, images, native-text glyph quads).
//   3. post_effects.cpp  — post-effect setup quads (blur downsample) — negligible VB.
//   4. SlugRenderer.cpp  — Slug SDF glyph quads (labels/value-bubbles). Lives in
//                          libs/s-series-ui (outside this submodule); it includes this
//                          header raw as <visage_graphics/render_perf_counters.h> and
//                          increments slugBytes/slugDrops directly, so a slug drop (text
//                          flickering out under pool exhaustion) is measured, not inferred.
//                          The `visage` aggregate links VisageGraphics PRIVATE, so the
//                          RENDER_PERF_DIAG define does NOT propagate to that target through
//                          visage; instead s-series-ui's CMakeLists sets the define on its
//                          own INTERFACE target (gated on the option) so the plugin that
//                          compiles SlugRenderer.cpp sees it.
//
// bgfx::getStats().transientVbUsed (read in canvas.cpp) is the ground-truth process-global
// total and is logged alongside as an independent cross-check: path+quad+post+slug should
// track it; a divergence flags a miscount.
//
// All bytes are VERTEX bytes (num_vertices * layout stride) — the dominant pool axis and
// the one bgfx's transientVbUsed reports. Index bytes are tracked by bgfx separately.
//
// Threading: the render path is single-threaded (every plugin editor window draws on the
// host main thread, serialized), so plain non-atomic accumulation is correct. The whole
// header is compiled out when RENDER_PERF_DIAG is OFF — release builds never see it.

#if RENDER_PERF_DIAG

#include <cstdint>

namespace visage::render_perf {

  struct Counters {
    // Successful transient-VB vertex bytes allocated this frame, per consumer.
    uint64_t pathBytes = 0;
    uint64_t quadBytes = 0;
    uint64_t postBytes = 0;
    uint64_t slugBytes = 0;
    // Dropped allocations this frame (allocTransientBuffers returned false → the draw is
    // silently skipped → that primitive flickers out of existence). This is the direct,
    // falsifiable proof of pool exhaustion: nonzero here == a visible dropped draw.
    // slugDrops answers "does text flicker too?" with data rather than inference.
    uint32_t pathDrops = 0;
    uint32_t quadDrops = 0;
    uint32_t postDrops = 0;
    uint32_t slugDrops = 0;
  };

  // One shared instance across all translation units (inline function => single static
  // local by the ODR). Returned by reference so call sites read/write the live counters.
  inline Counters& counters() {
    static Counters c;
    return c;
  }

}  // namespace visage::render_perf

#endif  // RENDER_PERF_DIAG

#endif  // VISAGE_RENDER_PERF_COUNTERS_H
