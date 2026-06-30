#include "Graphics/WebGPU/WebGpuCanvas.hpp"

#include <Lambda/Debug/PerfCounters.hpp>
#include <Lambda/Graphics/Image.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lambdaui::webgpu {

namespace {

bool containsFormat(WGPUSurfaceCapabilities const& capabilities, WGPUTextureFormat format) noexcept {
  for (std::size_t i = 0; i < capabilities.formatCount; ++i) {
    if (capabilities.formats[i] == format) {
      return true;
    }
  }
  return false;
}

bool containsPresentMode(WGPUSurfaceCapabilities const& capabilities, WGPUPresentMode mode) noexcept {
  for (std::size_t i = 0; i < capabilities.presentModeCount; ++i) {
    if (capabilities.presentModes[i] == mode) {
      return true;
    }
  }
  return false;
}

bool containsAlphaMode(WGPUSurfaceCapabilities const& capabilities, WGPUCompositeAlphaMode mode) noexcept {
  for (std::size_t i = 0; i < capabilities.alphaModeCount; ++i) {
    if (capabilities.alphaModes[i] == mode) {
      return true;
    }
  }
  return false;
}

WGPUColor toWgpuColor(Color color) noexcept {
  return WGPUColor{
      .r = static_cast<double>(color.r),
      .g = static_cast<double>(color.g),
      .b = static_cast<double>(color.b),
      .a = static_cast<double>(color.a),
  };
}

class WebGpuCanvas final : public Canvas {
public:
  WebGpuCanvas(WebGpuContext context,
               WGPUSurface surface,
               unsigned int handle,
               TextSystem& textSystem,
               WebGpuCanvasOptions options)
      : context_(std::move(context)),
        surface_(surface),
        handle_(handle),
        textSystem_(textSystem),
        transparentSurface_(options.transparentSurface) {
    (void)textSystem_;
    if (!surface_) {
      throw std::runtime_error("Lambda WebGPU: window canvas requires a surface");
    }
    context_.initializeDevice(surface_);
    configureSurface();
    resetState();
  }

  ~WebGpuCanvas() override {
    releaseFrameObjects();
    if (surface_) {
      wgpuSurfaceUnconfigure(surface_);
      wgpuSurfaceRelease(surface_);
      surface_ = nullptr;
    }
  }

  Backend backend() const noexcept override { return Backend::WebGPU; }
  unsigned int windowHandle() const override { return handle_; }

  void resize(int width, int height) override {
    size_.width = static_cast<float>(std::max(1, width));
    size_.height = static_cast<float>(std::max(1, height));
    configureSurface();
    resetState();
  }

  void updateDpiScale(float scaleX, float scaleY) override {
    dpiScaleX_ = scaleX > 0.f ? scaleX : 1.f;
    dpiScaleY_ = scaleY > 0.f ? scaleY : 1.f;
  }

  float dpiScale() const noexcept override {
    return std::max(dpiScaleX_, dpiScaleY_);
  }

  void beginFrame() override {
    releaseFrameObjects();
    resetState();
    frameActive_ = true;
    clearColor_ = Colors::transparent;
  }

  void present() override {
    if (!frameActive_) {
      return;
    }
    debug::perf::ScopedTimer timer(debug::perf::TimedMetric::CanvasPresent);
    ensureFrameObjects();
    encodeClearPass();
    WGPUCommandBufferDescriptor commandBufferDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandBufferDescriptor.label = stringView("LambdaUI WebGPU Frame Commands");
    WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(commandEncoder_, &commandBufferDescriptor);
    if (!commandBuffer) {
      throw std::runtime_error("Lambda WebGPU: failed to finish command buffer");
    }
    wgpuQueueSubmit(context_.queue(), 1, &commandBuffer);
    wgpuCommandBufferRelease(commandBuffer);
    wgpuSurfacePresent(surface_);
    releaseFrameObjects();
    frameActive_ = false;
    debug::perf::recordPresentedFrame();
  }

  void save() override {
    stack_.push_back(state_);
  }

  void restore() override {
    if (stack_.empty()) {
      return;
    }
    state_ = stack_.back();
    stack_.pop_back();
  }

  void setTransform(Mat3 const& transform) override {
    state_.transform = transform;
  }

  void transform(Mat3 const& transform) override {
    state_.transform = state_.transform * transform;
  }

  void translate(Point offset) override { transform(Mat3::translate(offset)); }
  void translate(float x, float y) override { translate(Point{x, y}); }
  void scale(float sx, float sy) override { transform(Mat3::scale(sx, sy)); }
  void scale(float s) override { scale(s, s); }
  void rotate(float radians) override { transform(Mat3::rotate(radians)); }
  void rotate(float radians, Point pivot) override { transform(Mat3::rotate(radians, pivot)); }
  Mat3 currentTransform() const override { return state_.transform; }

