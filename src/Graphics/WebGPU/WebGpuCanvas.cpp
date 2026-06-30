#include "WebGpuCanvas.hpp"

#include "WebGpuContext.hpp"
#include "Graphics/CanvasGeometry.hpp"
#include "Graphics/PathFlattener.hpp"
#include "Graphics/PathGradient.hpp"

#include <Lambda/Graphics/Image.hpp>
#include <Lambda/Graphics/TextSystem.hpp>
#include <Lambda/Graphics/WebGpuContext.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lambdaui::webgpu {

WGPUTextureFormat textureFormatForPixelFormat(Image::PixelFormat format) noexcept {
  switch (format) {
    case Image::PixelFormat::Rgba8888:
      return WGPUTextureFormat_RGBA8Unorm;
    case Image::PixelFormat::Bgra8888:
      return WGPUTextureFormat_BGRA8Unorm;
  }
  return WGPUTextureFormat_RGBA8Unorm;
}

class WebGpuImage final : public Image {
public:
  WebGpuImage(std::uint32_t width,
              std::uint32_t height,
              std::vector<std::uint8_t> pixels,
              PixelFormat format,
              bool premultiplied)
      : size_{static_cast<float>(width), static_cast<float>(height)},
        width_(width),
        height_(height),
        pixels_(std::move(pixels)),
        pixelFormat_(format),
        premultiplied_(premultiplied) {}

  WebGpuImage(std::uint32_t width, std::uint32_t height, WGPUTextureView textureView, bool premultiplied)
      : size_{static_cast<float>(width), static_cast<float>(height)},
        width_(width),
        height_(height),
        premultiplied_(premultiplied),
        externalTextureView_(true),
        textureView_(textureView) {
    if (textureView_) {
      wgpuTextureViewAddRef(textureView_);
    }
  }

  ~WebGpuImage() override {
    releaseGpuObjects();
  }

  Size size() const override { return size_; }
  bool premultipliedAlpha() const noexcept override { return premultiplied_; }

  bool updateRgbaPixels(std::span<std::uint8_t const> rgbaPixels, void* gpuDevice) override {
    return updatePixels(rgbaPixels, PixelFormat::Rgba8888, gpuDevice);
  }

  bool updatePixels(std::span<std::uint8_t const> pixels, PixelFormat format, void* gpuDevice) override {
    if (externalTextureView_) {
      return false;
    }
    std::size_t const expectedSize = static_cast<std::size_t>(width_) * height_ * 4u;
    if (width_ == 0 || height_ == 0 || pixels.size() != expectedSize) {
      return false;
    }
    if (format != pixelFormat_) {
      releaseGpuObjects();
      pixelFormat_ = format;
    }
    pixels_.assign(pixels.begin(), pixels.end());
    textureDirty_ = true;
    if (gpuDevice) {
      WGPUQueue queue = wgpuDeviceGetQueue(static_cast<WGPUDevice>(gpuDevice));
      if (!queue) {
        return false;
      }
      ensureTexture(static_cast<WGPUDevice>(gpuDevice), queue);
      wgpuQueueRelease(queue);
    }
    return true;
  }

  bool updatePixelsRegion(std::span<std::uint8_t const> pixels,
                          PixelFormat format,
                          std::uint32_t x,
                          std::uint32_t y,
                          std::uint32_t width,
                          std::uint32_t height,
                          void* gpuDevice = nullptr,
                          std::uint32_t sourceBytesPerRow = 0) override {
    if (externalTextureView_) {
      return false;
    }
    if (format != pixelFormat_ || x + width > width_ || y + height > height_ || width == 0 || height == 0) {
      return false;
    }
    if (sourceBytesPerRow == 0) {
      sourceBytesPerRow = width * 4u;
    }
    std::size_t const requiredSize =
        static_cast<std::size_t>(sourceBytesPerRow) * (height - 1u) + static_cast<std::size_t>(width) * 4u;
    if (pixels.size() < requiredSize) {
      return false;
    }
    for (std::uint32_t row = 0; row < height; ++row) {
      std::uint8_t const* src = pixels.data() + static_cast<std::size_t>(row) * sourceBytesPerRow;
      std::uint8_t* dst = pixels_.data() + (static_cast<std::size_t>(y + row) * width_ + x) * 4u;
      std::memcpy(dst, src, static_cast<std::size_t>(width) * 4u);
    }
    textureDirty_ = true;
    if (gpuDevice) {
      WGPUQueue queue = wgpuDeviceGetQueue(static_cast<WGPUDevice>(gpuDevice));
      if (!queue) {
        return false;
      }
      ensureTexture(static_cast<WGPUDevice>(gpuDevice), queue);
      wgpuQueueRelease(queue);
    }
    return true;
  }

  WGPUTextureView textureView(WGPUDevice device, WGPUQueue queue) const {
    ensureTexture(device, queue);
    return textureView_;
  }

private:
  void ensureTexture(WGPUDevice device, WGPUQueue queue) const {
    if (externalTextureView_) {
      if (!textureView_) {
        throw std::runtime_error("Lambda WebGPU: external image texture view is unavailable");
      }
      return;
    }
    if (!device || !queue || pixels_.empty()) {
      throw std::runtime_error("Lambda WebGPU: image upload requires a device, queue, and pixels");
    }
    if (device_ != device) {
      releaseGpuObjects();
      device_ = device;
      wgpuDeviceAddRef(device_);
      textureDirty_ = true;
    }
    if (!texture_) {
      WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
      descriptor.label = stringView("LambdaUI WebGPU Image Texture");
      descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
      descriptor.dimension = WGPUTextureDimension_2D;
      descriptor.size = WGPUExtent3D{
          .width = width_,
          .height = height_,
          .depthOrArrayLayers = 1,
      };
      descriptor.format = textureFormatForPixelFormat(pixelFormat_);
      descriptor.mipLevelCount = 1;
      descriptor.sampleCount = 1;
      texture_ = wgpuDeviceCreateTexture(device_, &descriptor);
      if (!texture_) {
        throw std::runtime_error("Lambda WebGPU: failed to create image texture");
      }

      WGPUTextureViewDescriptor viewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
      viewDescriptor.label = stringView("LambdaUI WebGPU Image View");
      textureView_ = wgpuTextureCreateView(texture_, &viewDescriptor);
      if (!textureView_) {
        throw std::runtime_error("Lambda WebGPU: failed to create image texture view");
      }
      textureDirty_ = true;
    }
    if (textureDirty_) {
      WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
      destination.texture = texture_;
      WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
      layout.bytesPerRow = width_ * 4u;
      layout.rowsPerImage = height_;
      WGPUExtent3D extent = WGPU_EXTENT_3D_INIT;
      extent.width = width_;
      extent.height = height_;
      extent.depthOrArrayLayers = 1;
      wgpuQueueWriteTexture(queue, &destination, pixels_.data(), pixels_.size(), &layout, &extent);
      textureDirty_ = false;
    }
  }

  void releaseGpuObjects() const noexcept {
    if (textureView_) {
      wgpuTextureViewRelease(textureView_);
      textureView_ = nullptr;
    }
    if (texture_) {
      wgpuTextureRelease(texture_);
      texture_ = nullptr;
    }
    if (device_) {
      wgpuDeviceRelease(device_);
      device_ = nullptr;
    }
  }

  Size size_{};
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::vector<std::uint8_t> pixels_;
  PixelFormat pixelFormat_ = PixelFormat::Rgba8888;
  bool premultiplied_ = false;
  bool externalTextureView_ = false;

  mutable WGPUDevice device_ = nullptr;
  mutable WGPUTexture texture_ = nullptr;
  mutable WGPUTextureView textureView_ = nullptr;
  mutable bool textureDirty_ = true;
};

namespace {

inline constexpr std::size_t kRoundedClipMaskCapacity = 4;
inline constexpr std::size_t kRoundedClipEntryCount = kRoundedClipMaskCapacity * 2;
inline constexpr std::size_t kWebGpuBlendModePipelineCount = static_cast<std::size_t>(BlendMode::Xor) + 1u;
inline constexpr std::uint32_t kWebGpuCopyBytesPerRowAlignment = 256;
inline constexpr std::uint32_t kWebGpuBackdropBlurMaxRadiusPx = 64;

struct WebGpuRectInstance {
  float rect[4]{};
  float axisX[4]{};
  float axisY[4]{};
  float radii[4]{};
  float fill0[4]{};
  float fill1[4]{};
  float fill2[4]{};
  float fill3[4]{};
  float stops[4]{};
  float gradient[4]{};
  float stroke[4]{};
  float params[4]{};
  float clipHeader[4]{};
  float clipEntries[kRoundedClipEntryCount][4]{};
};

struct WebGpuFrameUniforms {
  float viewport[2]{};
  float padding[2]{};
};

static_assert(sizeof(WebGpuFrameUniforms) == 16);

struct WebGpuBlurUniforms {
  float direction[2]{};
  float radiusPx = 0.f;
  float padding = 0.f;
};

static_assert(sizeof(WebGpuBlurUniforms) == 16);

struct WebGpuQuadInstance {
  float rect[4]{};
  float axisX[4]{};
  float axisY[4]{};
  float uv[4]{};
  float color[4]{};
  float radii[4]{};
  float clipHeader[4]{};
  float clipEntries[kRoundedClipEntryCount][4]{};
};

struct WebGpuPathVertex {
  float x = 0.f;
  float y = 0.f;
  float color[4]{};
  float viewport[2]{};
  float clipHeader[4]{};
  float clipEntries[kRoundedClipEntryCount][4]{};
};

static_assert(sizeof(WebGpuPathVertex) == 176);

struct WebGpuRoundedClipState {
  Rect rect{};
  CornerRadius radii{};
};

struct WebGpuMapWaitState {
  WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
  bool done = false;
};

char const kRectShaderWgsl[] = R"wgsl(
struct RectInstance {
  rect: vec4<f32>,
  axisX: vec4<f32>,
  axisY: vec4<f32>,
  radii: vec4<f32>,
  fill0: vec4<f32>,
  fill1: vec4<f32>,
  fill2: vec4<f32>,
  fill3: vec4<f32>,
  stops: vec4<f32>,
  gradient: vec4<f32>,
  stroke: vec4<f32>,
  params: vec4<f32>,
  clipHeader: vec4<f32>,
  clipEntries: array<vec4<f32>, 8>,
};

struct Rects {
  instances: array<RectInstance>,
};

struct FrameUniforms {
  viewport: vec2<f32>,
  padding: vec2<f32>,
};

@group(0) @binding(0) var<storage, read> rects: Rects;
@group(0) @binding(1) var<uniform> frame: FrameUniforms;

struct VertexOut {
  @builtin(position) position: vec4<f32>,
  @location(0) local: vec2<f32>,
  @location(1) @interpolate(flat) instance: u32,
  @location(2) world: vec2<f32>,
};

fn unit_vertex(index: u32) -> vec2<f32> {
  switch index {
    case 0u: { return vec2<f32>(0.0, 0.0); }
    case 1u: { return vec2<f32>(1.0, 0.0); }
    case 2u: { return vec2<f32>(1.0, 1.0); }
    case 3u: { return vec2<f32>(0.0, 0.0); }
    case 4u: { return vec2<f32>(1.0, 1.0); }
    default: { return vec2<f32>(0.0, 1.0); }
  }
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32,
           @builtin(instance_index) instanceIndex: u32) -> VertexOut {
  let r = rects.instances[instanceIndex];
  let unit = unit_vertex(vertexIndex);
  let size = max(r.rect.zw, vec2<f32>(0.000001, 0.000001));
  let pad = max(r.params.z * 0.5, 1.0);
  let local = unit * (size + vec2<f32>(pad * 2.0, pad * 2.0)) - vec2<f32>(pad, pad);
  let axisUnit = local / size;
  let world = r.axisX.xy + axisUnit.x * r.axisX.zw + axisUnit.y * r.axisY.xy;
  let ndc = vec2<f32>(world.x / frame.viewport.x * 2.0 - 1.0,
                      1.0 - world.y / frame.viewport.y * 2.0);
  var out: VertexOut;
  out.position = vec4<f32>(ndc, 0.0, 1.0);
  out.local = local;
  out.instance = instanceIndex;
  out.world = world;
  return out;
}

fn rounded_rect_sdf(p: vec2<f32>, halfSize: vec2<f32>, radii: vec4<f32>) -> f32 {
  let r = select(select(radii.x, radii.y, p.x > 0.0),
                 select(radii.w, radii.z, p.x > 0.0),
                 p.y > 0.0);
  let q = abs(p) - halfSize + vec2<f32>(r, r);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;
}

fn distance_coverage(d: f32) -> f32 {
  let aa = max(0.75 * length(vec2<f32>(dpdx(d), dpdy(d))), 0.0001);
  return 1.0 - smoothstep(-aa, aa, d);
}

fn rounded_clip_coverage(r: RectInstance, world: vec2<f32>) -> f32 {
  let count = clamp(i32(r.clipHeader.x + 0.5), 0, 4);
  var coverage = 1.0;
  for (var i = 0; i < count; i = i + 1) {
    let clipRect = r.clipEntries[u32(i * 2)];
    let clipRadii = r.clipEntries[u32(i * 2 + 1)];
    if (clipRect.z <= 0.0 || clipRect.w <= 0.0) {
      continue;
    }
    let halfSize = clipRect.zw * 0.5;
    let local = world - clipRect.xy - halfSize;
    coverage = coverage * distance_coverage(rounded_rect_sdf(local, halfSize, clipRadii));
  }
  return coverage;
}

fn sample_stops(r: RectInstance, tRaw: f32) -> vec4<f32> {
  let t = clamp(tRaw, 0.0, 1.0);
  let count = i32(r.params.y + 0.5);
  if (count <= 1 || t <= r.stops.x) {
    return r.fill0;
  }
  if (count <= 2 || t <= r.stops.y) {
    let span = max(0.000001, r.stops.y - r.stops.x);
    return mix(r.fill0, r.fill1, (t - r.stops.x) / span);
  }
  if (count <= 3 || t <= r.stops.z) {
    let span = max(0.000001, r.stops.z - r.stops.y);
    return mix(r.fill1, r.fill2, (t - r.stops.y) / span);
  }
  let span = max(0.000001, r.stops.w - r.stops.z);
  return mix(r.fill2, r.fill3, (t - r.stops.z) / span);
}

fn fill_color(r: RectInstance, p: vec2<f32>) -> vec4<f32> {
  let fillType = i32(r.params.x + 0.5);
  let uv = p / max(r.rect.zw, vec2<f32>(0.000001, 0.000001));
  if (fillType == 1) {
    let a = r.gradient.xy;
    let b = r.gradient.zw;
    let d = b - a;
    return sample_stops(r, dot(uv - a, d) / max(dot(d, d), 0.000001));
  }
  if (fillType == 2) {
    return sample_stops(r, length(uv - r.gradient.xy) / max(r.gradient.z, 0.000001));
  }
  if (fillType == 3) {
    let angle = atan2(uv.y - r.gradient.y, uv.x - r.gradient.x) - r.gradient.z;
    return sample_stops(r, fract(angle / 6.28318530718));
  }
  return r.fill0;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
  let r = rects.instances[in.instance];
  let size = r.rect.zw;
  let halfSize = size * 0.5;
  let local = in.local - halfSize;
  let d = rounded_rect_sdf(local, halfSize, r.radii);
  let aa = max(0.75 * length(vec2<f32>(dpdx(d), dpdy(d))), 0.0001);
  let fillCoverage = 1.0 - smoothstep(-aa, aa, d);
  var strokeCoverage = 0.0;
  if (r.params.z > 0.0) {
    strokeCoverage = 1.0 - smoothstep(-aa, aa, abs(d) - r.params.z * 0.5);
  }
  let clipCoverage = rounded_clip_coverage(r, in.world);
  let shapeCoverage = max(fillCoverage, strokeCoverage) * clipCoverage;
  if (shapeCoverage <= 0.001) {
    discard;
  }
  let fill = fill_color(r, in.local);
  let fillAlpha = fill.a * fillCoverage;
  let strokeAlpha = r.stroke.a * strokeCoverage;
  let outAlpha = (strokeAlpha + fillAlpha * (1.0 - strokeAlpha)) * r.params.w * clipCoverage;
  if (outAlpha <= 0.001) {
    discard;
  }
  let rgbAlpha = max(strokeAlpha + fillAlpha * (1.0 - strokeAlpha), 0.000001);
  let rgb = (r.stroke.rgb * strokeAlpha + fill.rgb * fillAlpha * (1.0 - strokeAlpha)) / rgbAlpha;
  return vec4<f32>(rgb, outAlpha);
}
)wgsl";

char const kImageShaderWgsl[] = R"wgsl(
struct QuadInstance {
  rect: vec4<f32>,
  axisX: vec4<f32>,
  axisY: vec4<f32>,
  uv: vec4<f32>,
  color: vec4<f32>,
  radii: vec4<f32>,
  clipHeader: vec4<f32>,
  clipEntries: array<vec4<f32>, 8>,
};

struct Quads {
  instances: array<QuadInstance>,
};

struct FrameUniforms {
  viewport: vec2<f32>,
  padding: vec2<f32>,
};

@group(0) @binding(0) var<storage, read> quads: Quads;
@group(0) @binding(1) var<uniform> frame: FrameUniforms;
@group(1) @binding(0) var imageSampler: sampler;
@group(1) @binding(1) var imageTexture: texture_2d<f32>;

struct VertexOut {
  @builtin(position) position: vec4<f32>,
  @location(0) local: vec2<f32>,
  @location(1) uv: vec2<f32>,
  @location(2) color: vec4<f32>,
  @location(3) @interpolate(flat) instance: u32,
  @location(4) world: vec2<f32>,
};

