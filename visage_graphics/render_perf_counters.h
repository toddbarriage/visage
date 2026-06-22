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
#include <cstring>

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

  // ===== Per-element ("scope") attribution =================================
  // A "scope" is a named label set by RPERF_SCOPE(...) around a draw block. The
  // four alloc sites attribute their vertex bytes/drops to the CURRENT scope IN
  // ADDITION to the global counters() above — the globals are left untouched, so
  // the legacy ~/render-perf-diag.csv stays byte-identical and remains the
  // cross-check. Slot 0 is a permanent "unscoped" catch-all, so EVERYTHING is
  // attributed even before any draw code is annotated, and the sum of all scopes
  // per consumer == the corresponding global total (a built-in miscount gate).
  // Single-threaded render path => plain statics, no atomics. Fully bounded: no
  // heap allocation ever (fixed kMaxScopes; overflow folds into the catch-all).
  enum Consumer { kPath = 0, kQuad = 1, kPost = 2, kSlug = 3 };

  inline constexpr int kMaxScopes = 256;

  // One ~1 s CSV window of per-scope accumulation: summed bytes (for the avg),
  // peak per-frame bytes (for the spike), and summed drops — per consumer.
  struct ScopeWindow {
    uint64_t sumBytes[4] = { 0, 0, 0, 0 };
    uint64_t maxBytes[4] = { 0, 0, 0, 0 };
    uint64_t sumDrops[4] = { 0, 0, 0, 0 };
  };

  struct ScopeRegistry {
    const char* names[kMaxScopes] = {};          // string-literal pointers (static lifetime)
    uint64_t    frameBytes[kMaxScopes][4] = {};  // this-frame bytes,  per consumer
    uint64_t    frameDrops[kMaxScopes][4] = {};  // this-frame drops,  per consumer
    ScopeWindow window[kMaxScopes] = {};
    int         count = 0;
    int         current = 0;                      // active scope id; 0 == "unscoped"
  };

  inline ScopeRegistry& scopes() {
    static ScopeRegistry r;
    if (r.count == 0) { r.names[0] = "unscoped"; r.count = 1; }  // permanent catch-all
    return r;
  }

  // Register-or-lookup by name. Same name => same id, so multiple draw sites that
  // share a label pool into one bucket. Overflow folds into slot 0 (unscoped).
  inline int registerScope(const char* name) {
    ScopeRegistry& r = scopes();
    for (int i = 0; i < r.count; ++i)
      if (r.names[i] == name || std::strcmp(r.names[i], name) == 0)
        return i;
    if (r.count >= kMaxScopes) return 0;
    int id = r.count++;
    r.names[id] = name;
    return id;
  }

  // RAII guard: set current scope on entry, restore the previous on exit (nests).
  struct ScopeGuard {
    int prev;
    explicit ScopeGuard(int id) { ScopeRegistry& r = scopes(); prev = r.current; r.current = id; }
    ~ScopeGuard() { scopes().current = prev; }
  };

  // Called by the four alloc sites IN ADDITION to the global counters() increment.
  inline void scopeAddBytes(int consumer, uint64_t bytes) {
    ScopeRegistry& r = scopes();
    r.frameBytes[r.current][consumer] += bytes;
  }
  inline void scopeAddDrop(int consumer) {
    ScopeRegistry& r = scopes();
    r.frameDrops[r.current][consumer] += 1;
  }

  // Called once per flushed frame (canvas.cpp, beside the global reset): fold this
  // frame's per-scope bytes/drops into the window (sum + per-frame peak), then zero.
  inline void scopeAccumulateFrameAndReset() {
    ScopeRegistry& r = scopes();
    for (int i = 0; i < r.count; ++i) {
      ScopeWindow& w = r.window[i];
      for (int c = 0; c < 4; ++c) {
        const uint64_t b = r.frameBytes[i][c];
        w.sumBytes[c] += b;
        if (b > w.maxBytes[c]) w.maxBytes[c] = b;
        w.sumDrops[c] += r.frameDrops[i][c];
        r.frameBytes[i][c] = 0;
        r.frameDrops[i][c] = 0;
      }
    }
  }

  // Called at CSV emit (once per ~1 s window): zero the window accumulators.
  inline void scopeResetWindow() {
    ScopeRegistry& r = scopes();
    for (int i = 0; i < r.count; ++i)
      r.window[i] = ScopeWindow{};
  }

}  // namespace visage::render_perf

// RPERF_SCOPE("Module/class") — wrap a draw block; tallies its transient-VB vertex
// bytes to that label. Registers once per call site (static local); the guard
// restores the previous scope on block exit. __LINE__ uniquifies the locals so two
// scopes can sit in sibling blocks of one function without identifier collision.
#define RPERF_SCOPE_CONCAT_(a, b) a##b
#define RPERF_SCOPE_CONCAT(a, b) RPERF_SCOPE_CONCAT_(a, b)
#define RPERF_SCOPE(NAME)                                                             \
  static const int RPERF_SCOPE_CONCAT(rperf_scope_id_, __LINE__) =                    \
      ::visage::render_perf::registerScope(NAME);                                     \
  ::visage::render_perf::ScopeGuard RPERF_SCOPE_CONCAT(rperf_scope_guard_, __LINE__)( \
      RPERF_SCOPE_CONCAT(rperf_scope_id_, __LINE__))

#else  // !RENDER_PERF_DIAG

// Compiled out entirely in release: the macro evaluates to nothing.
#define RPERF_SCOPE(NAME) ((void)0)

#endif  // RENDER_PERF_DIAG

#endif  // VISAGE_RENDER_PERF_COUNTERS_H