  void clipRect(Rect rect, CornerRadius const& = CornerRadius{}, bool = false) override {
    state_.clip = intersectRects(state_.clip, rect);
  }

  Rect clipBounds() const override { return state_.clip; }
  bool quickReject(Rect rect) const override { return !state_.clip.intersects(rect); }

  void setOpacity(float opacity) override {
    state_.opacity = std::clamp(opacity, 0.f, 1.f);
  }

  float opacity() const override { return state_.opacity; }

  void setBlendMode(BlendMode mode) override {
    state_.blendMode = mode;
  }

  BlendMode blendMode() const override { return state_.blendMode; }

  void drawRect(Rect const&, CornerRadius const&, FillStyle const&, StrokeStyle const&, ShadowStyle const&) override {}
  void drawLine(Point, Point, StrokeStyle const&) override {}
  void drawPath(Path const&, FillStyle const&, StrokeStyle const&, ShadowStyle const&) override {}
  void drawCircle(Point, float, FillStyle const&, StrokeStyle const&) override {}
  void drawTextLayout(TextLayout const&, Point) override {}
  void drawImage(Image const&, Rect const&, Rect const&, CornerRadius const&, float) override {}
  void drawImageTiled(Image const&, Rect const&, CornerRadius const&, float) override {}

  void* gpuDevice() const override {
    return reinterpret_cast<void*>(context_.device());
  }

  bool requestNextFrameCapture() override { return false; }

  bool takeCapturedFrame(std::vector<std::uint8_t>& out, std::uint32_t& width, std::uint32_t& height) override {
    out.clear();
    width = 0;
    height = 0;
    return false;
  }

  std::unique_ptr<RecordedOps> beginRecordedOpsCapture() override { return nullptr; }
  void endRecordedOpsCapture() override {}

  std::unique_ptr<scenegraph::PreparedRenderOps> finalizeRecordedOps(std::unique_ptr<RecordedOps>) override {
    return nullptr;
  }

  bool replayRecordedOps(RecordedOps const&, RecordedOpsReplaySlice const* = nullptr) override { return false; }
  bool replayRecordedLocalOps(RecordedOps const&, RecordedOpsReplaySlice const* = nullptr) override { return false; }

  void clear(Color color = Colors::transparent) override {
    clearColor_ = color;
  }

  void setResizeBoundsHint(int logicalWidth, int logicalHeight) noexcept {
    resizeBoundsWidth_ = std::max(0, logicalWidth);
    resizeBoundsHeight_ = std::max(0, logicalHeight);
  }

  bool setTransparentSurface(bool enabled) {
    if (transparentSurface_ == enabled) {
      return true;
    }
    transparentSurface_ = enabled;
    configureSurface();
    return true;
  }

  bool usesMailboxPresentMode() const noexcept {
    return presentMode_ == WGPUPresentMode_Mailbox;
  }

private:
  struct State {
    Mat3 transform = Mat3::identity();
    Rect clip = Rect::sharp(0.f, 0.f, 0.f, 0.f);
    float opacity = 1.f;
    BlendMode blendMode = BlendMode::SrcOver;
  };

  Rect viewportBounds() const noexcept {
    return Rect::sharp(0.f, 0.f, size_.width, size_.height);
  }

  static Rect intersectRects(Rect a, Rect b) noexcept {
    float const x0 = std::max(a.x, b.x);
    float const y0 = std::max(a.y, b.y);
    float const x1 = std::min(a.x + a.width, b.x + b.width);
    float const y1 = std::min(a.y + a.height, b.y + b.height);
    return Rect::sharp(x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0));
  }

  void resetState() {
    state_ = State{.clip = viewportBounds()};
    stack_.clear();
  }

  void configureSurface() {
    if (!surface_ || !context_.hasDevice()) {
      return;
    }

    WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(surface_, context_.adapter(), &capabilities) != WGPUStatus_Success) {
      throw std::runtime_error("Lambda WebGPU: failed to query surface capabilities");
    }

    if (containsFormat(capabilities, WGPUTextureFormat_BGRA8Unorm)) {
      surfaceFormat_ = WGPUTextureFormat_BGRA8Unorm;
    } else if (capabilities.formatCount > 0) {
      surfaceFormat_ = capabilities.formats[0];
    } else {
      surfaceFormat_ = WGPUTextureFormat_RGBA8Unorm;
    }

    presentMode_ = containsPresentMode(capabilities, WGPUPresentMode_Fifo)
                       ? WGPUPresentMode_Fifo
                       : (capabilities.presentModeCount > 0 ? capabilities.presentModes[0] : WGPUPresentMode_Undefined);