fn unit_vertex(index: u32) -> vec2<f32> {
  switch index {
    case 0u: { return vec2<f32>(0.0, 0.0); }
    case 1u: { return vec2<f32>(1.0, 0.0); }
    case 2u: { return vec2<f32>(1.0, 1.0); }
    case 3u: { return vec2<f32>(0.0, 0.0); }
    case 4u: { return vec2<f32>(1.0, 1.0); }
    default: { return vec2<f32>(0.0, 1.0); }
  }
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32,
           @builtin(instance_index) instanceIndex: u32) -> VertexOut {
  let q = quads.instances[instanceIndex];
  let unit = unit_vertex(vertexIndex);
  let size = max(q.rect.zw, vec2<f32>(0.000001, 0.000001));
  let local = unit * size;
  let axisUnit = local / size;
  let world = q.axisX.xy + axisUnit.x * q.axisX.zw + axisUnit.y * q.axisY.xy;
  let ndc = vec2<f32>(world.x / frame.viewport.x * 2.0 - 1.0,
                      1.0 - world.y / frame.viewport.y * 2.0);
  var out: VertexOut;
  out.position = vec4<f32>(ndc, 0.0, 1.0);
  out.local = local;
  out.uv = mix(q.uv.xy, q.uv.zw, unit);
  out.color = q.color;
  out.instance = instanceIndex;
  out.world = world;
  return out;
}

fn rounded_rect_sdf(p: vec2<f32>, halfSize: vec2<f32>, radii: vec4<f32>) -> f32 {
  let r = select(select(radii.x, radii.y, p.x > 0.0),
                 select(radii.w, radii.z, p.x > 0.0),
                 p.y > 0.0);
  let q = abs(p) - halfSize + vec2<f32>(r, r);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;
}

fn distance_coverage(d: f32) -> f32 {
  let aa = max(0.75 * length(vec2<f32>(dpdx(d), dpdy(d))), 0.0001);
  return 1.0 - smoothstep(-aa, aa, d);
}

fn rounded_clip_coverage(q: QuadInstance, world: vec2<f32>) -> f32 {
  let count = clamp(i32(q.clipHeader.x + 0.5), 0, 4);
  var coverage = 1.0;
  for (var i = 0; i < count; i = i + 1) {
    let clipRect = q.clipEntries[u32(i * 2)];
    let clipRadii = q.clipEntries[u32(i * 2 + 1)];
    if (clipRect.z <= 0.0 || clipRect.w <= 0.0) {
      continue;
    }
    let halfSize = clipRect.zw * 0.5;
    let local = world - clipRect.xy - halfSize;
    coverage = coverage * distance_coverage(rounded_rect_sdf(local, halfSize, clipRadii));
  }
  return coverage;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
  let q = quads.instances[in.instance];
  let halfSize = q.rect.zw * 0.5;
  let d = rounded_rect_sdf(in.local - halfSize, halfSize, q.radii);
  let coverage = distance_coverage(d) * rounded_clip_coverage(q, in.world);
  if (coverage <= 0.001) {
    discard;
  }
  var sampled = textureSample(imageTexture, imageSampler, in.uv) * in.color;
  sampled.a = sampled.a * coverage;
  if (sampled.a <= 0.001) {
    discard;
  }
  return sampled;
}
)wgsl";

char const kBlurShaderWgsl[] = R"wgsl(
struct BlurUniforms {
  direction: vec2<f32>,
  radiusPx: f32,
  padding: f32,
};

@group(0) @binding(0) var blurSampler: sampler;
@group(0) @binding(1) var sourceTexture: texture_2d<f32>;
@group(0) @binding(2) var<uniform> blur: BlurUniforms;

struct VertexOut {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

fn unit_vertex(index: u32) -> vec2<f32> {
  switch index {
    case 0u: { return vec2<f32>(0.0, 0.0); }
    case 1u: { return vec2<f32>(1.0, 0.0); }
    case 2u: { return vec2<f32>(1.0, 1.0); }
    case 3u: { return vec2<f32>(0.0, 0.0); }
    case 4u: { return vec2<f32>(1.0, 1.0); }
    default: { return vec2<f32>(0.0, 1.0); }
  }
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex: u32) -> VertexOut {
  let unit = unit_vertex(vertexIndex);
  var out: VertexOut;
  out.position = vec4<f32>(unit.x * 2.0 - 1.0, 1.0 - unit.y * 2.0, 0.0, 1.0);
  out.uv = unit;
  return out;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
  let textureSize = textureDimensions(sourceTexture);
  let dims = max(vec2<f32>(f32(textureSize.x), f32(textureSize.y)), vec2<f32>(1.0, 1.0));
  let step = blur.direction * max(blur.radiusPx, 1.0) / 4.0 / dims;
  var color = textureSample(sourceTexture, blurSampler, in.uv) * 0.2270270270;
  color = color + textureSample(sourceTexture, blurSampler, in.uv + step * 1.0) * 0.1945945946;
  color = color + textureSample(sourceTexture, blurSampler, in.uv - step * 1.0) * 0.1945945946;
  color = color + textureSample(sourceTexture, blurSampler, in.uv + step * 2.0) * 0.1216216216;
  color = color + textureSample(sourceTexture, blurSampler, in.uv - step * 2.0) * 0.1216216216;
  color = color + textureSample(sourceTexture, blurSampler, in.uv + step * 3.0) * 0.0540540541;
  color = color + textureSample(sourceTexture, blurSampler, in.uv - step * 3.0) * 0.0540540541;
  color = color + textureSample(sourceTexture, blurSampler, in.uv + step * 4.0) * 0.0162162162;
  color = color + textureSample(sourceTexture, blurSampler, in.uv - step * 4.0) * 0.0162162162;
  return color;
}
)wgsl";

char const kPathShaderWgsl[] = R"wgsl(
struct VertexOut {
  @builtin(position) position: vec4<f32>,
  @location(0) color: vec4<f32>,
  @location(1) world: vec2<f32>,
  @location(2) @interpolate(flat) clipHeader: vec4<f32>,
  @location(3) @interpolate(flat) clip0: vec4<f32>,
  @location(4) @interpolate(flat) clip1: vec4<f32>,
  @location(5) @interpolate(flat) clip2: vec4<f32>,
  @location(6) @interpolate(flat) clip3: vec4<f32>,
  @location(7) @interpolate(flat) clip4: vec4<f32>,
  @location(8) @interpolate(flat) clip5: vec4<f32>,
  @location(9) @interpolate(flat) clip6: vec4<f32>,
  @location(10) @interpolate(flat) clip7: vec4<f32>,
};

@vertex
fn vs_main(@location(0) position: vec2<f32>,
           @location(1) color: vec4<f32>,
           @location(2) viewport: vec2<f32>,
           @location(3) clipHeader: vec4<f32>,
           @location(4) clip0: vec4<f32>,
           @location(5) clip1: vec4<f32>,
           @location(6) clip2: vec4<f32>,
           @location(7) clip3: vec4<f32>,
           @location(8) clip4: vec4<f32>,
           @location(9) clip5: vec4<f32>,
           @location(10) clip6: vec4<f32>,
           @location(11) clip7: vec4<f32>) -> VertexOut {
  let safeViewport = max(viewport, vec2<f32>(1.0, 1.0));
  let ndc = vec2<f32>(position.x / safeViewport.x * 2.0 - 1.0,
                      1.0 - position.y / safeViewport.y * 2.0);
  var out: VertexOut;
  out.position = vec4<f32>(ndc, 0.0, 1.0);
  out.color = color;
  out.world = position;
  out.clipHeader = clipHeader;
  out.clip0 = clip0;
  out.clip1 = clip1;
  out.clip2 = clip2;
  out.clip3 = clip3;
  out.clip4 = clip4;
  out.clip5 = clip5;
  out.clip6 = clip6;
  out.clip7 = clip7;
  return out;
}

fn rounded_rect_sdf(p: vec2<f32>, halfSize: vec2<f32>, radii: vec4<f32>) -> f32 {
  let r = select(select(radii.x, radii.y, p.x > 0.0),
                 select(radii.w, radii.z, p.x > 0.0),
                 p.y > 0.0);
  let q = abs(p) - halfSize + vec2<f32>(r, r);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;
}

fn distance_coverage(d: f32) -> f32 {
  let aa = max(0.75 * length(vec2<f32>(dpdx(d), dpdy(d))), 0.0001);
  return 1.0 - smoothstep(-aa, aa, d);
}

fn clip_entry(in: VertexOut, index: u32) -> vec4<f32> {
  switch index {
    case 0u: { return in.clip0; }
    case 1u: { return in.clip1; }
    case 2u: { return in.clip2; }
    case 3u: { return in.clip3; }
    case 4u: { return in.clip4; }
    case 5u: { return in.clip5; }
    case 6u: { return in.clip6; }
    default: { return in.clip7; }
  }
}

fn rounded_clip_coverage(in: VertexOut) -> f32 {
  let count = clamp(i32(in.clipHeader.x + 0.5), 0, 4);
  var coverage = 1.0;
  for (var i = 0; i < count; i = i + 1) {
    let clipRect = clip_entry(in, u32(i * 2));
    let clipRadii = clip_entry(in, u32(i * 2 + 1));
    if (clipRect.z <= 0.0 || clipRect.w <= 0.0) {
      continue;
    }
    let halfSize = clipRect.zw * 0.5;
    let local = in.world - clipRect.xy - halfSize;
    coverage = coverage * distance_coverage(rounded_rect_sdf(local, halfSize, clipRadii));
  }
  return coverage;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
  let coverage = rounded_clip_coverage(in);
  let alpha = in.color.a * coverage;
  if (alpha <= 0.001) {
    discard;
  }
  return vec4<f32>(in.color.rgb, alpha);
}
)wgsl";

WGPUColor toWebGpuColor(Color color) noexcept {
  return WGPUColor{
      .r = static_cast<double>(color.r),
      .g = static_cast<double>(color.g),
      .b = static_cast<double>(color.b),
      .a = static_cast<double>(color.a),
  };
}

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

BlendMode webGpuFixedFunctionBlendMode(BlendMode mode) noexcept {
  switch (mode) {
    case BlendMode::Normal:
    case BlendMode::SrcOver:
      return BlendMode::Normal;
    case BlendMode::Multiply:
    case BlendMode::Screen:
    case BlendMode::Darken:
    case BlendMode::Lighten:
    case BlendMode::Clear:
    case BlendMode::Src:
    case BlendMode::Dst:
    case BlendMode::DstOver:
    case BlendMode::SrcIn:
    case BlendMode::DstIn:
    case BlendMode::SrcOut:
    case BlendMode::DstOut:
      return mode;
    default:
      return BlendMode::Normal;
  }
}

std::size_t webGpuBlendModeIndex(BlendMode mode) noexcept {
  std::size_t const index = static_cast<std::size_t>(webGpuFixedFunctionBlendMode(mode));
  return index < kWebGpuBlendModePipelineCount ? index : 0u;
}

void setSrcOverBlend(WGPUBlendState& blend) noexcept {
  blend.color.operation = WGPUBlendOperation_Add;
  blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
  blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
  blend.alpha.operation = WGPUBlendOperation_Add;
  blend.alpha.srcFactor = WGPUBlendFactor_One;
  blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
}

void applyBlendModeToAttachment(WGPUBlendState& blend, BlendMode mode) noexcept {
  setSrcOverBlend(blend);
  switch (webGpuFixedFunctionBlendMode(mode)) {
    case BlendMode::Normal:
      return;
    case BlendMode::Multiply:
      blend.color.srcFactor = WGPUBlendFactor_Dst;
      blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
      blend.alpha.srcFactor = WGPUBlendFactor_DstAlpha;
      blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
      return;
    case BlendMode::Screen:
      blend.color.srcFactor = WGPUBlendFactor_OneMinusDst;
      blend.color.dstFactor = WGPUBlendFactor_One;
      blend.alpha.srcFactor = WGPUBlendFactor_One;
      blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
      return;
    case BlendMode::Darken:
      blend.color.operation = WGPUBlendOperation_Min;
      blend.color.srcFactor = WGPUBlendFactor_One;
      blend.color.dstFactor = WGPUBlendFactor_One;
      return;
    case BlendMode::Lighten:
      blend.color.operation = WGPUBlendOperation_Max;
      blend.color.srcFactor = WGPUBlendFactor_One;
      blend.color.dstFactor = WGPUBlendFactor_One;
      return;
    case BlendMode::Clear:
      blend.color.srcFactor = WGPUBlendFactor_Zero;
      blend.color.dstFactor = WGPUBlendFactor_Zero;
      blend.alpha.srcFactor = WGPUBlendFactor_Zero;
      blend.alpha.dstFactor = WGPUBlendFactor_Zero;
      return;
    case BlendMode::Src:
      blend.color.srcFactor = WGPUBlendFactor_One;
      blend.color.dstFactor = WGPUBlendFactor_Zero;
      blend.alpha.srcFactor = WGPUBlendFactor_One;
      blend.alpha.dstFactor = WGPUBlendFactor_Zero;
      return;
    case BlendMode::Dst:
      blend.color.srcFactor = WGPUBlendFactor_Zero;
      blend.color.dstFactor = WGPUBlendFactor_One;
      blend.alpha.srcFactor = WGPUBlendFactor_Zero;
      blend.alpha.dstFactor = WGPUBlendFactor_One;
      return;
    case BlendMode::DstOver:
      blend.color.srcFactor = WGPUBlendFactor_OneMinusDstAlpha;
      blend.color.dstFactor = WGPUBlendFactor_One;
      blend.alpha.srcFactor = WGPUBlendFactor_OneMinusDstAlpha;
      blend.alpha.dstFactor = WGPUBlendFactor_One;
      return;
    case BlendMode::SrcIn:
      blend.color.srcFactor = WGPUBlendFactor_DstAlpha;
      blend.color.dstFactor = WGPUBlendFactor_Zero;
      blend.alpha.srcFactor = WGPUBlendFactor_DstAlpha;
      blend.alpha.dstFactor = WGPUBlendFactor_Zero;
      return;
    case BlendMode::DstIn:
      blend.color.srcFactor = WGPUBlendFactor_Zero;
      blend.color.dstFactor = WGPUBlendFactor_SrcAlpha;
      blend.alpha.srcFactor = WGPUBlendFactor_Zero;
      blend.alpha.dstFactor = WGPUBlendFactor_SrcAlpha;
      return;
    case BlendMode::SrcOut:
      blend.color.srcFactor = WGPUBlendFactor_OneMinusDstAlpha;
      blend.color.dstFactor = WGPUBlendFactor_Zero;
      blend.alpha.srcFactor = WGPUBlendFactor_OneMinusDstAlpha;
      blend.alpha.dstFactor = WGPUBlendFactor_Zero;
      return;
    case BlendMode::DstOut:
      blend.color.srcFactor = WGPUBlendFactor_Zero;
      blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
      blend.alpha.srcFactor = WGPUBlendFactor_Zero;
      blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
      return;
    default:
      return;
  }
}

std::uint32_t alignTo(std::uint32_t value, std::uint32_t alignment) noexcept {
  return (value + alignment - 1u) / alignment * alignment;
}

void frameCaptureMapCallback(WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*) {
  auto* state = static_cast<WebGpuMapWaitState*>(userdata1);
  state->status = status;
  state->done = true;
}

void putColor(float out[4], Color color, float opacity = 1.f) noexcept {
  out[0] = color.r;
  out[1] = color.g;
  out[2] = color.b;
  out[3] = color.a * opacity;
}

CornerRadius clampedRadii(CornerRadius radii, float width, float height) {
  clampRoundRectCornerRadii(width, height, radii);
  return radii;
}

bool representativeFillColor(FillStyle const& fill, Color* out) {
  if (fill.solidColor(out)) {
    return true;
  }
  LinearGradient linear{};
  if (fill.linearGradient(&linear) && linear.stopCount > 0) {
    *out = linear.stops[0].color;
    return true;
  }
  RadialGradient radial{};
  if (fill.radialGradient(&radial) && radial.stopCount > 0) {
    *out = radial.stops[0].color;
    return true;
  }
  ConicalGradient conical{};
  if (fill.conicalGradient(&conical) && conical.stopCount > 0) {
    *out = conical.stops[0].color;
    return true;
  }
  return false;
}

Rect boundsOfSubpaths(std::vector<std::vector<Point>> const& subpaths) {
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  for (std::vector<Point> const& subpath : subpaths) {
    for (Point const& point : subpath) {
      minX = std::min(minX, point.x);
      minY = std::min(minY, point.y);
      maxX = std::max(maxX, point.x);
      maxY = std::max(maxY, point.y);
    }
  }
  if (!std::isfinite(minX) || maxX <= minX || maxY <= minY) {
    return Rect::sharp(0.f, 0.f, 1.f, 1.f);
  }
  return Rect::sharp(minX, minY, maxX - minX, maxY - minY);
}

void encodeFill(FillStyle const& fill, WebGpuRectInstance& instance) {
  Color color{};
  if (fill.solidColor(&color)) {
    putColor(instance.fill0, color);
    instance.stops[0] = 0.f;
    instance.params[1] = 1.f;
    return;
  }

  auto writeStops = [&](auto const& gradient) {
    instance.params[1] = static_cast<float>(gradient.stopCount);
    std::array<float*, 4> colors{instance.fill0, instance.fill1, instance.fill2, instance.fill3};
    for (std::uint8_t i = 0; i < gradient.stopCount && i < colors.size(); ++i) {
      putColor(colors[i], gradient.stops[i].color);
      instance.stops[i] = gradient.stops[i].position;
    }
  };

  LinearGradient linear{};
  if (fill.linearGradient(&linear) && linear.stopCount > 0) {
    instance.params[0] = 1.f;
    instance.gradient[0] = linear.start.x;
    instance.gradient[1] = linear.start.y;
    instance.gradient[2] = linear.end.x;
    instance.gradient[3] = linear.end.y;
    writeStops(linear);
    return;
  }

  RadialGradient radial{};
  if (fill.radialGradient(&radial) && radial.stopCount > 0) {
    instance.params[0] = 2.f;
    instance.gradient[0] = radial.center.x;
    instance.gradient[1] = radial.center.y;
    instance.gradient[2] = radial.radius;
    writeStops(radial);
    return;
  }

  ConicalGradient conical{};
  if (fill.conicalGradient(&conical) && conical.stopCount > 0) {
    instance.params[0] = 3.f;
    instance.gradient[0] = conical.center.x;
    instance.gradient[1] = conical.center.y;
    instance.gradient[2] = conical.startAngleRadians;
    writeStops(conical);
    return;
  }

  putColor(instance.fill0, Colors::transparent);
  instance.params[1] = 1.f;
}

