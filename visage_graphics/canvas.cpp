/* Copyright Vital Audio, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "canvas.h"

#include "palette.h"
#include "renderer.h"
#include "theme.h"

#include <bgfx/bgfx.h>

#if RENDER_PERF_DIAG
// Render-performance diagnostic (compile-flagged via the RENDER_PERF_DIAG CMake option).
// Aggregates bgfx per-frame stats and appends a 1 Hz CSV row to ~/render-perf-diag.csv.
// Runs entirely on Visage's render thread — never the audio thread — so an open ofstream
// flushed once per second is cheap. Compiled out completely when the option is OFF.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "render_perf_counters.h"  // per-site transient-VB attribution counters
#endif

namespace visage {
  bool Canvas::swapChainSupported() {
    return bgfx::getCaps()->supported & BGFX_CAPS_SWAP_CHAIN;
  }

  Canvas::Canvas() :
      image_atlas_(ImageAtlas::DataType::RGBA8), data_atlas_(ImageAtlas::DataType::Float32),
      composite_layer_(&gradient_atlas_) {
    state_.current_region = &default_region_;
    layers_.push_back(&composite_layer_);
    composite_layer_.addRegion(&window_region_);

    window_region_.setCanvas(this);
    window_region_.addRegion(&default_region_);
    default_region_.setCanvas(this);
    default_region_.setNeedsLayer(true);
  }

  void Canvas::clearDrawnShapes() {
    default_region_.clear();
    default_region_.invalidate();
    composite_layer_.clear();
    composite_layer_.addRegion(&window_region_);
  }

  void Canvas::setWindowless(int width, int height) {
    setDimensions(width, height);
    composite_layer_.setWindowlessRender(width, height);
    Renderer::instance().initializeWindowless();
    bgfx::reset(0, 0, BGFX_RESET_FLUSH_AFTER_RENDER | BGFX_RESET_FLIP_AFTER_RENDER);
  }

  void Canvas::setupRenderScale(void* window_handle, int display_w, int display_h,
                                  int render_w, int render_h) {
    if (display_w <= 0 || display_h <= 0)
      return;

    render_scale_ = static_cast<float>(render_w) / display_w;
    display_width_ = display_w;
    display_height_ = display_h;
    render_width_ = render_w;
    render_height_ = render_h;

    // Composite layer renders offscreen at render resolution.
    // removeFromWindow() clears the window handle so checkFrameBuffer()
    // creates a texture RT instead of a swap chain.
    composite_layer_.removeFromWindow();
    setDimensions(render_w, render_h);

    // Create window-backed FB at display resolution for presentation.
    bgfx::FrameBufferHandle old_fb = { window_fb_idx_ };
    if (bgfx::isValid(old_fb))
      bgfx::destroy(old_fb);
    bgfx::FrameBufferHandle new_fb = bgfx::createFrameBuffer(
        window_handle, display_w, display_h, bgfx::TextureFormat::RGBA8);
    window_fb_idx_ = new_fb.idx;
  }

  void Canvas::updateDisplaySize(void* window_handle, int display_w, int display_h) {
    if (render_scale_ <= 0.0f)
      return;
    if (display_w <= 0 || display_h <= 0)
      return;
    if (display_w == display_width_ && display_h == display_height_)
      return;

    display_width_ = display_w;
    display_height_ = display_h;
    render_scale_ = static_cast<float>(render_width_) / display_w;

    bgfx::FrameBufferHandle old_fb = { window_fb_idx_ };
    if (bgfx::isValid(old_fb))
      bgfx::destroy(old_fb);
    bgfx::FrameBufferHandle new_fb = bgfx::createFrameBuffer(
        window_handle, display_w, display_h, bgfx::TextureFormat::RGBA8);
    window_fb_idx_ = new_fb.idx;
  }

  void Canvas::clearRenderScale(void* window_handle, int display_w, int display_h) {
    render_scale_ = 0.0f;
    render_width_ = 0;
    render_height_ = 0;
    display_width_ = 0;
    display_height_ = 0;

    bgfx::FrameBufferHandle fb = { window_fb_idx_ };
    if (bgfx::isValid(fb)) {
      bgfx::destroy(fb);
      window_fb_idx_ = bgfx::kInvalidHandle;
    }

    // Restore composite layer to window-backed mode
    composite_layer_.pairToWindow(window_handle, display_w, display_h);
    setDimensions(display_w, display_h);
  }

  void Canvas::setDimensions(int width, int height) {
    VISAGE_ASSERT(state_memory_.empty());
    width = std::max(1, width);
    height = std::max(1, height);
    composite_layer_.setDimensions(width, height);
    window_region_.setBounds(0, 0, width, height);
    default_region_.setBounds(0, 0, width, height);
    setClampBounds(0, 0, width, height);
  }

  int Canvas::submit(int submit_pass) {
    default_region_.computeBackdropCount();
    int submission = submit_pass;
    int last_submission = submission - 1;

    submission = path_atlas_.updatePaths(submission);

    for (int i = 2; i < layers_.size(); ++i) {
      if (!layers_[1]->invalidRects().empty())
        layers_[i]->checkBackdropInvalidation(layers_[1]->invalidRects().begin()->second);
    }

    for (int backdrop = 0; submission != last_submission; backdrop++) {
      last_submission = submission;
      for (int i = layers_.size() - 1; i > 0; --i)
        submission = layers_[i]->submit(submission, backdrop);
    }

    for (int i = 1; i < layers_.size(); ++i)
      layers_[i]->clearInvalidRects();

    if (submission > submit_pass) {
      composite_layer_.invalidate();
      submission = composite_layer_.submit(submission, 0);

      // Render-scale downscale pass: offscreen composite → window presentation FB.
      // Inserted after composite submission (all canvas + Slug content rendered)
      // and before bgfx::frame() (which flushes the command buffer).
      if (render_scale_ > 0.0f && downscale_callback_ && window_fb_idx_ != bgfx::kInvalidHandle) {
        bgfx::TextureHandle src = bgfx::getTexture(composite_layer_.frameBuffer());
        if (bgfx::isValid(src))
          submission = downscale_callback_(submission, src.idx, window_fb_idx_,
                                           display_width_, display_height_);
      }

      bgfx::frame();

#if RENDER_PERF_DIAG
      // Tap bgfx::getStats() IMMEDIATELY after the per-frame bgfx::frame() flush:
      // getStats() reports the most-recently-SUBMITTED frame, so this is the only point
      // where the stats describe the frame we just rendered (this is the single
      // once-per-rendered-frame frame() call — the render_frame_==0 double-flush below
      // and the skipped-frame branch are not rendered content). Read-only observation
      // plus a 1 Hz file append; it changes no rendering behavior.
      {
        const bgfx::Stats* s = bgfx::getStats();

        // Per-frame accumulators (render thread is single-threaded → plain statics are safe).
        static uint64_t accFrames = 0;        // frames counted in the current 1 s window
        static uint64_t accDraw = 0;          // Σ draw calls
        static uint64_t accTvb = 0;           // Σ transient vertex-buffer bytes used
        static uint64_t accTib = 0;           // Σ transient index-buffer bytes used
        static double accCpuMs = 0.0;         // Σ CPU frame time (ms)
        static double accGpuMs = 0.0;         // Σ GPU frame time (ms)
        static uint32_t maxDraw = 0;          // peak draw calls in window
        static int32_t maxTvb = 0;            // peak transient VB bytes in window
        static int32_t maxTib = 0;            // peak transient IB bytes in window

        // Per-site transient-VB attribution (render_perf_counters.h). Σ vertex bytes per
        // window, plus per-frame peak, plus window-summed dropped allocations per site.
        static uint64_t accPathVb = 0, accQuadVb = 0, accPostVb = 0, accSlugVb = 0;
        static uint64_t maxPathVb = 0, maxQuadVb = 0, maxSlugVb = 0;
        static uint64_t accPathDrops = 0, accQuadDrops = 0, accSlugDrops = 0, accPostDrops = 0;

        // CPU/GPU frame time in milliseconds: (end - begin) / timerFreq → seconds → ×1000.
        // timerFreq is timestamps-per-second; guard against a 0 freq (GPU timing not
        // supported on some backends → emit 0 rather than divide by zero).
        const double cpuMs = (s->cpuTimerFreq > 0)
            ? (double)(s->cpuTimeEnd - s->cpuTimeBegin) / (double)s->cpuTimerFreq * 1000.0
            : 0.0;
        const double gpuMs = (s->gpuTimerFreq > 0)
            ? (double)(s->gpuTimeEnd - s->gpuTimeBegin) / (double)s->gpuTimerFreq * 1000.0
            : 0.0;

        accFrames++;
        accDraw += s->numDraw;
        accTvb += (uint64_t)(s->transientVbUsed < 0 ? 0 : s->transientVbUsed);
        accTib += (uint64_t)(s->transientIbUsed < 0 ? 0 : s->transientIbUsed);
        accCpuMs += cpuMs;
        accGpuMs += gpuMs;
        if (s->numDraw > maxDraw) maxDraw = s->numDraw;
        if (s->transientVbUsed > maxTvb) maxTvb = s->transientVbUsed;
        if (s->transientIbUsed > maxTib) maxTib = s->transientIbUsed;

        // Snapshot the per-site attribution counters for the frame just flushed, then zero
        // them for the next frame. Single-threaded render path → no atomics needed. The
        // counters accumulate across all instances submitting into this bgfx::frame(), so
        // this matches the process-global vantage of getStats() read above.
        {
          render_perf::Counters& rpc = render_perf::counters();
          accPathVb += rpc.pathBytes;  accQuadVb += rpc.quadBytes;
          accPostVb += rpc.postBytes;  accSlugVb += rpc.slugBytes;
          if (rpc.pathBytes > maxPathVb) maxPathVb = rpc.pathBytes;
          if (rpc.quadBytes > maxQuadVb) maxQuadVb = rpc.quadBytes;
          if (rpc.slugBytes > maxSlugVb) maxSlugVb = rpc.slugBytes;
          accPathDrops += rpc.pathDrops;  accQuadDrops += rpc.quadDrops;
          accSlugDrops += rpc.slugDrops;  accPostDrops += rpc.postDrops;
          rpc = render_perf::Counters{};  // reset for next frame
        }

        // Emit one aggregated CSV row roughly every 1000 ms of wall-clock time.
        using clock = std::chrono::steady_clock;
        static const clock::time_point startTime = clock::now();  // t_ms origin (first frame)
        static clock::time_point lastEmit = startTime;
        const clock::time_point now = clock::now();
        const auto sinceEmitMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count();

        if (sinceEmitMs >= 1000 && accFrames > 0) {
          // Open the CSV once (append mode) and keep it open; write a header if it's new/empty.
          static std::ofstream csv = [] {
            const char* home = std::getenv("HOME");
            std::string path = home ? (std::string(home) + "/render-perf-diag.csv")
                                    : std::string("/tmp/render-perf-diag.csv");
            std::ofstream f(path, std::ios::app);
            f.seekp(0, std::ios::end);
            if (f.tellp() == std::streampos(0)) {
              f << "t_ms,fps,draw_avg,draw_max,tvb_kb_avg,tvb_kb_max,"
                   "tib_kb_avg,tib_kb_max,cpu_ms_avg,gpu_ms_avg,"
                   // Per-site transient-VB attribution (KB/frame) — path/quad/slug carry
                   // avg+peak; post is negligible (avg only). tvb_kb above is bgfx's
                   // ground-truth total: path+quad+slug+post should track it.
                   "path_kb_avg,path_kb_max,quad_kb_avg,quad_kb_max,"
                   "slug_kb_avg,slug_kb_max,post_kb_avg,"
                   // Window-summed dropped allocations per site = the direct flicker signal.
                   // slug_drops answers whether text flickers too.
                   "path_drops,quad_drops,slug_drops,post_drops\n";
            }
            return f;
          }();

          const auto tMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
          const double frames = (double)accFrames;
          // fps == frames counted in this ~1 s window (window is ~1 s → effective FPS).
          char row[512];
          std::snprintf(row, sizeof(row),
                        "%lld,%llu,%.2f,%u,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,"
                        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                        "%llu,%llu,%llu,%llu\n",
                        (long long)tMs,
                        (unsigned long long)accFrames,
                        (double)accDraw / frames,                    // draw_avg
                        maxDraw,                                     // draw_max
                        ((double)accTvb / frames) / 1024.0,          // tvb_kb_avg
                        (double)maxTvb / 1024.0,                     // tvb_kb_max
                        ((double)accTib / frames) / 1024.0,          // tib_kb_avg
                        (double)maxTib / 1024.0,                     // tib_kb_max
                        accCpuMs / frames,                           // cpu_ms_avg
                        accGpuMs / frames,                           // gpu_ms_avg
                        ((double)accPathVb / frames) / 1024.0,       // path_kb_avg
                        (double)maxPathVb / 1024.0,                  // path_kb_max
                        ((double)accQuadVb / frames) / 1024.0,       // quad_kb_avg
                        (double)maxQuadVb / 1024.0,                  // quad_kb_max
                        ((double)accSlugVb / frames) / 1024.0,       // slug_kb_avg
                        (double)maxSlugVb / 1024.0,                  // slug_kb_max
                        ((double)accPostVb / frames) / 1024.0,       // post_kb_avg
                        (unsigned long long)accPathDrops,            // path_drops
                        (unsigned long long)accQuadDrops,            // quad_drops
                        (unsigned long long)accSlugDrops,            // slug_drops
                        (unsigned long long)accPostDrops);           // post_drops
          csv << row;
          csv.flush();  // 1 Hz flush — cheap, and survives a host crash mid-session.

          // Reset all accumulators for the next window.
          accFrames = 0;
          accDraw = accTvb = accTib = 0;
          accCpuMs = accGpuMs = 0.0;
          maxDraw = 0;
          maxTvb = maxTib = 0;
          accPathVb = accQuadVb = accPostVb = accSlugVb = 0;
          maxPathVb = maxQuadVb = maxSlugVb = 0;
          accPathDrops = accQuadDrops = accSlugDrops = accPostDrops = 0;
          lastEmit = now;
        }
      }
#endif

      if (render_frame_ == 0)
        bgfx::frame();

      render_frame_++;
      FontCache::clearStaleFonts();
      gradient_atlas_.clearStaleGradients();
      image_atlas_.clearStaleImages();
      data_atlas_.clearStaleImages();
    }
    else if (last_skipped_frame_ != render_frame_) {
      last_skipped_frame_ = render_frame_;
      bgfx::frame();
    }
    return submission;
  }

  const Screenshot& Canvas::takeScreenshot() {
    composite_layer_.requestScreenshot();
    default_region_.invalidate();
    submit();
    return composite_layer_.screenshot();
  }

  const Screenshot& Canvas::screenshot() const {
    return composite_layer_.screenshot();
  }

  void Canvas::ensureLayerExists(int layer) {
    int layers_to_add = layer + 1 - layers_.size();
    for (int i = 0; i < layers_to_add; ++i) {
      intermediate_layers_.push_back(std::make_unique<Layer>(&gradient_atlas_));
      intermediate_layers_.back()->setIntermediateLayer(true);
      layers_.push_back(intermediate_layers_.back().get());
    }
  }

  void Canvas::invalidateRectInRegion(IBounds rect, const Region* region, int layer) {
    ensureLayerExists(layer);
    layers_[layer]->invalidateRectInRegion(rect, region);
  }

  void Canvas::addToPackedLayer(Region* region, int layer_index) {
    if (layer_index == 0)
      return;

    ensureLayerExists(layer_index);
    layers_[layer_index]->addPackedRegion(region);
  }

  void Canvas::removeFromPackedLayer(const Region* region, int layer_index) {
    if (layer_index == 0)
      return;

    layers_[layer_index]->removePackedRegion(region);
  }

  void Canvas::changePackedLayer(Region* region, int from, int to) {
    removeFromPackedLayer(region, from);
    addToPackedLayer(region, to);
  }

  Brush Canvas::color(theme::ColorId color_id) {
    if (palette_) {
      Brush result;
      theme::OverrideId last_check;
      for (auto it = state_memory_.rbegin(); it != state_memory_.rend(); ++it) {
        theme::OverrideId override_id = it->palette_override;
        if (override_id.id != last_check.id && palette_->color(override_id, color_id, result))
          return result;
        last_check = override_id;
      }
      if (palette_->color({}, color_id, result))
        return result;
    }

    return Brush::solid(theme::ColorId::defaultColor(color_id));
  }

  float Canvas::value(theme::ValueId value_id) {
    if (palette_) {
      float result = 0.0f;
      theme::OverrideId last_check;
      for (auto it = state_memory_.rbegin(); it != state_memory_.rend(); ++it) {
        theme::OverrideId override_id = it->palette_override;
        if (override_id.id != last_check.id && palette_->value(override_id, value_id, result))
          return result;

        last_check = override_id;
      }
      if (palette_->value({}, value_id, result))
        return result;
    }

    return theme::ValueId::defaultValue(value_id);
  }

  std::vector<std::string> Canvas::debugInfo() const {
    static const std::vector<std::pair<unsigned long long, std::string>> caps_list {
      { BGFX_CAPS_ALPHA_TO_COVERAGE, "Alpha to coverage is supported." },
      { BGFX_CAPS_BLEND_INDEPENDENT, "Blend independent is supported." },
      { BGFX_CAPS_COMPUTE, "Compute shaders are supported." },
      { BGFX_CAPS_CONSERVATIVE_RASTER, "Conservative rasterization is supported." },
      { BGFX_CAPS_DRAW_INDIRECT, "Draw indirect is supported." },
      { BGFX_CAPS_FRAGMENT_DEPTH, "Fragment depth is available in fragment shader." },
      { BGFX_CAPS_FRAGMENT_ORDERING, "Fragment ordering is available in fragment shader." },
      { BGFX_CAPS_GRAPHICS_DEBUGGER, "Graphics debugger is present." },
      { BGFX_CAPS_HDR10, "HDR10 rendering is supported." },
      { BGFX_CAPS_HIDPI, "HiDPI rendering is supported." },
      { BGFX_CAPS_IMAGE_RW, "Image Read/Write is supported." },
      { BGFX_CAPS_INDEX32, "32-bit indices are supported." },
      { BGFX_CAPS_INSTANCING, "Instancing is supported." },
      { BGFX_CAPS_OCCLUSION_QUERY, "Occlusion query is supported." },
      { BGFX_CAPS_RENDERER_MULTITHREADED, "Renderer is on separate thread." },
      { BGFX_CAPS_SWAP_CHAIN, "Multiple windows are supported." },
      { BGFX_CAPS_TEXTURE_2D_ARRAY, "2D texture array is supported." },
      { BGFX_CAPS_TEXTURE_3D, "3D textures are supported." },
      { BGFX_CAPS_TEXTURE_BLIT, "Texture blit is supported." },
      { BGFX_CAPS_TEXTURE_COMPARE_LEQUAL, "Texture compare less equal mode is supported." },
      { BGFX_CAPS_TEXTURE_CUBE_ARRAY, "Cubemap texture array is supported." },
      { BGFX_CAPS_TEXTURE_DIRECT_ACCESS, "CPU direct access to GPU texture memory." },
      { BGFX_CAPS_TEXTURE_READ_BACK, "Read-back texture is supported." },
      { BGFX_CAPS_VERTEX_ATTRIB_HALF, "Vertex attribute half-float is supported." },
      { BGFX_CAPS_VERTEX_ATTRIB_UINT10, "Vertex attribute 10_10_10_2 is supported." },
      { BGFX_CAPS_VERTEX_ID, "Rendering with VertexID only is supported." },
      { BGFX_CAPS_VIEWPORT_LAYER_ARRAY, "Viewport layer is available in vertex shader." },
    };

    const bgfx::Caps* caps = bgfx::getCaps();
    std::vector<std::string> result;
    result.push_back(std::string("Graphics API: ") + bgfx::getRendererName(caps->rendererType));
    float hz = 1.0f / std::max(0.001f, refresh_time_);
    result.push_back("Refresh Rate : " + std::to_string(hz) + " Hz");

    const bgfx::Stats* stats = bgfx::getStats();
    result.push_back("Render wait: " + std::to_string(stats->waitRender));
    result.push_back("Submit wait: " + std::to_string(stats->waitSubmit));
    result.push_back("Draw number: " + std::to_string(stats->numDraw));
    result.push_back("Num views: " + std::to_string(stats->numViews));

    for (auto& cap : caps_list) {
      if (caps->supported & cap.first)
        result.push_back("YES - " + cap.second);
      else
        result.push_back("    - " + cap.second);
    }

    return result;
  }

  void Canvas::updateTime(double time) {
    static constexpr float kRefreshRateSlew = 0.3f;
    delta_time_ = std::max(0.0, time - render_time_);
    render_time_ = time;
    refresh_time_ = (std::min(delta_time_, 1.0) - refresh_time_) * kRefreshRateSlew + refresh_time_;

    for (Layer* layer : layers_)
      layer->setTime(time);
  }
}