    WGPUCompositeAlphaMode alphaMode = transparentSurface_ ? WGPUCompositeAlphaMode_Premultiplied
                                                           : WGPUCompositeAlphaMode_Opaque;
    if (!containsAlphaMode(capabilities, alphaMode)) {
      alphaMode = capabilities.alphaModeCount > 0 ? capabilities.alphaModes[0] : WGPUCompositeAlphaMode_Auto;
    }

    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = context_.device();
    config.format = surfaceFormat_;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.width)));
    config.height = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.height)));
    config.presentMode = presentMode_;
    config.alphaMode = alphaMode;
    wgpuSurfaceConfigure(surface_, &config);

    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
  }

  void ensureFrameObjects() {
    if (currentView_) {
      return;
    }

    WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture);
    if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
      throw std::runtime_error("Lambda WebGPU: failed to acquire surface texture");
    }
    currentTexture_ = surfaceTexture.texture;
    if (!currentTexture_) {
      throw std::runtime_error("Lambda WebGPU: acquired surface texture is null");
    }

    WGPUTextureViewDescriptor viewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    viewDescriptor.label = stringView("LambdaUI WebGPU Surface View");
    currentView_ = wgpuTextureCreateView(currentTexture_, &viewDescriptor);
    if (!currentView_) {
      throw std::runtime_error("Lambda WebGPU: failed to create frame texture view");
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("LambdaUI WebGPU Command Encoder");
    commandEncoder_ = wgpuDeviceCreateCommandEncoder(context_.device(), &encoderDescriptor);
    if (!commandEncoder_) {
      throw std::runtime_error("Lambda WebGPU: failed to create command encoder");
    }
  }

  void encodeClearPass() {
    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = currentView_;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = toWgpuColor(clearColor_);

    WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("LambdaUI WebGPU Clear Pass");
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &colorAttachment;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(commandEncoder_, &passDescriptor);
    if (!pass) {
      throw std::runtime_error("Lambda WebGPU: failed to begin clear render pass");
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
  }

  void releaseFrameObjects() noexcept {
    if (commandEncoder_) {
      wgpuCommandEncoderRelease(commandEncoder_);
      commandEncoder_ = nullptr;
    }
    if (currentView_) {
      wgpuTextureViewRelease(currentView_);
      currentView_ = nullptr;
    }
    if (currentTexture_) {
      wgpuTextureRelease(currentTexture_);
      currentTexture_ = nullptr;
    }
  }

  WebGpuContext context_;
  WGPUSurface surface_ = nullptr;
  unsigned int handle_ = 0;
  TextSystem& textSystem_;
  bool transparentSurface_ = false;
  Size size_{1.f, 1.f};
  float dpiScaleX_ = 1.f;
  float dpiScaleY_ = 1.f;
  int resizeBoundsWidth_ = 0;
  int resizeBoundsHeight_ = 0;
  State state_{};
  std::vector<State> stack_{};
  Color clearColor_ = Colors::transparent;
  WGPUTextureFormat surfaceFormat_ = WGPUTextureFormat_Undefined;
  WGPUPresentMode presentMode_ = WGPUPresentMode_Undefined;
  WGPUTexture currentTexture_ = nullptr;
  WGPUTextureView currentView_ = nullptr;
  WGPUCommandEncoder commandEncoder_ = nullptr;
  bool frameActive_ = false;
};

} // namespace

std::unique_ptr<Canvas> createWebGpuCanvas(WebGpuContext context,
                                           WGPUSurface surface,
                                           unsigned int handle,
                                           TextSystem& textSystem,
                                           WebGpuCanvasOptions options) {
  return std::make_unique<WebGpuCanvas>(std::move(context), surface, handle, textSystem, options);
}

void setWebGpuCanvasResizeBoundsHint(Canvas* canvas, int logicalWidth, int logicalHeight) {
  if (auto* webGpuCanvas = dynamic_cast<WebGpuCanvas*>(canvas)) {
    webGpuCanvas->setResizeBoundsHint(logicalWidth, logicalHeight);
  }
}

bool setWebGpuCanvasTransparentSurface(Canvas* canvas, bool enabled) {
  auto* webGpuCanvas = dynamic_cast<WebGpuCanvas*>(canvas);
  return webGpuCanvas ? webGpuCanvas->setTransparentSurface(enabled) : false;
}

bool webGpuCanvasUsesMailboxPresentMode(Canvas* canvas) {
  auto* webGpuCanvas = dynamic_cast<WebGpuCanvas*>(canvas);
  return webGpuCanvas && webGpuCanvas->usesMailboxPresentMode();
}

} // namespace lambdaui::webgpu