WGPUSurface createSurface(WGPUInstance instance, WebGpuNativeSurface native) {
  WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
  descriptor.label = stringView("LambdaUI WebGPU Surface");

  WGPUSurfaceSourceMetalLayer metalLayer = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
  WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;

  switch (native.kind) {
    case WebGpuNativeSurface::Kind::MetalLayer:
      if (!native.surface) {
        throw std::runtime_error("Lambda WebGPU: missing CAMetalLayer surface");
      }
      metalLayer.layer = native.surface;
      descriptor.nextInChain = &metalLayer.chain;
      break;
    case WebGpuNativeSurface::Kind::WaylandSurface:
      if (!native.display || !native.surface) {
        throw std::runtime_error("Lambda WebGPU: missing Wayland display or surface");
      }
      wayland.display = native.display;
      wayland.surface = native.surface;
      descriptor.nextInChain = &wayland.chain;
      break;
    case WebGpuNativeSurface::Kind::None:
      throw std::runtime_error("Lambda WebGPU: window canvas requires a native surface");
  }

  WGPUSurface surface = wgpuInstanceCreateSurface(instance, &descriptor);
  if (!surface) {
    throw std::runtime_error("Lambda WebGPU: failed to create native surface");
  }
  return surface;
}

class WebGpuCanvas final : public Canvas {
  struct WebGpuDrawOp {
    enum class Kind {
      Rect,
      Image,
      Path,
      BackdropBlur,
    };

    Kind kind = Kind::Rect;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    BlendMode blendMode = BlendMode::Normal;
    WebGpuImage const* image = nullptr;
    std::shared_ptr<Image const> imageRef;
    float blurRadiusPx = 0.f;
  };

  struct WebGpuFrameRecorder final : RecordedOps {
    std::vector<WebGpuDrawOp> drawOps;
    std::vector<WebGpuRectInstance> rects;
    std::vector<WebGpuQuadInstance> quads;
    std::vector<WebGpuPathVertex> pathVertices;
    bool containsClipState = false;

    Backend backend() const noexcept override { return Backend::WebGPU; }
  };

  class WebGpuCanvasPreparedRenderOps final : public scenegraph::PreparedRenderOps {
  public:
    explicit WebGpuCanvasPreparedRenderOps(WebGpuFrameRecorder recorded)
        : recorded_(std::move(recorded)) {}

    bool replay(Canvas& canvas) const override {
      return canvas.replayRecordedLocalOps(recorded_);
    }

  private:
    WebGpuFrameRecorder recorded_;
  };

public:
  WebGpuCanvas(WebGpuNativeSurface nativeSurface,
               unsigned int handle,
               TextSystem& textSystem,
               Size initialSize,
               bool transparentSurface)
      : nativeSurface_(nativeSurface),
        handle_(handle),
        textSystem_(textSystem),
        size_(initialSize),
        transparentSurface_(transparentSurface),
        context_(),
        surface_(createSurface(context_.instance(), nativeSurface_)) {
    context_.initializeDevice(surface_);
    configureSurface();
    clip_ = viewportBounds();
  }

  WebGpuCanvas(TextSystem& textSystem,
               Size logicalSize,
               std::uint32_t pixelWidth,
               std::uint32_t pixelHeight)
      : textSystem_(textSystem),
        size_(logicalSize),
        context_(),
        surfaceFormat_(WGPUTextureFormat_RGBA8Unorm),
        surfaceCopySrcSupported_(true),
        offscreenPixelWidth_(std::max(1u, pixelWidth)),
        offscreenPixelHeight_(std::max(1u, pixelHeight)) {
    context_.initializeDevice(nullptr);
    dpiScale_ = std::max(logicalSize.width > 0.f ? static_cast<float>(offscreenPixelWidth_) / logicalSize.width : 1.f,
                         logicalSize.height > 0.f ? static_cast<float>(offscreenPixelHeight_) / logicalSize.height : 1.f);
    clip_ = viewportBounds();
  }

  WebGpuCanvas(TextSystem& textSystem,
               Size logicalSize,
               std::uint32_t pixelWidth,
               std::uint32_t pixelHeight,
               WGPUDevice device,
               WGPUQueue queue,
               WGPUTextureView textureView,
               WGPUTextureFormat format)
      : textSystem_(textSystem),
        size_(logicalSize),
        context_(device, queue),
        surfaceFormat_(format == WGPUTextureFormat_Undefined ? WGPUTextureFormat_RGBA8Unorm : format),
        surfaceCopySrcSupported_(false),
        externalRenderTargetView_(textureView),
        offscreenPixelWidth_(std::max(1u, pixelWidth)),
        offscreenPixelHeight_(std::max(1u, pixelHeight)) {
    if (!textureView) {
      throw std::runtime_error("Lambda WebGPU: external render targets require a WGPUTextureView");
    }
    wgpuTextureViewAddRef(externalRenderTargetView_);
    dpiScale_ = std::max(logicalSize.width > 0.f ? static_cast<float>(offscreenPixelWidth_) / logicalSize.width : 1.f,
                         logicalSize.height > 0.f ? static_cast<float>(offscreenPixelHeight_) / logicalSize.height : 1.f);
    clip_ = viewportBounds();
  }

  ~WebGpuCanvas() override {
    releaseFrameObjects();
    clearPendingFrameCapture();
    releaseExternalRenderTarget();
    releaseBackdropTargets();
    releaseOffscreenTarget();
    releaseDrawResources();
    if (surface_) {
      wgpuSurfaceUnconfigure(surface_);
      wgpuSurfaceRelease(surface_);
    }
  }

  Backend backend() const noexcept override { return Backend::WebGPU; }
  unsigned int windowHandle() const override { return handle_; }
  WGPUDevice deviceHandle() const noexcept { return context_.device(); }
  WGPUQueue queueHandle() const noexcept { return context_.queue(); }
  WGPUTextureFormat renderTargetFormat() const noexcept { return surfaceFormat_; }

  void resize(int width, int height) override {
    width = std::max(1, width);
    height = std::max(1, height);
    if (size_.width == static_cast<float>(width) && size_.height == static_cast<float>(height)) {
      return;
    }
    size_ = {static_cast<float>(width), static_cast<float>(height)};
    if (surface_) {
      configureSurface();
      releaseBackdropTargets();
    } else if (externalRenderTargetView_) {
      offscreenPixelWidth_ = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.width * dpiScale_)));
      offscreenPixelHeight_ = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.height * dpiScale_)));
      releaseBackdropTargets();
    } else {
      offscreenPixelWidth_ = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.width * dpiScale_)));
      offscreenPixelHeight_ = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.height * dpiScale_)));
      releaseOffscreenTarget();
      releaseBackdropTargets();
    }
    if (!frameActive_) {
      clip_ = viewportBounds();
    }
  }

  void updateDpiScale(float scaleX, float scaleY) override {
    dpiScale_ = std::max(scaleX, scaleY);
  }

  float dpiScale() const noexcept override { return dpiScale_; }

  void beginFrame() override {
    releaseFrameObjects();
    rects_.clear();
    quads_.clear();
    pathVertices_.clear();
    drawOps_.clear();
    stateStack_.clear();
    transform_ = Mat3::identity();
    opacity_ = 1.f;
    blendMode_ = BlendMode::Normal;
    clip_ = viewportBounds();
    clipMaskCount_ = 0;
    frameActive_ = true;
  }

  void present() override {
    if (!frameActive_) {
      return;
    }
    encodeFramePass();
    bool const captureThisFrame = enqueueFrameCaptureCopy();
    bool submitted = false;
    if (commandEncoder_) {
      WGPUCommandBufferDescriptor finishDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
      finishDescriptor.label = stringView("LambdaUI WebGPU Command Buffer");
      WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(commandEncoder_, &finishDescriptor);
      if (commandBuffer) {
        wgpuQueueSubmit(context_.queue(), 1, &commandBuffer);
        wgpuCommandBufferRelease(commandBuffer);
        submitted = true;
      }
    }
    if (captureThisFrame && submitted) {
      finishFrameCaptureReadback();
    } else if (captureThisFrame) {
      clearPendingFrameCapture();
    }
    if (surface_) {
      wgpuSurfacePresent(surface_);
    }
    releaseFrameObjects();
    frameActive_ = false;
  }

  void save() override { stateStack_.push_back(state()); }
  void restore() override {
    if (stateStack_.empty()) {
      return;
    }
    State const restored = stateStack_.back();
    stateStack_.pop_back();
    transform_ = restored.transform;
    opacity_ = restored.opacity;
    blendMode_ = restored.blendMode;
    clip_ = restored.clip;
    clipMasks_ = restored.clipMasks;
    clipMaskCount_ = restored.clipMaskCount;
  }

  void setTransform(Mat3 const& m) override { transform_ = m; }
  void transform(Mat3 const& m) override { transform_ = transform_ * m; }
  void translate(Point offset) override { transform(Mat3::translate(offset)); }
  void translate(float x, float y) override { transform(Mat3::translate(x, y)); }
  void scale(float sx, float sy) override { transform(Mat3::scale(sx, sy)); }
  void scale(float s) override { transform(Mat3::scale(s)); }
  void rotate(float radians) override { transform(Mat3::rotate(radians)); }
  void rotate(float radians, Point pivot) override { transform(Mat3::rotate(radians, pivot)); }
  Mat3 currentTransform() const override { return transform_; }

  void clipRect(Rect rect, CornerRadius const& cornerRadius, bool) override {
    if (captureTarget_) {
      captureTarget_->containsClipState = true;
    }
    Rect const bounds = transformedBounds(rect);
    Rect const effective = intersectRects(clip_, bounds);
    clip_ = effective;
    if (effective.width <= 0.f || effective.height <= 0.f) {
      clipMaskCount_ = 0;
      return;
    }

    float const sx = std::hypot(transform_.m[0], transform_.m[1]);
    float const sy = std::hypot(transform_.m[3], transform_.m[4]);
    float const radiusScale = std::max(0.f, std::min(sx, sy));
    CornerRadius radii{
        cornerRadius.topLeft * radiusScale,
        cornerRadius.topRight * radiusScale,
        cornerRadius.bottomRight * radiusScale,
        cornerRadius.bottomLeft * radiusScale,
    };
    constexpr float eps = 1e-3f;
    bool const clipped = std::abs(effective.x - bounds.x) > eps || std::abs(effective.y - bounds.y) > eps ||
                         std::abs(effective.width - bounds.width) > eps ||
                         std::abs(effective.height - bounds.height) > eps;
    if (clipped) {
      radii = cornerRadiiAfterAxisAlignedClip(bounds, effective, radii);
    }
    radii = clampedRadii(radii, effective.width, effective.height);

    WebGpuRoundedClipState const mask{
        .rect = effective,
        .radii = radii,
    };
    if (clipMaskCount_ < kRoundedClipMaskCapacity) {
      clipMasks_[clipMaskCount_++] = mask;
    } else {
      clipMasks_[kRoundedClipMaskCapacity - 1] = WebGpuRoundedClipState{
          .rect = effective,
          .radii = CornerRadius{},
      };
    }
  }
  Rect clipBounds() const override { return clip_; }
  bool quickReject(Rect rect) const override {
    return clip_.width <= 0.f || clip_.height <= 0.f || !clip_.intersects(transformedBounds(rect));
  }

  void setOpacity(float opacity) override { opacity_ = opacity; }
  float opacity() const override { return opacity_; }
  void setBlendMode(BlendMode mode) override { blendMode_ = mode; }
  BlendMode blendMode() const override { return blendMode_; }

  void drawRect(Rect const& rect,
                CornerRadius const& cornerRadius,
                FillStyle const& fill,
                StrokeStyle const& stroke,
                ShadowStyle const& shadow) override {
    if (!frameActive_ || rect.width <= 0.f || rect.height <= 0.f) {
      return;
    }
    Color strokeColor{};
    bool const hasFill = !fill.isNone();
    bool const hasStroke = stroke.solidColor(&strokeColor) && stroke.width > 0.f;
    if (!hasFill && !hasStroke && shadow.isNone()) {
      return;
    }

    if (!shadow.isNone()) {
      if (shadow.radius <= 0.f) {
        pushRectInstance(Rect::sharp(rect.x + shadow.offset.x, rect.y + shadow.offset.y, rect.width, rect.height),
                         cornerRadius,
                         FillStyle::solid(shadow.color),
                         StrokeStyle::none(),
                         opacity_);
      } else {
        int const steps = std::clamp(static_cast<int>(std::ceil(shadow.radius / 3.f)), 3, 8);
        for (int i = steps; i >= 1; --i) {
          float const t = static_cast<float>(i) / static_cast<float>(steps);
          float const spread = shadow.radius * t;
          Color color = shadow.color;
          color.a *= (1.f - t * 0.72f) / static_cast<float>(steps);
          Rect const layer = Rect::sharp(rect.x + shadow.offset.x - spread,
                                         rect.y + shadow.offset.y - spread,
                                         rect.width + spread * 2.f,
                                         rect.height + spread * 2.f);
          CornerRadius radii{cornerRadius.topLeft + spread,
                             cornerRadius.topRight + spread,
                             cornerRadius.bottomRight + spread,
                             cornerRadius.bottomLeft + spread};
          pushRectInstance(layer, radii, FillStyle::solid(color), StrokeStyle::none(), opacity_);
        }
      }
    }

    pushRectInstance(rect, cornerRadius, fill, stroke, opacity_);
  }

  void drawLine(Point from, Point to, StrokeStyle const& stroke) override {
    if (!frameActive_ || stroke.isNone() || clip_.width <= 0.f || clip_.height <= 0.f) {
      return;
    }
    Color strokeColor{};
    if (!stroke.solidColor(&strokeColor) || stroke.width <= 0.f) {
      return;
    }
    Point const a = transform_.apply(from);
    Point const b = transform_.apply(to);
    float const dx = b.x - a.x;
    float const dy = b.y - a.y;
    float const length = std::hypot(dx, dy);
    if (!std::isfinite(length) || length <= 0.f) {
      return;
    }
    float const halfWidth = stroke.width * 0.5f;
    Rect const lineBounds = Rect::sharp(std::min(a.x, b.x) - halfWidth,
                                        std::min(a.y, b.y) - halfWidth,
                                        std::abs(dx) + stroke.width,
                                        std::abs(dy) + stroke.width);
    if (!clip_.intersects(lineBounds)) {
      return;
    }
    Point const normal{-dy / length * halfWidth, dx / length * halfWidth};
    WebGpuRectInstance instance{};
    instance.rect[2] = length;
    instance.rect[3] = stroke.width;
    instance.axisX[0] = a.x + normal.x;
    instance.axisX[1] = a.y + normal.y;
    instance.axisX[2] = dx;
    instance.axisX[3] = dy;
    instance.axisY[0] = -normal.x * 2.f;
    instance.axisY[1] = -normal.y * 2.f;
    float const radius = stroke.cap == StrokeCap::Round ? halfWidth : 0.f;
    instance.radii[0] = radius;
    instance.radii[1] = radius;
    instance.radii[2] = radius;
    instance.radii[3] = radius;
    putColor(instance.fill0, strokeColor);
    instance.params[1] = 1.f;
    instance.params[3] = opacity_;
    applyCurrentClip(instance);
    std::uint32_t const first = static_cast<std::uint32_t>(rects_.size());
    rects_.push_back(instance);
    drawOps_.push_back(WebGpuDrawOp{
        .kind = WebGpuDrawOp::Kind::Rect,
        .first = first,
        .count = 1,
        .blendMode = blendMode_,
    });
  }

  void drawPath(Path const& path,
                FillStyle const& fill,
                StrokeStyle const& stroke,
                ShadowStyle const& shadow) override {
    if (!frameActive_ || path.isEmpty()) {
      return;
    }
    if (path.commandCount() == 1) {
      Path::CommandView const command = path.command(0);
      if (command.type == Path::CommandType::Rect && command.dataCount >= 8) {
        Rect const rect{command.data[0], command.data[1], command.data[2], command.data[3]};
        CornerRadius const radii{command.data[4], command.data[5], command.data[6], command.data[7]};
        drawRect(rect, radii, fill, stroke, shadow);
        return;
      }
      if (command.type == Path::CommandType::Circle && command.dataCount >= 3) {
        float const radius = command.data[2];
        Rect const rect{command.data[0] - radius, command.data[1] - radius, radius * 2.f, radius * 2.f};
        drawRect(rect, CornerRadius::pill(rect), fill, stroke, shadow);
        return;
      }
      if (command.type == Path::CommandType::Ellipse && command.dataCount >= 4) {
        Rect const rect{command.data[0] - command.data[2],
                        command.data[1] - command.data[3],
                        command.data[2] * 2.f,
                        command.data[3] * 2.f};
        drawRect(rect, CornerRadius::pill(rect), fill, stroke, shadow);
        return;
      }
    }

    if (!shadow.isNone()) {
      save();
      translate(shadow.offset);
      appendPath(path, FillStyle::solid(shadow.color), StrokeStyle::none());
      restore();
    }
    appendPath(path, fill, stroke);
  }

  void drawCircle(Point center, float radius, FillStyle const& fill, StrokeStyle const& stroke) override {
    Rect const rect{center.x - radius, center.y - radius, radius * 2.f, radius * 2.f};
    drawRect(rect, CornerRadius::pill(rect), fill, stroke, ShadowStyle::none());
  }

  void drawTextLayout(TextLayout const& layout, Point origin) override {
    if (!frameActive_) {
      return;
    }
    for (TextLayout::PlacedRun const& placed : layout.runs) {
      if (placed.run.backgroundColor) {
        drawRect(Rect::sharp(origin.x + placed.origin.x,
                             origin.y + placed.origin.y - placed.run.ascent,
                             placed.run.width,
                             placed.run.ascent + placed.run.descent),
                 CornerRadius{},
                 FillStyle::solid(*placed.run.backgroundColor),
                 StrokeStyle::none(),
                 ShadowStyle::none());
      }

      std::size_t const glyphCount = std::min(placed.run.glyphIds.size(), placed.run.positions.size());
      for (std::size_t i = 0; i < glyphCount; ++i) {
        GlyphImage const* glyph = glyphImage(placed.run.fontId, placed.run.glyphIds[i], placed.run.fontSize);
        if (!glyph || !glyph->image || glyph->width == 0 || glyph->height == 0) {
          continue;
        }
        Point const pos = origin + placed.origin + placed.run.positions[i];
        Rect const glyphRect = Rect::sharp(pos.x + glyph->bearing.x / dpiScale_,
                                           pos.y - glyph->bearing.y / dpiScale_,
                                           static_cast<float>(glyph->width) / dpiScale_,
                                           static_cast<float>(glyph->height) / dpiScale_);
        pushImageInstance(*glyph->image,
                          Rect::sharp(0.f, 0.f, static_cast<float>(glyph->width), static_cast<float>(glyph->height)),
                          glyphRect,
                          CornerRadius{},
                          placed.run.color,
                          1.f);
      }
    }
  }
  void drawImage(Image const& image,
                 Rect const& src,
                 Rect const& dst,
                 CornerRadius const& corners,
                 float opacity) override {
    if (!frameActive_ || src.width <= 0.f || src.height <= 0.f || dst.width <= 0.f || dst.height <= 0.f) {
      return;
    }
    auto const* webgpuImage = dynamic_cast<WebGpuImage const*>(&image);
    if (!webgpuImage) {
      return;
    }
    pushImageInstance(*webgpuImage, src, dst, corners, Colors::white, opacity);
  }

  void drawImageTiled(Image const& image, Rect const& dst, CornerRadius const& corners, float opacity) override {
    Size const imageSize = image.size();
    if (imageSize.width <= 0.f || imageSize.height <= 0.f || dst.width <= 0.f || dst.height <= 0.f) {
      return;
    }
    int const columns = static_cast<int>(std::ceil(dst.width / imageSize.width));
    int const rows = static_cast<int>(std::ceil(dst.height / imageSize.height));
    for (int row = 0; row < rows; ++row) {
      for (int column = 0; column < columns; ++column) {
        Rect const tile = Rect::sharp(
            dst.x + static_cast<float>(column) * imageSize.width,
            dst.y + static_cast<float>(row) * imageSize.height,
            std::min(imageSize.width,
                     dst.x + dst.width - (dst.x + static_cast<float>(column) * imageSize.width)),
            std::min(imageSize.height,
                     dst.y + dst.height - (dst.y + static_cast<float>(row) * imageSize.height)));
        Rect const tileSource = Rect::sharp(0.f, 0.f, tile.width, tile.height);
        drawImage(image, tileSource, tile, corners, opacity);
      }
    }
  }
  void drawBackdropBlur(Rect const& rect, float radius, Color tint, CornerRadius const& corners) override {
    if (!frameActive_ || rect.width <= 0.f || rect.height <= 0.f) {
      return;
    }
    if (radius > 0.f && !externalRenderTargetView_) {
      pushBackdropBlurInstance(rect, corners, radius);
    }
    if (tint.a > 0.001f) {
      pushRectInstance(rect, corners, FillStyle::solid(tint), StrokeStyle::none(), opacity_);
    }
  }

  void* gpuDevice() const override { return context_.device(); }

  TextSystem& textSystem() noexcept { return textSystem_; }

  bool requestNextFrameCapture() override {
    if (!surfaceCopySrcSupported_ || !frameCaptureFormatSupported()) {
      return false;
    }
    frameCaptureRequested_ = true;
    return true;
  }

  bool takeCapturedFrame(std::vector<std::uint8_t>& out, std::uint32_t& width, std::uint32_t& height) override {
    if (!capturedFrameAvailable_) {
      return false;
    }
    out = std::move(capturedFrameBytes_);
    width = capturedFrameWidth_;
    height = capturedFrameHeight_;
    capturedFrameBytes_.clear();
    capturedFrameWidth_ = 0;
    capturedFrameHeight_ = 0;
    capturedFrameAvailable_ = false;
    return true;
  }
  std::unique_ptr<RecordedOps> beginRecordedOpsCapture() override {
    if (!frameActive_ || captureTarget_) {
      return nullptr;
    }
    auto recorded = std::make_unique<WebGpuFrameRecorder>();
    beginRecordedOpsCapture(recorded.get());
    return recorded;
  }

  void endRecordedOpsCapture() override {
    if (!captureTarget_) {
      return;
    }

    captureTarget_->rects.assign(rects_.begin() + static_cast<std::ptrdiff_t>(captureRectStart_), rects_.end());
    captureTarget_->quads.assign(quads_.begin() + static_cast<std::ptrdiff_t>(captureQuadStart_), quads_.end());
    captureTarget_->pathVertices.assign(pathVertices_.begin() + static_cast<std::ptrdiff_t>(capturePathVertexStart_),
                                        pathVertices_.end());
    captureTarget_->drawOps.assign(drawOps_.begin() + static_cast<std::ptrdiff_t>(captureDrawOpStart_),
                                   drawOps_.end());
    for (WebGpuDrawOp& op : captureTarget_->drawOps) {
      switch (op.kind) {
        case WebGpuDrawOp::Kind::Rect:
          op.first -= captureRectStart_;
          break;
        case WebGpuDrawOp::Kind::Image:
        case WebGpuDrawOp::Kind::BackdropBlur:
          op.first -= captureQuadStart_;
          break;
        case WebGpuDrawOp::Kind::Path:
          op.first -= capturePathVertexStart_;
          break;
      }
    }

    rects_.resize(captureRectStart_);
    quads_.resize(captureQuadStart_);
    pathVertices_.resize(capturePathVertexStart_);
    drawOps_.resize(captureDrawOpStart_);
    captureTarget_ = nullptr;
    restoreAfterRecordedOpsCapture();
  }

  std::unique_ptr<scenegraph::PreparedRenderOps> finalizeRecordedOps(std::unique_ptr<RecordedOps> recorded) override {
    WebGpuFrameRecorder* webgpuRecorded = asWebGpuRecordedOps(recorded.get());
    if (!webgpuRecorded || recordedOpsContainClipState(*webgpuRecorded)) {
      return nullptr;
    }
    return std::make_unique<WebGpuCanvasPreparedRenderOps>(std::move(*webgpuRecorded));
  }

  bool replayRecordedOps(RecordedOps const& recorded, RecordedOpsReplaySlice const* slice) override {
    if (slice && slice->backend != Backend::WebGPU) {
      return false;
    }
    WebGpuFrameRecorder const* webgpuRecorded = asWebGpuRecordedOps(recorded);
    return webgpuRecorded && appendRecordedOps(*webgpuRecorded, false);
  }

  bool replayRecordedLocalOps(RecordedOps const& recorded, RecordedOpsReplaySlice const* slice) override {
    if (slice && slice->backend != Backend::WebGPU) {
      return false;
    }
    WebGpuFrameRecorder const* webgpuRecorded = asWebGpuRecordedOps(recorded);
    return webgpuRecorded && appendRecordedOps(*webgpuRecorded, true);
  }

  void clear(Color color) override {
    clearColor_ = color;
  }

private:
  struct State {
    Mat3 transform = Mat3::identity();
    float opacity = 1.f;
    BlendMode blendMode = BlendMode::Normal;
    Rect clip{};
    std::array<WebGpuRoundedClipState, kRoundedClipMaskCapacity> clipMasks{};
    std::uint32_t clipMaskCount = 0;
  };

  struct GlyphKey {
    std::uint32_t fontId = 0;
    std::uint32_t glyphId = 0;
    std::uint16_t pixelSize = 0;

    bool operator==(GlyphKey const&) const = default;
  };

  struct GlyphKeyHash {
    std::size_t operator()(GlyphKey const& key) const noexcept {
      std::size_t h = static_cast<std::size_t>(key.fontId) * 0x9e3779b1u;
      h ^= static_cast<std::size_t>(key.glyphId) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
      h ^= static_cast<std::size_t>(key.pixelSize) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
      return h;
    }
  };

  struct GlyphImage {
    std::shared_ptr<WebGpuImage> image;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Point bearing{};
  };

  State state() const noexcept {
    return State{
        .transform = transform_,
        .opacity = opacity_,
        .blendMode = blendMode_,
        .clip = clip_,
        .clipMasks = clipMasks_,
        .clipMaskCount = clipMaskCount_,
    };
  }

  Rect viewportBounds() const noexcept {
    return Rect::sharp(0.f, 0.f, std::max(1.f, size_.width), std::max(1.f, size_.height));
  }

  std::uint32_t targetPixelWidth() const noexcept {
    if (surface_) {
      return std::max(1u, static_cast<std::uint32_t>(std::lround(size_.width)));
    }
    return std::max(1u, offscreenPixelWidth_);
  }

  std::uint32_t targetPixelHeight() const noexcept {
    if (surface_) {
      return std::max(1u, static_cast<std::uint32_t>(std::lround(size_.height)));
    }
    return std::max(1u, offscreenPixelHeight_);
  }

  float targetDpiScale() const noexcept {
    float const sx = static_cast<float>(targetPixelWidth()) / std::max(1.f, size_.width);
    float const sy = static_cast<float>(targetPixelHeight()) / std::max(1.f, size_.height);
    return std::max(sx, sy);
  }

  Rect transformedBounds(Rect rect) const {
    return boundsOfTransformedRect(rect, transform_);
  }

  template <typename Instance>
  void applyCurrentClip(Instance& instance) const {
    std::uint32_t const count = std::min<std::uint32_t>(clipMaskCount_, kRoundedClipMaskCapacity);
    instance.clipHeader[0] = static_cast<float>(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      WebGpuRoundedClipState const& clip = clipMasks_[i];
      instance.clipEntries[i * 2u][0] = clip.rect.x;
      instance.clipEntries[i * 2u][1] = clip.rect.y;
      instance.clipEntries[i * 2u][2] = clip.rect.width;
      instance.clipEntries[i * 2u][3] = clip.rect.height;
      instance.clipEntries[i * 2u + 1u][0] = clip.radii.topLeft;
      instance.clipEntries[i * 2u + 1u][1] = clip.radii.topRight;
      instance.clipEntries[i * 2u + 1u][2] = clip.radii.bottomRight;
      instance.clipEntries[i * 2u + 1u][3] = clip.radii.bottomLeft;
    }
  }

  WebGpuPathVertex makePathVertex(PathVertex const& source) const {
    WebGpuPathVertex vertex{};
    vertex.x = source.x;
    vertex.y = source.y;
    std::memcpy(vertex.color, source.color, sizeof(vertex.color));
    std::memcpy(vertex.viewport, source.viewport, sizeof(vertex.viewport));
    applyCurrentClip(vertex);
    return vertex;
  }

  static WebGpuFrameRecorder* asWebGpuRecordedOps(RecordedOps* recorded) noexcept {
    return recorded && recorded->backend() == Backend::WebGPU
               ? static_cast<WebGpuFrameRecorder*>(recorded)
               : nullptr;
  }

  static WebGpuFrameRecorder const* asWebGpuRecordedOps(RecordedOps const& recorded) noexcept {
    return recorded.backend() == Backend::WebGPU
               ? static_cast<WebGpuFrameRecorder const*>(&recorded)
               : nullptr;
  }

  static bool instanceHasClip(float const (&clipHeader)[4]) noexcept {
    return clipHeader[0] > 0.5f;
  }

  static bool recordedOpsContainClipState(WebGpuFrameRecorder const& recorded) noexcept {
    if (recorded.containsClipState) {
      return true;
    }
    bool const rectClip = std::any_of(recorded.rects.begin(), recorded.rects.end(), [](WebGpuRectInstance const& inst) {
      return instanceHasClip(inst.clipHeader);
    });
    if (rectClip) {
      return true;
    }
    bool const quadClip = std::any_of(recorded.quads.begin(), recorded.quads.end(), [](WebGpuQuadInstance const& inst) {
      return instanceHasClip(inst.clipHeader);
    });
    if (quadClip) {
      return true;
    }
    bool const backdropBlur = std::any_of(recorded.drawOps.begin(), recorded.drawOps.end(), [](WebGpuDrawOp const& op) {
      return op.kind == WebGpuDrawOp::Kind::BackdropBlur;
    });
    if (backdropBlur) {
      return true;
    }
    return std::any_of(recorded.pathVertices.begin(), recorded.pathVertices.end(), [](WebGpuPathVertex const& vertex) {
      return instanceHasClip(vertex.clipHeader);
    });
  }

  void beginRecordedOpsCapture(WebGpuFrameRecorder* target) {
    if (!target || captureTarget_) {
      return;
    }
    target->drawOps.clear();
    target->rects.clear();
    target->quads.clear();
    target->pathVertices.clear();
    target->containsClipState = false;

    captureRectStart_ = static_cast<std::uint32_t>(rects_.size());
    captureQuadStart_ = static_cast<std::uint32_t>(quads_.size());
    capturePathVertexStart_ = static_cast<std::uint32_t>(pathVertices_.size());
    captureDrawOpStart_ = static_cast<std::uint32_t>(drawOps_.size());
    captureSavedState_ = state();
    captureSavedStack_ = stateStack_;
    captureTarget_ = target;

    stateStack_.clear();
    transform_ = Mat3::identity();
    opacity_ = 1.f;
    blendMode_ = BlendMode::Normal;
    clip_ = viewportBounds();
    clipMaskCount_ = 0;
  }

  void restoreAfterRecordedOpsCapture() {
    transform_ = captureSavedState_.transform;
    opacity_ = captureSavedState_.opacity;
    blendMode_ = captureSavedState_.blendMode;
    clip_ = captureSavedState_.clip;
    clipMasks_ = captureSavedState_.clipMasks;
    clipMaskCount_ = captureSavedState_.clipMaskCount;
    stateStack_ = std::move(captureSavedStack_);
    captureSavedState_ = {};
    captureSavedStack_.clear();
    captureRectStart_ = 0;
    captureQuadStart_ = 0;
    capturePathVertexStart_ = 0;
    captureDrawOpStart_ = 0;
  }

  static void translateRectInstance(WebGpuRectInstance& inst, float dx, float dy, float opacityScale) noexcept {
    inst.axisX[0] += dx;
    inst.axisX[1] += dy;
    inst.params[3] *= opacityScale;
  }

  static void translateQuadInstance(WebGpuQuadInstance& inst, float dx, float dy, float opacityScale) noexcept {
    inst.axisX[0] += dx;
    inst.axisX[1] += dy;
    inst.color[3] *= opacityScale;
  }

  static void translateClipEntries(float (&clipHeader)[4],
                                   float (&clipEntries)[kRoundedClipEntryCount][4],
                                   float dx,
                                   float dy) noexcept {
    std::uint32_t const count = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(clipHeader[0] + 0.5f), 0, static_cast<int>(kRoundedClipMaskCapacity)));
    for (std::uint32_t i = 0; i < count; ++i) {
      clipEntries[i * 2u][0] += dx;
      clipEntries[i * 2u][1] += dy;
    }
  }

  static void translatePathVertex(WebGpuPathVertex& vertex, float dx, float dy, float opacityScale) noexcept {
    vertex.x += dx;
    vertex.y += dy;
    vertex.color[3] *= opacityScale;
    translateClipEntries(vertex.clipHeader, vertex.clipEntries, dx, dy);
  }

  bool appendRecordedOps(WebGpuFrameRecorder const& recorded, bool localReplay) {
    if (!frameActive_) {
      return false;
    }
    if (localReplay && !transform_.isTranslationOnly()) {
      return false;
    }

    float const dx = localReplay ? transform_.m[6] : 0.f;
    float const dy = localReplay ? transform_.m[7] : 0.f;
    float const opacityScale = localReplay ? opacity_ : 1.f;
    std::uint32_t const rectBase = static_cast<std::uint32_t>(rects_.size());
    std::uint32_t const quadBase = static_cast<std::uint32_t>(quads_.size());
    std::uint32_t const pathBase = static_cast<std::uint32_t>(pathVertices_.size());

    rects_.reserve(rects_.size() + recorded.rects.size());
    for (WebGpuRectInstance inst : recorded.rects) {
      if (localReplay) {
        translateRectInstance(inst, dx, dy, opacityScale);
        applyCurrentClip(inst);
      }
      rects_.push_back(inst);
    }

    quads_.reserve(quads_.size() + recorded.quads.size());
    for (WebGpuQuadInstance inst : recorded.quads) {
      if (localReplay) {
        translateQuadInstance(inst, dx, dy, opacityScale);
        applyCurrentClip(inst);
      }
      quads_.push_back(inst);
    }

    pathVertices_.reserve(pathVertices_.size() + recorded.pathVertices.size());
    for (WebGpuPathVertex vertex : recorded.pathVertices) {
      if (localReplay) {
        translatePathVertex(vertex, dx, dy, opacityScale);
        applyCurrentClip(vertex);
      }
      pathVertices_.push_back(vertex);
    }

    drawOps_.reserve(drawOps_.size() + recorded.drawOps.size());
    for (WebGpuDrawOp op : recorded.drawOps) {
      switch (op.kind) {
        case WebGpuDrawOp::Kind::Rect:
          op.first += rectBase;
          break;
        case WebGpuDrawOp::Kind::Image:
        case WebGpuDrawOp::Kind::BackdropBlur:
          op.first += quadBase;
          break;
        case WebGpuDrawOp::Kind::Path:
          op.first += pathBase;
          break;
      }
      drawOps_.push_back(std::move(op));
    }
    return true;
  }

  void configureSurface() {
    if (!surface_) {
      return;
    }
    WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(surface_, context_.adapter(), &capabilities) != WGPUStatus_Success) {
      throw std::runtime_error("Lambda WebGPU: failed to query surface capabilities");
    }

    surfaceFormat_ = capabilities.formatCount > 0 ? capabilities.formats[0] : WGPUTextureFormat_BGRA8Unorm;
    if (containsFormat(capabilities, WGPUTextureFormat_BGRA8Unorm)) {
      surfaceFormat_ = WGPUTextureFormat_BGRA8Unorm;
    } else if (containsFormat(capabilities, WGPUTextureFormat_RGBA8Unorm)) {
      surfaceFormat_ = WGPUTextureFormat_RGBA8Unorm;
    }

    WGPUPresentMode presentMode = containsPresentMode(capabilities, WGPUPresentMode_Fifo)
                                      ? WGPUPresentMode_Fifo
                                      : (capabilities.presentModeCount > 0 ? capabilities.presentModes[0]
                                                                          : WGPUPresentMode_Undefined);

    WGPUCompositeAlphaMode alphaMode = transparentSurface_ ? WGPUCompositeAlphaMode_Premultiplied
                                                           : WGPUCompositeAlphaMode_Opaque;
    if (capabilities.alphaModeCount > 0) {
      bool alphaSupported = false;
      for (std::size_t i = 0; i < capabilities.alphaModeCount; ++i) {
        if (capabilities.alphaModes[i] == alphaMode) {
          alphaSupported = true;
          break;
        }
      }
      if (!alphaSupported) {
        alphaMode = capabilities.alphaModes[0];
      }
    }

    surfaceCopySrcSupported_ = (capabilities.usages & WGPUTextureUsage_CopySrc) != 0;

    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = context_.device();
    config.format = surfaceFormat_;
    config.usage = WGPUTextureUsage_RenderAttachment;
    if (surfaceCopySrcSupported_) {
      config.usage |= WGPUTextureUsage_CopySrc;
    }
    config.width = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.width)));
    config.height = std::max(1u, static_cast<std::uint32_t>(std::lround(size_.height)));
    config.presentMode = presentMode;
    config.alphaMode = alphaMode;
    wgpuSurfaceConfigure(surface_, &config);

    wgpuSurfaceCapabilitiesFreeMembers(capabilities);

    if (pipelineFormat_ != WGPUTextureFormat_Undefined && pipelineFormat_ != surfaceFormat_) {
      releaseRectPipeline();
    }
    if (imagePipelineFormat_ != WGPUTextureFormat_Undefined && imagePipelineFormat_ != surfaceFormat_) {
      releaseImagePipeline();
    }
    if (pathPipelineFormat_ != WGPUTextureFormat_Undefined && pathPipelineFormat_ != surfaceFormat_) {
      releasePathPipeline();
    }
  }

  void ensureOffscreenTarget() {
    std::uint32_t const width = std::max(1u, offscreenPixelWidth_);
    std::uint32_t const height = std::max(1u, offscreenPixelHeight_);
    if (offscreenTexture_ && offscreenPixelWidth_ == width && offscreenPixelHeight_ == height) {
      return;
    }
    releaseOffscreenTarget();
    offscreenPixelWidth_ = width;
    offscreenPixelHeight_ = height;

    WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Offscreen Texture");
    descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = WGPUExtent3D{
        .width = offscreenPixelWidth_,
        .height = offscreenPixelHeight_,
        .depthOrArrayLayers = 1,
    };
    descriptor.format = surfaceFormat_;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    offscreenTexture_ = wgpuDeviceCreateTexture(context_.device(), &descriptor);
    if (!offscreenTexture_) {
      throw std::runtime_error("Lambda WebGPU: failed to create offscreen texture");
    }
  }

  void ensureFrameObjects() {
    if (currentTexture_ || currentView_) {
      return;
    }

    if (externalRenderTargetView_) {
      currentView_ = externalRenderTargetView_;
      wgpuTextureViewAddRef(currentView_);
    } else if (surface_) {
      WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
      wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture);
      if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
          surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        throw std::runtime_error("Lambda WebGPU: failed to acquire surface texture");
      }
      currentTexture_ = surfaceTexture.texture;
    } else {
      ensureOffscreenTarget();
      currentTexture_ = offscreenTexture_;
    }

    if (!currentView_) {
      WGPUTextureViewDescriptor viewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
      viewDescriptor.label = stringView(surface_ ? "LambdaUI WebGPU Surface View" : "LambdaUI WebGPU Offscreen View");
      currentView_ = wgpuTextureCreateView(currentTexture_, &viewDescriptor);
      if (!currentView_) {
        throw std::runtime_error("Lambda WebGPU: failed to create frame texture view");
      }
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("LambdaUI WebGPU Command Encoder");
    commandEncoder_ = wgpuDeviceCreateCommandEncoder(context_.device(), &encoderDescriptor);
    if (!commandEncoder_) {
      throw std::runtime_error("Lambda WebGPU: failed to create command encoder");
    }
  }

  WGPUTexture createBackdropTexture(char const* label, WGPUTextureUsage usage) {
    WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = stringView(label);
    descriptor.usage = usage;
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = WGPUExtent3D{
        .width = backdropPixelWidth_,
        .height = backdropPixelHeight_,
        .depthOrArrayLayers = 1,
    };
    descriptor.format = surfaceFormat_;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    WGPUTexture texture = wgpuDeviceCreateTexture(context_.device(), &descriptor);
    if (!texture) {
      throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur texture");
    }
    return texture;
  }

  WGPUTextureView createBackdropView(WGPUTexture texture, char const* label) {
    WGPUTextureViewDescriptor descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    descriptor.label = stringView(label);
    WGPUTextureView view = wgpuTextureCreateView(texture, &descriptor);
    if (!view) {
      throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur texture view");
    }
    return view;
  }

  void ensureBackdropTargets() {
    std::uint32_t const width = targetPixelWidth();
    std::uint32_t const height = targetPixelHeight();
    if (backdropSourceTexture_ && backdropPixelWidth_ == width && backdropPixelHeight_ == height) {
      return;
    }
    releaseBackdropTargets();
    backdropPixelWidth_ = width;
    backdropPixelHeight_ = height;

    backdropSourceTexture_ = createBackdropTexture("LambdaUI WebGPU Backdrop Source",
                                                  WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding);
    backdropSourceView_ = createBackdropView(backdropSourceTexture_, "LambdaUI WebGPU Backdrop Source View");
    backdropScratchTexture_ = createBackdropTexture("LambdaUI WebGPU Backdrop Scratch",
                                                   WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
    backdropScratchView_ = createBackdropView(backdropScratchTexture_, "LambdaUI WebGPU Backdrop Scratch View");
    backdropBlurTexture_ = createBackdropTexture("LambdaUI WebGPU Backdrop Blur",
                                                WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
    backdropBlurView_ = createBackdropView(backdropBlurTexture_, "LambdaUI WebGPU Backdrop Blur View");
  }

  WGPURenderPassEncoder beginFrameRenderPass(WGPULoadOp loadOp, char const* label) {
    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = currentView_;
    colorAttachment.loadOp = loadOp;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = toWebGpuColor(clearColor_);

    WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView(label);
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &colorAttachment;
    return wgpuCommandEncoderBeginRenderPass(commandEncoder_, &passDescriptor);
  }

  void drawOpRange(WGPURenderPassEncoder pass, std::size_t start, std::size_t end) {
    if (!pass) {
      return;
    }
    WGPURenderPipeline activePipeline = nullptr;
    for (std::size_t i = start; i < end && i < drawOps_.size(); ++i) {
      WebGpuDrawOp const& op = drawOps_[i];
      switch (op.kind) {
        case WebGpuDrawOp::Kind::Rect:
          if (!rectBindGroup_) {
            break;
          }
          if (WGPURenderPipeline pipeline = ensureRectPipeline(op.blendMode); activePipeline != pipeline) {
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, rectBindGroup_, 0, nullptr);
            activePipeline = pipeline;
          }
          wgpuRenderPassEncoderDraw(pass, 6, op.count, 0, op.first);
          break;
        case WebGpuDrawOp::Kind::Image:
          drawImageOp(pass, op, activePipeline, nullptr);
          break;
        case WebGpuDrawOp::Kind::Path:
          if (!pathBuffer_) {
            break;
          }
          if (WGPURenderPipeline pipeline = ensurePathPipeline(op.blendMode); activePipeline != pipeline) {
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            activePipeline = pipeline;
          }
          wgpuRenderPassEncoderSetVertexBuffer(pass, 0, pathBuffer_, 0, pathBufferCapacity_);
          wgpuRenderPassEncoderDraw(pass, op.count, 1, op.first, 0);
          break;
        case WebGpuDrawOp::Kind::BackdropBlur:
          break;
      }
    }
  }

  void encodeFramePass() {
    ensureFrameObjects();
    uploadRectResources();
    uploadImageResources();
    uploadPathResources();

    WGPURenderPassEncoder pass = beginFrameRenderPass(WGPULoadOp_Clear, "LambdaUI WebGPU Frame Pass");
    std::size_t segmentStart = 0;
    for (std::size_t i = 0; i < drawOps_.size(); ++i) {
      if (drawOps_[i].kind != WebGpuDrawOp::Kind::BackdropBlur) {
        continue;
      }
      drawOpRange(pass, segmentStart, i);
      if (pass) {
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
      }
      encodeBackdropBlurOp(drawOps_[i]);
      pass = beginFrameRenderPass(WGPULoadOp_Load, "LambdaUI WebGPU Frame Continue Pass");
      segmentStart = i + 1;
    }
    drawOpRange(pass, segmentStart, drawOps_.size());
    if (pass) {
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
  }

  bool frameCaptureFormatSupported() const noexcept {
    return surfaceFormat_ == WGPUTextureFormat_BGRA8Unorm || surfaceFormat_ == WGPUTextureFormat_RGBA8Unorm;
  }

  bool enqueueFrameCaptureCopy() {
    if (!frameCaptureRequested_) {
      return false;
    }
    frameCaptureRequested_ = false;
    capturedFrameAvailable_ = false;
    capturedFrameBytes_.clear();
    if (!surfaceCopySrcSupported_ || !frameCaptureFormatSupported() || !commandEncoder_ || !currentTexture_) {
      return false;
    }

    pendingFrameCaptureWidth_ =
        surface_ ? std::max(1u, static_cast<std::uint32_t>(std::lround(size_.width))) : offscreenPixelWidth_;
    pendingFrameCaptureHeight_ =
        surface_ ? std::max(1u, static_cast<std::uint32_t>(std::lround(size_.height))) : offscreenPixelHeight_;
    pendingFrameCaptureBytesPerRow_ =
        alignTo(pendingFrameCaptureWidth_ * 4u, kWebGpuCopyBytesPerRowAlignment);
    pendingFrameCaptureBufferSize_ =
        static_cast<std::uint64_t>(pendingFrameCaptureBytesPerRow_) * pendingFrameCaptureHeight_;
    pendingFrameCaptureFormat_ = surfaceFormat_;

    if (pendingFrameCaptureBuffer_) {
      wgpuBufferRelease(pendingFrameCaptureBuffer_);
      pendingFrameCaptureBuffer_ = nullptr;
    }
    WGPUBufferDescriptor bufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    bufferDescriptor.label = stringView("LambdaUI WebGPU Frame Capture Readback");
    bufferDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    bufferDescriptor.size = pendingFrameCaptureBufferSize_;
    pendingFrameCaptureBuffer_ = wgpuDeviceCreateBuffer(context_.device(), &bufferDescriptor);
    if (!pendingFrameCaptureBuffer_) {
      clearPendingFrameCapture();
      return false;
    }

    WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    source.texture = currentTexture_;
    source.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    destination.buffer = pendingFrameCaptureBuffer_;
    destination.layout.bytesPerRow = pendingFrameCaptureBytesPerRow_;
    destination.layout.rowsPerImage = pendingFrameCaptureHeight_;
    WGPUExtent3D copySize = WGPU_EXTENT_3D_INIT;
    copySize.width = pendingFrameCaptureWidth_;
    copySize.height = pendingFrameCaptureHeight_;
    copySize.depthOrArrayLayers = 1;
    wgpuCommandEncoderCopyTextureToBuffer(commandEncoder_, &source, &destination, &copySize);
    return true;
  }

  void finishFrameCaptureReadback() {
    if (!pendingFrameCaptureBuffer_ || pendingFrameCaptureWidth_ == 0 || pendingFrameCaptureHeight_ == 0 ||
        pendingFrameCaptureBufferSize_ == 0) {
      clearPendingFrameCapture();
      return;
    }

    WebGpuMapWaitState mapState{};
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = frameCaptureMapCallback;
    callback.userdata1 = &mapState;
    WGPUFuture const future = wgpuBufferMapAsync(pendingFrameCaptureBuffer_,
                                                 WGPUMapMode_Read,
                                                 0,
                                                 static_cast<std::size_t>(pendingFrameCaptureBufferSize_),
                                                 callback);
    WGPUFutureWaitInfo waitInfo = WGPU_FUTURE_WAIT_INFO_INIT;
    waitInfo.future = future;
    WGPUWaitStatus const waitStatus = wgpuInstanceWaitAny(context_.instance(), 1, &waitInfo, UINT64_MAX);
    if (waitStatus != WGPUWaitStatus_Success || !waitInfo.completed || !mapState.done ||
        mapState.status != WGPUMapAsyncStatus_Success) {
      clearPendingFrameCapture();
      return;
    }

    auto const* mapped = static_cast<std::uint8_t const*>(
        wgpuBufferGetConstMappedRange(pendingFrameCaptureBuffer_,
                                      0,
                                      static_cast<std::size_t>(pendingFrameCaptureBufferSize_)));
    if (!mapped) {
      clearPendingFrameCapture();
      return;
    }

    capturedFrameBytes_.resize(static_cast<std::size_t>(pendingFrameCaptureWidth_) *
                               pendingFrameCaptureHeight_ * 4u);
    for (std::uint32_t row = 0; row < pendingFrameCaptureHeight_; ++row) {
      std::uint8_t const* src = mapped + static_cast<std::size_t>(row) * pendingFrameCaptureBytesPerRow_;
      std::uint8_t* dst = capturedFrameBytes_.data() +
                          static_cast<std::size_t>(row) * pendingFrameCaptureWidth_ * 4u;
      if (pendingFrameCaptureFormat_ == WGPUTextureFormat_BGRA8Unorm) {
        for (std::uint32_t x = 0; x < pendingFrameCaptureWidth_; ++x) {
          dst[x * 4u + 0u] = src[x * 4u + 2u];
          dst[x * 4u + 1u] = src[x * 4u + 1u];
          dst[x * 4u + 2u] = src[x * 4u + 0u];
          dst[x * 4u + 3u] = src[x * 4u + 3u];
        }
      } else {
        std::memcpy(dst, src, static_cast<std::size_t>(pendingFrameCaptureWidth_) * 4u);
      }
    }
    capturedFrameWidth_ = pendingFrameCaptureWidth_;
    capturedFrameHeight_ = pendingFrameCaptureHeight_;
    capturedFrameAvailable_ = true;
    clearPendingFrameCapture();
  }

  void clearPendingFrameCapture() noexcept {
    if (pendingFrameCaptureBuffer_) {
      if (wgpuBufferGetMapState(pendingFrameCaptureBuffer_) == WGPUBufferMapState_Mapped) {
        wgpuBufferUnmap(pendingFrameCaptureBuffer_);
      }
      wgpuBufferRelease(pendingFrameCaptureBuffer_);
      pendingFrameCaptureBuffer_ = nullptr;
    }
    pendingFrameCaptureWidth_ = 0;
    pendingFrameCaptureHeight_ = 0;
    pendingFrameCaptureBytesPerRow_ = 0;
    pendingFrameCaptureBufferSize_ = 0;
    pendingFrameCaptureFormat_ = WGPUTextureFormat_Undefined;
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
    if (currentTexture_ && surface_) {
      wgpuTextureRelease(currentTexture_);
    }
    currentTexture_ = nullptr;
  }

  void releaseOffscreenTarget() noexcept {
    if (offscreenTexture_) {
      wgpuTextureRelease(offscreenTexture_);
      offscreenTexture_ = nullptr;
    }
  }

  void releaseBackdropTargets() noexcept {
    if (backdropSourceView_) {
      wgpuTextureViewRelease(backdropSourceView_);
      backdropSourceView_ = nullptr;
    }
    if (backdropSourceTexture_) {
      wgpuTextureRelease(backdropSourceTexture_);
      backdropSourceTexture_ = nullptr;
    }
    if (backdropScratchView_) {
      wgpuTextureViewRelease(backdropScratchView_);
      backdropScratchView_ = nullptr;
    }
    if (backdropScratchTexture_) {
      wgpuTextureRelease(backdropScratchTexture_);
      backdropScratchTexture_ = nullptr;
    }
    if (backdropBlurView_) {
      wgpuTextureViewRelease(backdropBlurView_);
      backdropBlurView_ = nullptr;
    }
    if (backdropBlurTexture_) {
      wgpuTextureRelease(backdropBlurTexture_);
      backdropBlurTexture_ = nullptr;
    }
    backdropPixelWidth_ = 0;
    backdropPixelHeight_ = 0;
  }

  void releaseExternalRenderTarget() noexcept {
    if (externalRenderTargetView_) {
      wgpuTextureViewRelease(externalRenderTargetView_);
      externalRenderTargetView_ = nullptr;
    }
  }

  void pushRectInstance(Rect const& rect,
                        CornerRadius const& cornerRadius,
                        FillStyle const& fill,
                        StrokeStyle const& stroke,
                        float opacity) {
    if (rect.width <= 0.f || rect.height <= 0.f) {
      return;
    }
    if (clip_.width <= 0.f || clip_.height <= 0.f || !clip_.intersects(transformedBounds(rect))) {
      return;
    }
    Point const p0 = transform_.apply({rect.x, rect.y});
    Point const p1 = transform_.apply({rect.x + rect.width, rect.y});
    Point const p3 = transform_.apply({rect.x, rect.y + rect.height});
    WebGpuRectInstance instance{};
    instance.rect[2] = rect.width;
    instance.rect[3] = rect.height;
    instance.axisX[0] = p0.x;
    instance.axisX[1] = p0.y;
    instance.axisX[2] = p1.x - p0.x;
    instance.axisX[3] = p1.y - p0.y;
    instance.axisY[0] = p3.x - p0.x;
    instance.axisY[1] = p3.y - p0.y;
    CornerRadius const radii = clampedRadii(cornerRadius, rect.width, rect.height);
    instance.radii[0] = radii.topLeft;
    instance.radii[1] = radii.topRight;
    instance.radii[2] = radii.bottomRight;
    instance.radii[3] = radii.bottomLeft;
    encodeFill(fill, instance);
    Color strokeColor{};
    if (stroke.solidColor(&strokeColor) && stroke.width > 0.f) {
      putColor(instance.stroke, strokeColor);
      instance.params[2] = stroke.width;
    }
    instance.params[3] = opacity;
    applyCurrentClip(instance);
    std::uint32_t const first = static_cast<std::uint32_t>(rects_.size());
    rects_.push_back(instance);
    drawOps_.push_back(WebGpuDrawOp{
        .kind = WebGpuDrawOp::Kind::Rect,
        .first = first,
        .count = 1,
        .blendMode = blendMode_,
    });
  }

  void pushImageInstance(WebGpuImage const& image,
                         Rect const& src,
                         Rect const& dst,
                         CornerRadius const& corners,
                         Color tint,
                         float opacity) {
    Size const imageSize = image.size();
    if (imageSize.width <= 0.f || imageSize.height <= 0.f ||
        src.width <= 0.f || src.height <= 0.f || dst.width <= 0.f || dst.height <= 0.f) {
      return;
    }
    if (clip_.width <= 0.f || clip_.height <= 0.f || !clip_.intersects(transformedBounds(dst))) {
      return;
    }

    Point const p0 = transform_.apply({dst.x, dst.y});
    Point const p1 = transform_.apply({dst.x + dst.width, dst.y});
    Point const p3 = transform_.apply({dst.x, dst.y + dst.height});
    WebGpuQuadInstance instance{};
    instance.rect[2] = dst.width;
    instance.rect[3] = dst.height;
    instance.axisX[0] = p0.x;
    instance.axisX[1] = p0.y;
    instance.axisX[2] = p1.x - p0.x;
    instance.axisX[3] = p1.y - p0.y;
    instance.axisY[0] = p3.x - p0.x;
    instance.axisY[1] = p3.y - p0.y;
    instance.uv[0] = src.x / imageSize.width;
    instance.uv[1] = src.y / imageSize.height;
    instance.uv[2] = (src.x + src.width) / imageSize.width;
    instance.uv[3] = (src.y + src.height) / imageSize.height;
    putColor(instance.color, tint, opacity_ * opacity);
    CornerRadius const radii = clampedRadii(corners, dst.width, dst.height);
    instance.radii[0] = radii.topLeft;
    instance.radii[1] = radii.topRight;
    instance.radii[2] = radii.bottomRight;
    instance.radii[3] = radii.bottomLeft;
    applyCurrentClip(instance);
    std::uint32_t const first = static_cast<std::uint32_t>(quads_.size());
    quads_.push_back(instance);
    WebGpuDrawOp op{
        .kind = WebGpuDrawOp::Kind::Image,
        .first = first,
        .count = 1,
        .blendMode = blendMode_,
        .image = &image,
    };
    try {
      op.imageRef = image.shared_from_this();
    } catch (std::bad_weak_ptr const&) {
      op.imageRef.reset();
    }
    drawOps_.push_back(std::move(op));
  }

  void pushBackdropBlurInstance(Rect const& rect, CornerRadius const& corners, float radius) {
    if (clip_.width <= 0.f || clip_.height <= 0.f || !clip_.intersects(transformedBounds(rect))) {
      return;
    }

    Point const p0 = transform_.apply({rect.x, rect.y});
    Point const p1 = transform_.apply({rect.x + rect.width, rect.y});
    Point const p3 = transform_.apply({rect.x, rect.y + rect.height});
    WebGpuQuadInstance instance{};
    instance.rect[2] = rect.width;
    instance.rect[3] = rect.height;
    instance.axisX[0] = p0.x;
    instance.axisX[1] = p0.y;
    instance.axisX[2] = p1.x - p0.x;
    instance.axisX[3] = p1.y - p0.y;
    instance.axisY[0] = p3.x - p0.x;
    instance.axisY[1] = p3.y - p0.y;
    float const viewportWidth = std::max(1.f, size_.width);
    float const viewportHeight = std::max(1.f, size_.height);
    instance.uv[0] = rect.x / viewportWidth;
    instance.uv[1] = rect.y / viewportHeight;
    instance.uv[2] = (rect.x + rect.width) / viewportWidth;
    instance.uv[3] = (rect.y + rect.height) / viewportHeight;
    putColor(instance.color, Colors::white, opacity_);
    CornerRadius const radii = clampedRadii(corners, rect.width, rect.height);
    instance.radii[0] = radii.topLeft;
    instance.radii[1] = radii.topRight;
    instance.radii[2] = radii.bottomRight;
    instance.radii[3] = radii.bottomLeft;
    applyCurrentClip(instance);
    std::uint32_t const first = static_cast<std::uint32_t>(quads_.size());
    quads_.push_back(instance);
    drawOps_.push_back(WebGpuDrawOp{
        .kind = WebGpuDrawOp::Kind::BackdropBlur,
        .first = first,
        .count = 1,
        .blendMode = blendMode_,
        .blurRadiusPx = std::clamp(radius * targetDpiScale(), 1.f, static_cast<float>(kWebGpuBackdropBlurMaxRadiusPx)),
    });
  }

  void appendPath(Path const& path, FillStyle const& fill, StrokeStyle const& stroke) {
    if (path.isEmpty() || size_.width < 1.f || size_.height < 1.f) {
      return;
    }

    auto subpaths = PathFlattener::flattenSubpaths(path);
    if (subpaths.empty()) {
      return;
    }
    for (std::vector<Point>& subpath : subpaths) {
      for (Point& point : subpath) {
        point = transform_.apply(point);
      }
    }

    std::uint32_t const firstVertex = static_cast<std::uint32_t>(pathVertices_.size());
    float const viewportW = std::max(1.f, size_.width);
    float const viewportH = std::max(1.f, size_.height);
    Rect const fillBounds = boundsOfSubpaths(subpaths);
    if (clip_.width <= 0.f || clip_.height <= 0.f || !clip_.intersects(fillBounds)) {
      return;
    }

    auto appendVertices = [this](TessellatedPath&& tessellated) {
      if (tessellated.vertices.empty()) {
        return;
      }
      pathVertices_.reserve(pathVertices_.size() + tessellated.vertices.size());
      for (PathVertex const& vertex : tessellated.vertices) {
        pathVertices_.push_back(makePathVertex(vertex));
      }
    };

    if (!fill.isNone()) {
      Color fillColor{};
      if (representativeFillColor(fill, &fillColor)) {
        fillColor.a *= opacity_;
        std::vector<std::vector<Point>> contours;
        contours.reserve(subpaths.size());
        for (std::vector<Point> const& subpath : subpaths) {
          if (subpath.size() >= 3) {
            contours.push_back(subpath);
          }
        }
        if (!contours.empty()) {
          TessellatedPath tessellated = PathFlattener::tessellateFillContours(
              contours, fillColor, viewportW, viewportH, PathFlattener::tessWindingFromFillRule(fill.fillRule));
          (void)applyPathGradientFill(tessellated, fill, fillBounds, opacity_);
          appendVertices(std::move(tessellated));
        }
      }
    }

    if (!stroke.isNone()) {
      Color strokeColor{};
      if (stroke.solidColor(&strokeColor) && stroke.width > 0.f) {
        strokeColor.a *= opacity_;
        float const scaleX = std::hypot(transform_.m[0], transform_.m[1]);
        float const scaleY = std::hypot(transform_.m[3], transform_.m[4]);
        float const strokeScale = (scaleX > 0.f || scaleY > 0.f) ? (scaleX + scaleY) * 0.5f : 1.f;
        for (std::vector<Point> const& subpath : subpaths) {
          if (subpath.size() >= 2) {
            appendVertices(PathFlattener::tessellateStroke(
                subpath, stroke.width * strokeScale, strokeColor, viewportW, viewportH, stroke.join, stroke.cap));
          }
        }
      }
    }

    std::uint32_t const vertexCount = static_cast<std::uint32_t>(pathVertices_.size()) - firstVertex;
    if (vertexCount > 0) {
      drawOps_.push_back(WebGpuDrawOp{
          .kind = WebGpuDrawOp::Kind::Path,
          .first = firstVertex,
          .count = vertexCount,
          .blendMode = blendMode_,
      });
    }
  }

  GlyphImage const* glyphImage(std::uint32_t fontId, std::uint32_t glyphId, float fontSize) {
    std::uint16_t const pixelSize =
        static_cast<std::uint16_t>(std::clamp(std::round(fontSize * dpiScale_), 1.f, 512.f));
    GlyphKey const key{
        .fontId = fontId,
        .glyphId = glyphId,
        .pixelSize = pixelSize,
    };
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) {
      return &it->second;
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Point bearing{};
    std::vector<std::uint8_t> alpha = textSystem_.rasterizeGlyph(fontId, glyphId, static_cast<float>(pixelSize),
                                                                 width, height, bearing);
    GlyphImage glyph{};
    glyph.width = width;
    glyph.height = height;
    glyph.bearing = bearing;
    if (width > 0 && height > 0 && alpha.size() == static_cast<std::size_t>(width) * height) {
      std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u);
      for (std::size_t i = 0; i < alpha.size(); ++i) {
        pixels[i * 4u + 0u] = 255;
        pixels[i * 4u + 1u] = 255;
        pixels[i * 4u + 2u] = 255;
        pixels[i * 4u + 3u] = alpha[i];
      }
      glyph.image = std::make_shared<WebGpuImage>(width,
                                                  height,
                                                  std::move(pixels),
                                                  Image::PixelFormat::Rgba8888,
                                                  false);
    }
    auto [inserted, ok] = glyphs_.emplace(key, std::move(glyph));
    (void)ok;
    return &inserted->second;
  }

  bool ensureBuffer(WGPUBuffer& buffer,
                    std::uint64_t& capacity,
                    std::uint64_t requiredSize,
                    WGPUBufferUsage usage,
                    char const* label) {
    requiredSize = std::max<std::uint64_t>(requiredSize, 16);
    if (buffer && capacity >= requiredSize) {
      return false;
    }
    if (buffer) {
      wgpuBufferRelease(buffer);
      buffer = nullptr;
    }
    WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.label = stringView(label);
    descriptor.usage = usage;
    descriptor.size = requiredSize;
    buffer = wgpuDeviceCreateBuffer(context_.device(), &descriptor);
    if (!buffer) {
      throw std::runtime_error(std::string("Lambda WebGPU: failed to create buffer: ") + label);
    }
    capacity = requiredSize;
    return true;
  }

  void uploadRectResources() {
    if (rects_.empty()) {
      return;
    }
    bool resized = ensureBuffer(rectBuffer_,
                                rectBufferCapacity_,
                                static_cast<std::uint64_t>(rects_.size() * sizeof(WebGpuRectInstance)),
                                WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                                "LambdaUI WebGPU Rect Instances");
    resized = ensureBuffer(frameUniformBuffer_,
                           frameUniformBufferCapacity_,
                           sizeof(WebGpuFrameUniforms),
                           WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                           "LambdaUI WebGPU Frame Uniforms") ||
              resized;
    if (resized && rectBindGroup_) {
      wgpuBindGroupRelease(rectBindGroup_);
      rectBindGroup_ = nullptr;
    }
    WebGpuFrameUniforms uniforms{};
    uniforms.viewport[0] = std::max(1.f, size_.width);
    uniforms.viewport[1] = std::max(1.f, size_.height);
    wgpuQueueWriteBuffer(context_.queue(), rectBuffer_, 0, rects_.data(), rects_.size() * sizeof(WebGpuRectInstance));
    wgpuQueueWriteBuffer(context_.queue(), frameUniformBuffer_, 0, &uniforms, sizeof(uniforms));
    ensureRectBindGroup();
  }

  void uploadImageResources() {
    if (quads_.empty()) {
      return;
    }
    bool resized = ensureBuffer(quadBuffer_,
                                quadBufferCapacity_,
                                static_cast<std::uint64_t>(quads_.size() * sizeof(WebGpuQuadInstance)),
                                WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                                "LambdaUI WebGPU Image Quads");
    resized = ensureBuffer(imageFrameUniformBuffer_,
                           imageFrameUniformBufferCapacity_,
                           sizeof(WebGpuFrameUniforms),
                           WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                           "LambdaUI WebGPU Image Frame Uniforms") ||
              resized;
    if (resized && imageFrameBindGroup_) {
      wgpuBindGroupRelease(imageFrameBindGroup_);
      imageFrameBindGroup_ = nullptr;
    }
    WebGpuFrameUniforms uniforms{};
    uniforms.viewport[0] = std::max(1.f, size_.width);
    uniforms.viewport[1] = std::max(1.f, size_.height);
    wgpuQueueWriteBuffer(context_.queue(), quadBuffer_, 0, quads_.data(), quads_.size() * sizeof(WebGpuQuadInstance));
    wgpuQueueWriteBuffer(context_.queue(), imageFrameUniformBuffer_, 0, &uniforms, sizeof(uniforms));
    ensureImageFrameBindGroup();
  }

  void uploadPathResources() {
    if (pathVertices_.empty()) {
      return;
    }
    ensureBuffer(pathBuffer_,
                 pathBufferCapacity_,
                 static_cast<std::uint64_t>(pathVertices_.size() * sizeof(WebGpuPathVertex)),
                 WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                 "LambdaUI WebGPU Path Vertices");
    wgpuQueueWriteBuffer(context_.queue(),
                         pathBuffer_,
                         0,
                         pathVertices_.data(),
                         pathVertices_.size() * sizeof(WebGpuPathVertex));
  }

  void drawImageOp(WGPURenderPassEncoder pass,
                   WebGpuDrawOp const& op,
                   WGPURenderPipeline& activePipeline,
                   WGPUTextureView textureViewOverride) {
    if (!imageFrameBindGroup_) {
      return;
    }
    if (WGPURenderPipeline pipeline = ensureImagePipeline(op.blendMode); activePipeline != pipeline) {
      wgpuRenderPassEncoderSetPipeline(pass, pipeline);
      wgpuRenderPassEncoderSetBindGroup(pass, 0, imageFrameBindGroup_, 0, nullptr);
      activePipeline = pipeline;
    }
    try {
      WGPUTextureView view = textureViewOverride;
      if (!view) {
        if (!op.image) {
          return;
        }
        view = op.image->textureView(context_.device(), context_.queue());
      }
      if (!view) {
        return;
      }
      WGPUBindGroup textureBindGroup = createImageTextureBindGroup(view);
      wgpuRenderPassEncoderSetBindGroup(pass, 1, textureBindGroup, 0, nullptr);
      wgpuRenderPassEncoderDraw(pass, 6, op.count, 0, op.first);
      wgpuBindGroupRelease(textureBindGroup);
    } catch (std::exception const& e) {
      std::fprintf(stderr, "Lambda WebGPU: image upload failed: %s\n", e.what());
    }
  }

  bool backdropCopySupported() const noexcept {
    return currentTexture_ && !externalRenderTargetView_ && (!surface_ || surfaceCopySrcSupported_);
  }

  void copyCurrentTargetToBackdropSource() {
    WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    source.texture = currentTexture_;
    source.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = backdropSourceTexture_;
    destination.aspect = WGPUTextureAspect_All;
    WGPUExtent3D copySize = WGPU_EXTENT_3D_INIT;
    copySize.width = backdropPixelWidth_;
    copySize.height = backdropPixelHeight_;
    copySize.depthOrArrayLayers = 1;
    wgpuCommandEncoderCopyTextureToTexture(commandEncoder_, &source, &destination, &copySize);
  }

  void ensureBlurSampler() {
    if (blurSampler_) {
      return;
    }
    WGPUSamplerDescriptor descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Backdrop Blur Sampler");
    descriptor.addressModeU = WGPUAddressMode_ClampToEdge;
    descriptor.addressModeV = WGPUAddressMode_ClampToEdge;
    descriptor.addressModeW = WGPUAddressMode_ClampToEdge;
    descriptor.magFilter = WGPUFilterMode_Linear;
    descriptor.minFilter = WGPUFilterMode_Linear;
    descriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    blurSampler_ = wgpuDeviceCreateSampler(context_.device(), &descriptor);
    if (!blurSampler_) {
      throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur sampler");
    }
  }

  void ensureBlurUniformBuffer() {
    ensureBuffer(blurUniformBuffer_,
                 blurUniformBufferCapacity_,
                 sizeof(WebGpuBlurUniforms),
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                 "LambdaUI WebGPU Backdrop Blur Uniforms");
  }

  WGPUBindGroup createBlurBindGroup(WGPUTextureView sourceView) {
    ensureBlurSampler();
    ensureBlurUniformBuffer();
    (void)ensureBlurPipeline();
    WGPUBindGroupEntry entries[3] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
    };
    entries[0].binding = 0;
    entries[0].sampler = blurSampler_;
    entries[1].binding = 1;
    entries[1].textureView = sourceView;
    entries[2].binding = 2;
    entries[2].buffer = blurUniformBuffer_;
    entries[2].size = sizeof(WebGpuBlurUniforms);

    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Backdrop Blur Bind Group");
    descriptor.layout = blurBindGroupLayout_;
    descriptor.entryCount = 3;
    descriptor.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(context_.device(), &descriptor);
    if (!bindGroup) {
      throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur bind group");
    }
    return bindGroup;
  }

  WGPURenderPipeline ensureBlurPipeline() {
    if (blurPipelineFormat_ != WGPUTextureFormat_Undefined && blurPipelineFormat_ != surfaceFormat_) {
      releaseBlurPipeline();
    }
    if (blurPipeline_) {
      return blurPipeline_;
    }
    if (!blurBindGroupLayout_) {
      WGPUBindGroupLayoutEntry entries[3] = {
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
      };
      entries[0].binding = 0;
      entries[0].visibility = WGPUShaderStage_Fragment;
      entries[0].sampler.type = WGPUSamplerBindingType_Filtering;
      entries[1].binding = 1;
      entries[1].visibility = WGPUShaderStage_Fragment;
      entries[1].texture.sampleType = WGPUTextureSampleType_Float;
      entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
      entries[2].binding = 2;
      entries[2].visibility = WGPUShaderStage_Fragment;
      entries[2].buffer.type = WGPUBufferBindingType_Uniform;
      entries[2].buffer.minBindingSize = sizeof(WebGpuBlurUniforms);

      WGPUBindGroupLayoutDescriptor descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
      descriptor.label = stringView("LambdaUI WebGPU Backdrop Blur Bind Group Layout");
      descriptor.entryCount = 3;
      descriptor.entries = entries;
      blurBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(context_.device(), &descriptor);
      if (!blurBindGroupLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur bind group layout");
      }
    }
    if (!blurPipelineLayout_) {
      WGPUPipelineLayoutDescriptor descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
      descriptor.label = stringView("LambdaUI WebGPU Backdrop Blur Pipeline Layout");
      descriptor.bindGroupLayoutCount = 1;
      descriptor.bindGroupLayouts = &blurBindGroupLayout_;
      blurPipelineLayout_ = wgpuDeviceCreatePipelineLayout(context_.device(), &descriptor);
      if (!blurPipelineLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur pipeline layout");
      }
    }

    WGPUShaderSourceWGSL shaderSource = WGPU_SHADER_SOURCE_WGSL_INIT;
    shaderSource.code = stringView(kBlurShaderWgsl);
    WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDescriptor.label = stringView("LambdaUI WebGPU Backdrop Blur Shader");
    shaderDescriptor.nextInChain = &shaderSource.chain;
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(context_.device(), &shaderDescriptor);
    if (!shader) {
      throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur shader module");
    }

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader;
    fragment.entryPoint = stringView("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Backdrop Blur Pipeline");
    descriptor.layout = blurPipelineLayout_;
    descriptor.vertex.module = shader;
    descriptor.vertex.entryPoint = stringView("vs_main");
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    descriptor.fragment = &fragment;

    blurPipeline_ = wgpuDeviceCreateRenderPipeline(context_.device(), &descriptor);
    wgpuShaderModuleRelease(shader);
    if (!blurPipeline_) {
      throw std::runtime_error("Lambda WebGPU: failed to create backdrop blur pipeline");
    }
    blurPipelineFormat_ = surfaceFormat_;
    return blurPipeline_;
  }

  void drawBlurPass(WGPUTextureView sourceView, WGPUTextureView destinationView, float axisX, float axisY, float radiusPx) {
    WebGpuBlurUniforms uniforms{};
    uniforms.direction[0] = axisX;
    uniforms.direction[1] = axisY;
    uniforms.radiusPx = radiusPx;
    ensureBlurUniformBuffer();
    wgpuQueueWriteBuffer(context_.queue(), blurUniformBuffer_, 0, &uniforms, sizeof(uniforms));
    WGPUBindGroup bindGroup = createBlurBindGroup(sourceView);

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = destinationView;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = WGPUColor{0.0, 0.0, 0.0, 0.0};

    WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("LambdaUI WebGPU Backdrop Blur Pass");
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &colorAttachment;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(commandEncoder_, &passDescriptor);
    if (pass) {
      wgpuRenderPassEncoderSetPipeline(pass, ensureBlurPipeline());
      wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
      wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
    wgpuBindGroupRelease(bindGroup);
  }

  void drawBackdropBlurResult(WebGpuDrawOp const& op) {
    WGPURenderPassEncoder pass = beginFrameRenderPass(WGPULoadOp_Load, "LambdaUI WebGPU Backdrop Composite Pass");
    if (!pass) {
      return;
    }
    WGPURenderPipeline activePipeline = nullptr;
    drawImageOp(pass, op, activePipeline, backdropBlurView_);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
  }

  void encodeBackdropBlurOp(WebGpuDrawOp const& op) {
    if (!backdropCopySupported() || !commandEncoder_ || op.count == 0 || op.blurRadiusPx <= 0.f) {
      return;
    }
    ensureBackdropTargets();
    copyCurrentTargetToBackdropSource();
    drawBlurPass(backdropSourceView_, backdropScratchView_, 1.f, 0.f, op.blurRadiusPx);
    drawBlurPass(backdropScratchView_, backdropBlurView_, 0.f, 1.f, op.blurRadiusPx);
    drawBackdropBlurResult(op);
  }

  void ensureRectBindGroup() {
    if (rectBindGroup_) {
      return;
    }
    (void)ensureRectPipeline(BlendMode::Normal);
    WGPUBindGroupEntry entries[2] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
    };
    entries[0].binding = 0;
    entries[0].buffer = rectBuffer_;
    entries[0].size = rectBufferCapacity_;
    entries[1].binding = 1;
    entries[1].buffer = frameUniformBuffer_;
    entries[1].size = sizeof(WebGpuFrameUniforms);

    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Rect Bind Group");
    descriptor.layout = rectBindGroupLayout_;
    descriptor.entryCount = 2;
    descriptor.entries = entries;
    rectBindGroup_ = wgpuDeviceCreateBindGroup(context_.device(), &descriptor);
    if (!rectBindGroup_) {
      throw std::runtime_error("Lambda WebGPU: failed to create rect bind group");
    }
  }

  WGPURenderPipeline ensureRectPipeline(BlendMode blendMode) {
    if (pipelineFormat_ != WGPUTextureFormat_Undefined && pipelineFormat_ != surfaceFormat_) {
      releaseRectPipeline();
    }
    std::size_t const pipelineIndex = webGpuBlendModeIndex(blendMode);
    if (rectPipelines_[pipelineIndex]) {
      return rectPipelines_[pipelineIndex];
    }

    if (!rectBindGroupLayout_) {
      WGPUBindGroupLayoutEntry entries[2] = {
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
      };
      entries[0].binding = 0;
      entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      entries[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
      entries[1].binding = 1;
      entries[1].visibility = WGPUShaderStage_Vertex;
      entries[1].buffer.type = WGPUBufferBindingType_Uniform;
      entries[1].buffer.minBindingSize = sizeof(WebGpuFrameUniforms);

      WGPUBindGroupLayoutDescriptor bindGroupLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
      bindGroupLayoutDescriptor.label = stringView("LambdaUI WebGPU Rect Bind Group Layout");
      bindGroupLayoutDescriptor.entryCount = 2;
      bindGroupLayoutDescriptor.entries = entries;
      rectBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(context_.device(), &bindGroupLayoutDescriptor);
      if (!rectBindGroupLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create rect bind group layout");
      }
    }

    if (!rectPipelineLayout_) {
      WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
      pipelineLayoutDescriptor.label = stringView("LambdaUI WebGPU Rect Pipeline Layout");
      pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
      pipelineLayoutDescriptor.bindGroupLayouts = &rectBindGroupLayout_;
      rectPipelineLayout_ = wgpuDeviceCreatePipelineLayout(context_.device(), &pipelineLayoutDescriptor);
      if (!rectPipelineLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create rect pipeline layout");
      }
    }

    WGPUShaderSourceWGSL shaderSource = WGPU_SHADER_SOURCE_WGSL_INIT;
    shaderSource.code = stringView(kRectShaderWgsl);
    WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDescriptor.label = stringView("LambdaUI WebGPU Rect Shader");
    shaderDescriptor.nextInChain = &shaderSource.chain;
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(context_.device(), &shaderDescriptor);
    if (!shader) {
      throw std::runtime_error("Lambda WebGPU: failed to create rect shader module");
    }

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    applyBlendModeToAttachment(blend, blendMode);

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.blend = &blend;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader;
    fragment.entryPoint = stringView("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    WGPURenderPipelineDescriptor pipelineDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDescriptor.label = stringView("LambdaUI WebGPU Rect Pipeline");
    pipelineDescriptor.layout = rectPipelineLayout_;
    pipelineDescriptor.vertex.module = shader;
    pipelineDescriptor.vertex.entryPoint = stringView("vs_main");
    pipelineDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDescriptor.fragment = &fragment;

    rectPipelines_[pipelineIndex] = wgpuDeviceCreateRenderPipeline(context_.device(), &pipelineDescriptor);
    wgpuShaderModuleRelease(shader);
    if (!rectPipelines_[pipelineIndex]) {
      throw std::runtime_error("Lambda WebGPU: failed to create rect pipeline");
    }
    pipelineFormat_ = surfaceFormat_;
    return rectPipelines_[pipelineIndex];
  }

  void ensureImageFrameBindGroup() {
    if (imageFrameBindGroup_) {
      return;
    }
    (void)ensureImagePipeline(BlendMode::Normal);
    WGPUBindGroupEntry entries[2] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
    };
    entries[0].binding = 0;
    entries[0].buffer = quadBuffer_;
    entries[0].size = quadBufferCapacity_;
    entries[1].binding = 1;
    entries[1].buffer = imageFrameUniformBuffer_;
    entries[1].size = sizeof(WebGpuFrameUniforms);

    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Image Frame Bind Group");
    descriptor.layout = imageFrameBindGroupLayout_;
    descriptor.entryCount = 2;
    descriptor.entries = entries;
    imageFrameBindGroup_ = wgpuDeviceCreateBindGroup(context_.device(), &descriptor);
    if (!imageFrameBindGroup_) {
      throw std::runtime_error("Lambda WebGPU: failed to create image frame bind group");
    }
  }

  void ensureImageSampler() {
    if (imageSampler_) {
      return;
    }
    WGPUSamplerDescriptor descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Image Sampler");
    descriptor.addressModeU = WGPUAddressMode_ClampToEdge;
    descriptor.addressModeV = WGPUAddressMode_ClampToEdge;
    descriptor.addressModeW = WGPUAddressMode_ClampToEdge;
    descriptor.magFilter = WGPUFilterMode_Linear;
    descriptor.minFilter = WGPUFilterMode_Linear;
    descriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    imageSampler_ = wgpuDeviceCreateSampler(context_.device(), &descriptor);
    if (!imageSampler_) {
      throw std::runtime_error("Lambda WebGPU: failed to create image sampler");
    }
  }

  WGPUBindGroup createImageTextureBindGroup(WGPUTextureView textureView) {
    (void)ensureImagePipeline(BlendMode::Normal);
    ensureImageSampler();
    WGPUBindGroupEntry entries[2] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
    };
    entries[0].binding = 0;
    entries[0].sampler = imageSampler_;
    entries[1].binding = 1;
    entries[1].textureView = textureView;

    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.label = stringView("LambdaUI WebGPU Image Texture Bind Group");
    descriptor.layout = imageTextureBindGroupLayout_;
    descriptor.entryCount = 2;
    descriptor.entries = entries;
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(context_.device(), &descriptor);
    if (!bindGroup) {
      throw std::runtime_error("Lambda WebGPU: failed to create image texture bind group");
    }
    return bindGroup;
  }

  WGPURenderPipeline ensureImagePipeline(BlendMode blendMode) {
    if (imagePipelineFormat_ != WGPUTextureFormat_Undefined && imagePipelineFormat_ != surfaceFormat_) {
      releaseImagePipeline();
    }
    std::size_t const pipelineIndex = webGpuBlendModeIndex(blendMode);
    if (imagePipelines_[pipelineIndex]) {
      return imagePipelines_[pipelineIndex];
    }

    if (!imageFrameBindGroupLayout_) {
      WGPUBindGroupLayoutEntry frameEntries[2] = {
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
      };
      frameEntries[0].binding = 0;
      frameEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      frameEntries[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
      frameEntries[1].binding = 1;
      frameEntries[1].visibility = WGPUShaderStage_Vertex;
      frameEntries[1].buffer.type = WGPUBufferBindingType_Uniform;
      frameEntries[1].buffer.minBindingSize = sizeof(WebGpuFrameUniforms);

      WGPUBindGroupLayoutDescriptor frameLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
      frameLayoutDescriptor.label = stringView("LambdaUI WebGPU Image Frame Bind Group Layout");
      frameLayoutDescriptor.entryCount = 2;
      frameLayoutDescriptor.entries = frameEntries;
      imageFrameBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(context_.device(), &frameLayoutDescriptor);
      if (!imageFrameBindGroupLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create image frame bind group layout");
      }
    }

    if (!imageTextureBindGroupLayout_) {
      WGPUBindGroupLayoutEntry textureEntries[2] = {
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
          WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
      };
      textureEntries[0].binding = 0;
      textureEntries[0].visibility = WGPUShaderStage_Fragment;
      textureEntries[0].sampler.type = WGPUSamplerBindingType_Filtering;
      textureEntries[1].binding = 1;
      textureEntries[1].visibility = WGPUShaderStage_Fragment;
      textureEntries[1].texture.sampleType = WGPUTextureSampleType_Float;
      textureEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

      WGPUBindGroupLayoutDescriptor textureLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
      textureLayoutDescriptor.label = stringView("LambdaUI WebGPU Image Texture Bind Group Layout");
      textureLayoutDescriptor.entryCount = 2;
      textureLayoutDescriptor.entries = textureEntries;
      imageTextureBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(context_.device(), &textureLayoutDescriptor);
      if (!imageTextureBindGroupLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create image texture bind group layout");
      }
    }

    if (!imagePipelineLayout_) {
      WGPUBindGroupLayout layouts[2] = {
          imageFrameBindGroupLayout_,
          imageTextureBindGroupLayout_,
      };
      WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
      pipelineLayoutDescriptor.label = stringView("LambdaUI WebGPU Image Pipeline Layout");
      pipelineLayoutDescriptor.bindGroupLayoutCount = 2;
      pipelineLayoutDescriptor.bindGroupLayouts = layouts;
      imagePipelineLayout_ = wgpuDeviceCreatePipelineLayout(context_.device(), &pipelineLayoutDescriptor);
      if (!imagePipelineLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create image pipeline layout");
      }
    }

    WGPUShaderSourceWGSL shaderSource = WGPU_SHADER_SOURCE_WGSL_INIT;
    shaderSource.code = stringView(kImageShaderWgsl);
    WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDescriptor.label = stringView("LambdaUI WebGPU Image Shader");
    shaderDescriptor.nextInChain = &shaderSource.chain;
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(context_.device(), &shaderDescriptor);
    if (!shader) {
      throw std::runtime_error("Lambda WebGPU: failed to create image shader module");
    }

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    applyBlendModeToAttachment(blend, blendMode);

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.blend = &blend;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader;
    fragment.entryPoint = stringView("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    WGPURenderPipelineDescriptor pipelineDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDescriptor.label = stringView("LambdaUI WebGPU Image Pipeline");
    pipelineDescriptor.layout = imagePipelineLayout_;
    pipelineDescriptor.vertex.module = shader;
    pipelineDescriptor.vertex.entryPoint = stringView("vs_main");
    pipelineDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDescriptor.fragment = &fragment;

    imagePipelines_[pipelineIndex] = wgpuDeviceCreateRenderPipeline(context_.device(), &pipelineDescriptor);
    wgpuShaderModuleRelease(shader);
    if (!imagePipelines_[pipelineIndex]) {
      throw std::runtime_error("Lambda WebGPU: failed to create image pipeline");
    }
    imagePipelineFormat_ = surfaceFormat_;
    ensureImageSampler();
    return imagePipelines_[pipelineIndex];
  }

  WGPURenderPipeline ensurePathPipeline(BlendMode blendMode) {
    if (pathPipelineFormat_ != WGPUTextureFormat_Undefined && pathPipelineFormat_ != surfaceFormat_) {
      releasePathPipeline();
    }
    std::size_t const pipelineIndex = webGpuBlendModeIndex(blendMode);
    if (pathPipelines_[pipelineIndex]) {
      return pathPipelines_[pipelineIndex];
    }

    if (!pathPipelineLayout_) {
      WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
      pipelineLayoutDescriptor.label = stringView("LambdaUI WebGPU Path Pipeline Layout");
      pathPipelineLayout_ = wgpuDeviceCreatePipelineLayout(context_.device(), &pipelineLayoutDescriptor);
      if (!pathPipelineLayout_) {
        throw std::runtime_error("Lambda WebGPU: failed to create path pipeline layout");
      }
    }

    WGPUShaderSourceWGSL shaderSource = WGPU_SHADER_SOURCE_WGSL_INIT;
    shaderSource.code = stringView(kPathShaderWgsl);
    WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    shaderDescriptor.label = stringView("LambdaUI WebGPU Path Shader");
    shaderDescriptor.nextInChain = &shaderSource.chain;
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(context_.device(), &shaderDescriptor);
    if (!shader) {
      throw std::runtime_error("Lambda WebGPU: failed to create path shader module");
    }

    WGPUVertexAttribute attributes[4 + kRoundedClipEntryCount];
    for (WGPUVertexAttribute& attribute : attributes) {
      attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
    }
    attributes[0].format = WGPUVertexFormat_Float32x2;
    attributes[0].offset = offsetof(WebGpuPathVertex, x);
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Float32x4;
    attributes[1].offset = offsetof(WebGpuPathVertex, color);
    attributes[1].shaderLocation = 1;
    attributes[2].format = WGPUVertexFormat_Float32x2;
    attributes[2].offset = offsetof(WebGpuPathVertex, viewport);
    attributes[2].shaderLocation = 2;
    attributes[3].format = WGPUVertexFormat_Float32x4;
    attributes[3].offset = offsetof(WebGpuPathVertex, clipHeader);
    attributes[3].shaderLocation = 3;
    for (std::uint32_t i = 0; i < kRoundedClipEntryCount; ++i) {
      WGPUVertexAttribute& attribute = attributes[4u + i];
      attribute.format = WGPUVertexFormat_Float32x4;
      attribute.offset = offsetof(WebGpuPathVertex, clipEntries) + static_cast<std::uint64_t>(i) * sizeof(float[4]);
      attribute.shaderLocation = 4u + i;
    }

    WGPUVertexBufferLayout vertexBufferLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vertexBufferLayout.stepMode = WGPUVertexStepMode_Vertex;
    vertexBufferLayout.arrayStride = sizeof(WebGpuPathVertex);
    vertexBufferLayout.attributeCount = 4 + kRoundedClipEntryCount;
    vertexBufferLayout.attributes = attributes;

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    applyBlendModeToAttachment(blend, blendMode);

    WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
    colorTarget.format = surfaceFormat_;
    colorTarget.blend = &blend;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader;
    fragment.entryPoint = stringView("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    WGPURenderPipelineDescriptor pipelineDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipelineDescriptor.label = stringView("LambdaUI WebGPU Path Pipeline");
    pipelineDescriptor.layout = pathPipelineLayout_;
    pipelineDescriptor.vertex.module = shader;
    pipelineDescriptor.vertex.entryPoint = stringView("vs_main");
    pipelineDescriptor.vertex.bufferCount = 1;
    pipelineDescriptor.vertex.buffers = &vertexBufferLayout;
    pipelineDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDescriptor.fragment = &fragment;

    pathPipelines_[pipelineIndex] = wgpuDeviceCreateRenderPipeline(context_.device(), &pipelineDescriptor);
    wgpuShaderModuleRelease(shader);
    if (!pathPipelines_[pipelineIndex]) {
      throw std::runtime_error("Lambda WebGPU: failed to create path pipeline");
    }
    pathPipelineFormat_ = surfaceFormat_;
    return pathPipelines_[pipelineIndex];
  }

  void releaseRectPipeline() noexcept {
    if (rectBindGroup_) {
      wgpuBindGroupRelease(rectBindGroup_);
      rectBindGroup_ = nullptr;
    }
    for (WGPURenderPipeline& pipeline : rectPipelines_) {
      if (pipeline) {
        wgpuRenderPipelineRelease(pipeline);
        pipeline = nullptr;
      }
    }
    if (rectPipelineLayout_) {
      wgpuPipelineLayoutRelease(rectPipelineLayout_);
      rectPipelineLayout_ = nullptr;
    }
    if (rectBindGroupLayout_) {
      wgpuBindGroupLayoutRelease(rectBindGroupLayout_);
      rectBindGroupLayout_ = nullptr;
    }
    pipelineFormat_ = WGPUTextureFormat_Undefined;
  }

  void releaseImagePipeline() noexcept {
    if (imageFrameBindGroup_) {
      wgpuBindGroupRelease(imageFrameBindGroup_);
      imageFrameBindGroup_ = nullptr;
    }
    for (WGPURenderPipeline& pipeline : imagePipelines_) {
      if (pipeline) {
        wgpuRenderPipelineRelease(pipeline);
        pipeline = nullptr;
      }
    }
    if (imagePipelineLayout_) {
      wgpuPipelineLayoutRelease(imagePipelineLayout_);
      imagePipelineLayout_ = nullptr;
    }
    if (imageFrameBindGroupLayout_) {
      wgpuBindGroupLayoutRelease(imageFrameBindGroupLayout_);
      imageFrameBindGroupLayout_ = nullptr;
    }
    if (imageTextureBindGroupLayout_) {
      wgpuBindGroupLayoutRelease(imageTextureBindGroupLayout_);
      imageTextureBindGroupLayout_ = nullptr;
    }
    imagePipelineFormat_ = WGPUTextureFormat_Undefined;
  }

  void releasePathPipeline() noexcept {
    for (WGPURenderPipeline& pipeline : pathPipelines_) {
      if (pipeline) {
        wgpuRenderPipelineRelease(pipeline);
        pipeline = nullptr;
      }
    }
    if (pathPipelineLayout_) {
      wgpuPipelineLayoutRelease(pathPipelineLayout_);
      pathPipelineLayout_ = nullptr;
    }
    pathPipelineFormat_ = WGPUTextureFormat_Undefined;
  }

  void releaseBlurPipeline() noexcept {
    if (blurPipeline_) {
      wgpuRenderPipelineRelease(blurPipeline_);
      blurPipeline_ = nullptr;
    }
    if (blurPipelineLayout_) {
      wgpuPipelineLayoutRelease(blurPipelineLayout_);
      blurPipelineLayout_ = nullptr;
    }
    if (blurBindGroupLayout_) {
      wgpuBindGroupLayoutRelease(blurBindGroupLayout_);
      blurBindGroupLayout_ = nullptr;
    }
    blurPipelineFormat_ = WGPUTextureFormat_Undefined;
  }

  void releaseDrawResources() noexcept {
    releaseRectPipeline();
    releaseImagePipeline();
    releasePathPipeline();
    releaseBlurPipeline();
    if (rectBuffer_) {
      wgpuBufferRelease(rectBuffer_);
      rectBuffer_ = nullptr;
    }
    if (quadBuffer_) {
      wgpuBufferRelease(quadBuffer_);
      quadBuffer_ = nullptr;
    }
    if (frameUniformBuffer_) {
      wgpuBufferRelease(frameUniformBuffer_);
      frameUniformBuffer_ = nullptr;
    }
    if (imageFrameUniformBuffer_) {
      wgpuBufferRelease(imageFrameUniformBuffer_);
      imageFrameUniformBuffer_ = nullptr;
    }
    if (pathBuffer_) {
      wgpuBufferRelease(pathBuffer_);
      pathBuffer_ = nullptr;
    }
    if (imageSampler_) {
      wgpuSamplerRelease(imageSampler_);
      imageSampler_ = nullptr;
    }
    if (blurSampler_) {
      wgpuSamplerRelease(blurSampler_);
      blurSampler_ = nullptr;
    }
    if (blurUniformBuffer_) {
      wgpuBufferRelease(blurUniformBuffer_);
      blurUniformBuffer_ = nullptr;
    }
    rectBufferCapacity_ = 0;
    quadBufferCapacity_ = 0;
    frameUniformBufferCapacity_ = 0;
    imageFrameUniformBufferCapacity_ = 0;
    pathBufferCapacity_ = 0;
    blurUniformBufferCapacity_ = 0;
  }

  WebGpuNativeSurface nativeSurface_{};
  unsigned int handle_ = 0;
  TextSystem& textSystem_;
  Size size_{};
  bool transparentSurface_ = false;
  WebGpuContext context_;
  WGPUSurface surface_ = nullptr;
  WGPUTextureFormat surfaceFormat_ = WGPUTextureFormat_Undefined;
  bool surfaceCopySrcSupported_ = false;
  WGPUTextureView externalRenderTargetView_ = nullptr;
  WGPUTexture offscreenTexture_ = nullptr;
  std::uint32_t offscreenPixelWidth_ = 0;
  std::uint32_t offscreenPixelHeight_ = 0;

  WGPUTexture backdropSourceTexture_ = nullptr;
  WGPUTextureView backdropSourceView_ = nullptr;
  WGPUTexture backdropScratchTexture_ = nullptr;
  WGPUTextureView backdropScratchView_ = nullptr;
  WGPUTexture backdropBlurTexture_ = nullptr;
  WGPUTextureView backdropBlurView_ = nullptr;
  std::uint32_t backdropPixelWidth_ = 0;
  std::uint32_t backdropPixelHeight_ = 0;

  WGPUTexture currentTexture_ = nullptr;
  WGPUTextureView currentView_ = nullptr;
  WGPUCommandEncoder commandEncoder_ = nullptr;
  bool frameActive_ = false;

  float dpiScale_ = 1.f;
  Mat3 transform_{Mat3::identity()};
  float opacity_ = 1.f;
  BlendMode blendMode_ = BlendMode::Normal;
  Rect clip_{};
  std::array<WebGpuRoundedClipState, kRoundedClipMaskCapacity> clipMasks_{};
  std::uint32_t clipMaskCount_ = 0;
  Color clearColor_ = Colors::transparent;
  std::vector<State> stateStack_;
  std::vector<WebGpuRectInstance> rects_;
  std::vector<WebGpuQuadInstance> quads_;
  std::vector<WebGpuPathVertex> pathVertices_;
  std::vector<WebGpuDrawOp> drawOps_;
  std::unordered_map<GlyphKey, GlyphImage, GlyphKeyHash> glyphs_;
  WebGpuFrameRecorder* captureTarget_ = nullptr;
  State captureSavedState_{};
  std::vector<State> captureSavedStack_;
  std::uint32_t captureRectStart_ = 0;
  std::uint32_t captureQuadStart_ = 0;
  std::uint32_t capturePathVertexStart_ = 0;
  std::uint32_t captureDrawOpStart_ = 0;

  WGPUBindGroupLayout rectBindGroupLayout_ = nullptr;
  WGPUPipelineLayout rectPipelineLayout_ = nullptr;
  std::array<WGPURenderPipeline, kWebGpuBlendModePipelineCount> rectPipelines_{};
  WGPUTextureFormat pipelineFormat_ = WGPUTextureFormat_Undefined;
  WGPUBuffer rectBuffer_ = nullptr;
  std::uint64_t rectBufferCapacity_ = 0;
  WGPUBuffer frameUniformBuffer_ = nullptr;
  std::uint64_t frameUniformBufferCapacity_ = 0;
  WGPUBindGroup rectBindGroup_ = nullptr;

  WGPUBindGroupLayout imageFrameBindGroupLayout_ = nullptr;
  WGPUBindGroupLayout imageTextureBindGroupLayout_ = nullptr;
  WGPUPipelineLayout imagePipelineLayout_ = nullptr;
  std::array<WGPURenderPipeline, kWebGpuBlendModePipelineCount> imagePipelines_{};
  WGPUTextureFormat imagePipelineFormat_ = WGPUTextureFormat_Undefined;
  WGPUBuffer quadBuffer_ = nullptr;
  std::uint64_t quadBufferCapacity_ = 0;
  WGPUBuffer imageFrameUniformBuffer_ = nullptr;
  std::uint64_t imageFrameUniformBufferCapacity_ = 0;
  WGPUBindGroup imageFrameBindGroup_ = nullptr;
  WGPUSampler imageSampler_ = nullptr;

  WGPUPipelineLayout pathPipelineLayout_ = nullptr;
  std::array<WGPURenderPipeline, kWebGpuBlendModePipelineCount> pathPipelines_{};
  WGPUTextureFormat pathPipelineFormat_ = WGPUTextureFormat_Undefined;
  WGPUBuffer pathBuffer_ = nullptr;
  std::uint64_t pathBufferCapacity_ = 0;

  WGPUBindGroupLayout blurBindGroupLayout_ = nullptr;
  WGPUPipelineLayout blurPipelineLayout_ = nullptr;
  WGPURenderPipeline blurPipeline_ = nullptr;
  WGPUTextureFormat blurPipelineFormat_ = WGPUTextureFormat_Undefined;
  WGPUSampler blurSampler_ = nullptr;
  WGPUBuffer blurUniformBuffer_ = nullptr;
  std::uint64_t blurUniformBufferCapacity_ = 0;

  bool frameCaptureRequested_ = false;
  bool capturedFrameAvailable_ = false;
  std::vector<std::uint8_t> capturedFrameBytes_;
  std::uint32_t capturedFrameWidth_ = 0;
  std::uint32_t capturedFrameHeight_ = 0;
  WGPUBuffer pendingFrameCaptureBuffer_ = nullptr;
  std::uint32_t pendingFrameCaptureWidth_ = 0;
  std::uint32_t pendingFrameCaptureHeight_ = 0;
  std::uint32_t pendingFrameCaptureBytesPerRow_ = 0;
  std::uint64_t pendingFrameCaptureBufferSize_ = 0;
  WGPUTextureFormat pendingFrameCaptureFormat_ = WGPUTextureFormat_Undefined;
};

} // namespace

std::unique_ptr<Canvas> createWebGpuCanvas(WebGpuNativeSurface nativeSurface,
                                           unsigned int handle,
                                           TextSystem& textSystem,
                                           Size initialSize,
                                           bool transparentSurface) {
  return std::make_unique<WebGpuCanvas>(nativeSurface, handle, textSystem, initialSize, transparentSurface);
}

std::unique_ptr<Canvas> createWebGpuRenderTargetCanvas(TextSystem& textSystem,
                                                       Size logicalSize,
                                                       std::uint32_t pixelWidth,
                                                       std::uint32_t pixelHeight) {
  return std::make_unique<WebGpuCanvas>(textSystem, logicalSize, pixelWidth, pixelHeight);
}

std::unique_ptr<Canvas> createWebGpuExternalRenderTargetCanvas(TextSystem& textSystem,
                                                               Size logicalSize,
                                                               std::uint32_t pixelWidth,
                                                               std::uint32_t pixelHeight,
                                                               WGPUDevice device,
                                                               WGPUQueue queue,
                                                               WGPUTextureView textureView,
                                                               WGPUTextureFormat format) {
  return std::make_unique<WebGpuCanvas>(textSystem,
                                        logicalSize,
                                        pixelWidth,
                                        pixelHeight,
                                        device,
                                        queue,
                                        textureView,
                                        format);
}

WebGpuCanvasHandles canvasHandles(Canvas const& canvas) noexcept {
  auto const* webgpuCanvas = dynamic_cast<WebGpuCanvas const*>(&canvas);
  if (!webgpuCanvas) {
    return {};
  }
  return WebGpuCanvasHandles{
      .device = webgpuCanvas->deviceHandle(),
      .queue = webgpuCanvas->queueHandle(),
      .renderTargetFormat = webgpuCanvas->renderTargetFormat(),
  };
}

void unpremultiplyRgbaPixels(std::vector<std::uint8_t>& pixels) {
  for (std::size_t i = 0; i + 3u < pixels.size(); i += 4u) {
    std::uint8_t const alpha = pixels[i + 3u];
    if (alpha == 0) {
      pixels[i + 0u] = 0;
      pixels[i + 1u] = 0;
      pixels[i + 2u] = 0;
      continue;
    }
    if (alpha == 255) {
      continue;
    }
    pixels[i + 0u] = static_cast<std::uint8_t>(std::min<unsigned>(255u, pixels[i + 0u] * 255u / alpha));
    pixels[i + 1u] = static_cast<std::uint8_t>(std::min<unsigned>(255u, pixels[i + 1u] * 255u / alpha));
    pixels[i + 2u] = static_cast<std::uint8_t>(std::min<unsigned>(255u, pixels[i + 2u] * 255u / alpha));
  }
}

std::shared_ptr<Image> rasterizeToWebGpuImage(Canvas& canvas,
                                              Size logicalSize,
                                              RasterizeDrawCallback const& draw,
                                              float dpiScale) {
  auto* webgpuCanvas = dynamic_cast<WebGpuCanvas*>(&canvas);
  if (!webgpuCanvas || !draw || logicalSize.width <= 0.f || logicalSize.height <= 0.f) {
    return nullptr;
  }

  float const scale = dpiScale > 0.f ? dpiScale : std::max(webgpuCanvas->dpiScale(), 1.f);
  std::uint32_t const pixelWidth = std::max(1u, static_cast<std::uint32_t>(std::ceil(logicalSize.width * scale)));
  std::uint32_t const pixelHeight = std::max(1u, static_cast<std::uint32_t>(std::ceil(logicalSize.height * scale)));

  WebGpuCanvas target(webgpuCanvas->textSystem(), logicalSize, pixelWidth, pixelHeight);
  target.updateDpiScale(scale, scale);
  target.beginFrame();
  target.clear(Colors::transparent);
  draw(target, Rect::sharp(0.f, 0.f, logicalSize.width, logicalSize.height));
  if (!target.requestNextFrameCapture()) {
    return nullptr;
  }
  target.present();

  std::vector<std::uint8_t> pixels;
  std::uint32_t capturedWidth = 0;
  std::uint32_t capturedHeight = 0;
  if (!target.takeCapturedFrame(pixels, capturedWidth, capturedHeight) ||
      capturedWidth != pixelWidth ||
      capturedHeight != pixelHeight) {
    return nullptr;
  }

  unpremultiplyRgbaPixels(pixels);
  return std::make_shared<WebGpuImage>(capturedWidth,
                                       capturedHeight,
                                       std::move(pixels),
                                       Image::PixelFormat::Rgba8888,
                                       false);
}

} // namespace lambdaui::webgpu

#if LAMBDAUI_WEBGPU
namespace lambdaui {

WebGpuCanvasHandles webGpuCanvasHandles(Canvas const& canvas) noexcept {
  return webgpu::canvasHandles(canvas);
}

WGPUDevice webGpuDevice(Canvas const& canvas) noexcept {
  return webGpuCanvasHandles(canvas).device;
}

WGPUQueue webGpuQueue(Canvas const& canvas) noexcept {
  return webGpuCanvasHandles(canvas).queue;
}

WGPUTextureFormat webGpuRenderTargetFormat(Canvas const& canvas) noexcept {
  return webGpuCanvasHandles(canvas).renderTargetFormat;
}

std::shared_ptr<Image> Image::fromRgbaPixels(std::uint32_t width,
                                             std::uint32_t height,
                                             std::span<std::uint8_t const> rgbaPixels,
                                             void* gpuDevice) {
  if (width == 0 || height == 0 || rgbaPixels.size() != static_cast<std::size_t>(width) * height * 4u) {
    return nullptr;
  }
  return fromPixels(width, height, rgbaPixels, PixelFormat::Rgba8888, gpuDevice);
}

std::shared_ptr<Image> Image::fromPixels(std::uint32_t width,
                                         std::uint32_t height,
                                         std::span<std::uint8_t const> pixels,
                                         PixelFormat format,
                                         void* gpuDevice) {
  if (width == 0 || height == 0 || pixels.size() != static_cast<std::size_t>(width) * height * 4u) {
    return nullptr;
  }
  std::vector<std::uint8_t> copy(pixels.begin(), pixels.end());
  auto image = std::make_shared<webgpu::WebGpuImage>(width, height, std::move(copy), format, false);
  if (gpuDevice) {
    try {
      WGPUQueue queue = wgpuDeviceGetQueue(static_cast<WGPUDevice>(gpuDevice));
      if (!queue) {
        return nullptr;
      }
      (void)image->textureView(static_cast<WGPUDevice>(gpuDevice), queue);
      wgpuQueueRelease(queue);
    } catch (...) {
      return nullptr;
    }
  }
  return image;
}

std::shared_ptr<Image> Image::fromExternalWebGpu(WGPUTextureView textureView,
                                                 std::uint32_t width,
                                                 std::uint32_t height,
                                                 bool premultipliedAlpha) {
  if (!textureView || width == 0 || height == 0) {
    return nullptr;
  }
  return std::make_shared<webgpu::WebGpuImage>(width, height, textureView, premultipliedAlpha);
}

std::shared_ptr<Image> rasterizeToImage(Canvas& canvas,
                                        Size logicalSize,
                                        RasterizeDrawCallback draw,
                                        float dpiScale) {
  return webgpu::rasterizeToWebGpuImage(canvas, logicalSize, draw, dpiScale);
}

} // namespace lambdaui
#endif
