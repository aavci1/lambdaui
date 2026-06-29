#include "WebGpuCanvas.hpp"

#include "WebGpuContext.hpp"
#include "Graphics/CanvasGeometry.hpp"

#include <Lambda/Graphics/Image.hpp>
#include <Lambda/Graphics/TextSystem.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
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

  ~WebGpuImage() override {
    releaseGpuObjects();
  }

  Size size() const override { return size_; }
  bool premultipliedAlpha() const noexcept override { return premultiplied_; }

  bool updateRgbaPixels(std::span<std::uint8_t const> rgbaPixels, void* gpuDevice) override {
    return updatePixels(rgbaPixels, PixelFormat::Rgba8888, gpuDevice);
  }

  bool updatePixels(std::span<std::uint8_t const> pixels, PixelFormat format, void* gpuDevice) override {
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

  mutable WGPUDevice device_ = nullptr;
  mutable WGPUTexture texture_ = nullptr;
  mutable WGPUTextureView textureView_ = nullptr;
  mutable bool textureDirty_ = true;
};

namespace {

inline constexpr std::size_t kRoundedClipMaskCapacity = 4;
inline constexpr std::size_t kRoundedClipEntryCount = kRoundedClipMaskCapacity * 2;

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
    if (max(max(clipRadii.x, clipRadii.y), max(clipRadii.z, clipRadii.w)) <= 0.0) {
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
    if (max(max(clipRadii.x, clipRadii.y), max(clipRadii.z, clipRadii.w)) <= 0.0) {
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
public:
  WebGpuCanvas(WebGpuNativeSurface nativeSurface,
               unsigned int handle,
               Size initialSize,
               bool transparentSurface)
      : nativeSurface_(nativeSurface),
        handle_(handle),
        size_(initialSize),
        transparentSurface_(transparentSurface),
        context_(),
        surface_(createSurface(context_.instance(), nativeSurface_)) {
    context_.initializeDevice(surface_);
    configureSurface();
  }

  ~WebGpuCanvas() override {
    releaseFrameObjects();
    releaseDrawResources();
    if (surface_) {
      wgpuSurfaceUnconfigure(surface_);
      wgpuSurfaceRelease(surface_);
    }
  }

  Backend backend() const noexcept override { return Backend::WebGPU; }
  unsigned int windowHandle() const override { return handle_; }

  void resize(int width, int height) override {
    width = std::max(1, width);
    height = std::max(1, height);
    if (size_.width == static_cast<float>(width) && size_.height == static_cast<float>(height)) {
      return;
    }
    size_ = {static_cast<float>(width), static_cast<float>(height)};
    configureSurface();
  }

  void updateDpiScale(float scaleX, float scaleY) override {
    dpiScale_ = std::max(scaleX, scaleY);
  }

  float dpiScale() const noexcept override { return dpiScale_; }

  void beginFrame() override {
    releaseFrameObjects();
    rects_.clear();
    quads_.clear();
    drawOps_.clear();
    frameActive_ = true;
  }

  void present() override {
    if (!frameActive_) {
      return;
    }
    encodeFramePass();
    if (commandEncoder_) {
      WGPUCommandBufferDescriptor finishDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
      finishDescriptor.label = stringView("LambdaUI WebGPU Command Buffer");
      WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(commandEncoder_, &finishDescriptor);
      if (commandBuffer) {
        wgpuQueueSubmit(context_.queue(), 1, &commandBuffer);
        wgpuCommandBufferRelease(commandBuffer);
      }
    }
    wgpuSurfacePresent(surface_);
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

  void clipRect(Rect, CornerRadius const&, bool) override {}
  Rect clipBounds() const override { return Rect{0.f, 0.f, size_.width, size_.height}; }
  bool quickReject(Rect) const override { return false; }

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
    if (!frameActive_ || stroke.isNone()) {
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
    std::uint32_t const first = static_cast<std::uint32_t>(rects_.size());
    rects_.push_back(instance);
    drawOps_.push_back(WebGpuDrawOp{
        .kind = WebGpuDrawOp::Kind::Rect,
        .first = first,
        .count = 1,
    });
  }

  void drawPath(Path const& path,
                FillStyle const& fill,
                StrokeStyle const& stroke,
                ShadowStyle const& shadow) override {
    if (!frameActive_ || path.isEmpty() || path.commandCount() != 1) {
      return;
    }
    Path::CommandView const command = path.command(0);
    if (command.type == Path::CommandType::Rect && command.dataCount >= 8) {
      Rect const rect{command.data[0], command.data[1], command.data[2], command.data[3]};
      CornerRadius const radii{command.data[4], command.data[5], command.data[6], command.data[7]};
      drawRect(rect, radii, fill, stroke, shadow);
      return;
    }
    if (command.type == Path::CommandType::Circle && command.dataCount >= 3) {
      drawCircle(Point{command.data[0], command.data[1]}, command.data[2], fill, stroke);
      return;
    }
    if (command.type == Path::CommandType::Ellipse && command.dataCount >= 4) {
      Rect const rect{command.data[0] - command.data[2],
                      command.data[1] - command.data[3],
                      command.data[2] * 2.f,
                      command.data[3] * 2.f};
      drawRect(rect, CornerRadius::pill(rect), fill, stroke, shadow);
    }
  }

  void drawCircle(Point center, float radius, FillStyle const& fill, StrokeStyle const& stroke) override {
    Rect const rect{center.x - radius, center.y - radius, radius * 2.f, radius * 2.f};
    drawRect(rect, CornerRadius::pill(rect), fill, stroke, ShadowStyle::none());
  }

  void drawTextLayout(TextLayout const&, Point) override {}
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
    Size const imageSize = image.size();
    if (imageSize.width <= 0.f || imageSize.height <= 0.f) {
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
    instance.color[0] = 1.f;
    instance.color[1] = 1.f;
    instance.color[2] = 1.f;
    instance.color[3] = opacity_ * opacity;
    CornerRadius const radii = clampedRadii(corners, dst.width, dst.height);
    instance.radii[0] = radii.topLeft;
    instance.radii[1] = radii.topRight;
    instance.radii[2] = radii.bottomRight;
    instance.radii[3] = radii.bottomLeft;
    std::uint32_t const first = static_cast<std::uint32_t>(quads_.size());
    quads_.push_back(instance);
    WebGpuDrawOp op{
        .kind = WebGpuDrawOp::Kind::Image,
        .first = first,
        .count = 1,
        .image = webgpuImage,
    };
    try {
      op.imageRef = image.shared_from_this();
    } catch (std::bad_weak_ptr const&) {
      op.imageRef.reset();
    }
    drawOps_.push_back(std::move(op));
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
  void drawBackdropBlur(Rect const&, float, Color, CornerRadius const&) override {}

  void* gpuDevice() const override { return context_.device(); }

  bool requestNextFrameCapture() override { return false; }
  bool takeCapturedFrame(std::vector<std::uint8_t>&, std::uint32_t&, std::uint32_t&) override { return false; }
  std::unique_ptr<RecordedOps> beginRecordedOpsCapture() override { return nullptr; }
  void endRecordedOpsCapture() override {}
  std::unique_ptr<scenegraph::PreparedRenderOps> finalizeRecordedOps(std::unique_ptr<RecordedOps>) override {
    return nullptr;
  }
  bool replayRecordedOps(RecordedOps const&, RecordedOpsReplaySlice const*) override { return false; }
  bool replayRecordedLocalOps(RecordedOps const&, RecordedOpsReplaySlice const*) override { return false; }

  void clear(Color color) override {
    clearColor_ = color;
  }

private:
  struct State {
    Mat3 transform = Mat3::identity();
    float opacity = 1.f;
    BlendMode blendMode = BlendMode::Normal;
  };

  struct WebGpuDrawOp {
    enum class Kind {
      Rect,
      Image,
    };

    Kind kind = Kind::Rect;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    WebGpuImage const* image = nullptr;
    std::shared_ptr<Image const> imageRef;
  };

  State state() const noexcept {
    return State{
        .transform = transform_,
        .opacity = opacity_,
        .blendMode = blendMode_,
    };
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

    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = context_.device();
    config.format = surfaceFormat_;
    config.usage = WGPUTextureUsage_RenderAttachment;
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
  }

  void ensureFrameObjects() {
    if (currentTexture_) {
      return;
    }

    WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture);
    if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
      throw std::runtime_error("Lambda WebGPU: failed to acquire surface texture");
    }
    currentTexture_ = surfaceTexture.texture;

    WGPUTextureViewDescriptor viewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    viewDescriptor.label = stringView("LambdaUI WebGPU Surface View");
    currentView_ = wgpuTextureCreateView(currentTexture_, &viewDescriptor);
    if (!currentView_) {
      throw std::runtime_error("Lambda WebGPU: failed to create surface texture view");
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("LambdaUI WebGPU Command Encoder");
    commandEncoder_ = wgpuDeviceCreateCommandEncoder(context_.device(), &encoderDescriptor);
    if (!commandEncoder_) {
      throw std::runtime_error("Lambda WebGPU: failed to create command encoder");
    }
  }

  void encodeFramePass() {
    ensureFrameObjects();
    uploadRectResources();
    uploadImageResources();

    WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    colorAttachment.view = currentView_;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = toWebGpuColor(clearColor_);

    WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("LambdaUI WebGPU Clear Pass");
    passDescriptor.colorAttachmentCount = 1;
    passDescriptor.colorAttachments = &colorAttachment;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(commandEncoder_, &passDescriptor);
    if (pass) {
      WGPURenderPipeline activePipeline = nullptr;
      for (WebGpuDrawOp const& op : drawOps_) {
        switch (op.kind) {
          case WebGpuDrawOp::Kind::Rect:
            if (!rectBindGroup_) {
              break;
            }
            ensureRectPipeline();
            if (activePipeline != rectPipeline_) {
              wgpuRenderPassEncoderSetPipeline(pass, rectPipeline_);
              wgpuRenderPassEncoderSetBindGroup(pass, 0, rectBindGroup_, 0, nullptr);
              activePipeline = rectPipeline_;
            }
            wgpuRenderPassEncoderDraw(pass, 6, op.count, 0, op.first);
            break;
          case WebGpuDrawOp::Kind::Image:
            if (!imageFrameBindGroup_ || !op.image) {
              break;
            }
            ensureImagePipeline();
            if (activePipeline != imagePipeline_) {
              wgpuRenderPassEncoderSetPipeline(pass, imagePipeline_);
              wgpuRenderPassEncoderSetBindGroup(pass, 0, imageFrameBindGroup_, 0, nullptr);
              activePipeline = imagePipeline_;
            }
            try {
              WGPUTextureView const view = op.image->textureView(context_.device(), context_.queue());
              if (!view) {
                break;
              }
              WGPUBindGroup textureBindGroup = createImageTextureBindGroup(view);
              wgpuRenderPassEncoderSetBindGroup(pass, 1, textureBindGroup, 0, nullptr);
              wgpuRenderPassEncoderDraw(pass, 6, op.count, 0, op.first);
              wgpuBindGroupRelease(textureBindGroup);
            } catch (std::exception const& e) {
              std::fprintf(stderr, "Lambda WebGPU: image upload failed: %s\n", e.what());
            }
            break;
        }
      }
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
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

  void pushRectInstance(Rect const& rect,
                        CornerRadius const& cornerRadius,
                        FillStyle const& fill,
                        StrokeStyle const& stroke,
                        float opacity) {
    if (rect.width <= 0.f || rect.height <= 0.f) {
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
    std::uint32_t const first = static_cast<std::uint32_t>(rects_.size());
    rects_.push_back(instance);
    drawOps_.push_back(WebGpuDrawOp{
        .kind = WebGpuDrawOp::Kind::Rect,
        .first = first,
        .count = 1,
    });
  }

  void ensureBuffer(WGPUBuffer& buffer,
                    std::uint64_t& capacity,
                    std::uint64_t requiredSize,
                    WGPUBufferUsage usage,
                    char const* label) {
    requiredSize = std::max<std::uint64_t>(requiredSize, 16);
    if (buffer && capacity >= requiredSize) {
      return;
    }
    if (rectBindGroup_) {
      wgpuBindGroupRelease(rectBindGroup_);
      rectBindGroup_ = nullptr;
    }
    if (imageFrameBindGroup_) {
      wgpuBindGroupRelease(imageFrameBindGroup_);
      imageFrameBindGroup_ = nullptr;
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
  }

  void uploadRectResources() {
    if (rects_.empty()) {
      return;
    }
    ensureBuffer(rectBuffer_,
                 rectBufferCapacity_,
                 static_cast<std::uint64_t>(rects_.size() * sizeof(WebGpuRectInstance)),
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                 "LambdaUI WebGPU Rect Instances");
    ensureBuffer(frameUniformBuffer_,
                 frameUniformBufferCapacity_,
                 sizeof(WebGpuFrameUniforms),
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                 "LambdaUI WebGPU Frame Uniforms");
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
    ensureBuffer(quadBuffer_,
                 quadBufferCapacity_,
                 static_cast<std::uint64_t>(quads_.size() * sizeof(WebGpuQuadInstance)),
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                 "LambdaUI WebGPU Image Quads");
    ensureBuffer(imageFrameUniformBuffer_,
                 imageFrameUniformBufferCapacity_,
                 sizeof(WebGpuFrameUniforms),
                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                 "LambdaUI WebGPU Image Frame Uniforms");
    WebGpuFrameUniforms uniforms{};
    uniforms.viewport[0] = std::max(1.f, size_.width);
    uniforms.viewport[1] = std::max(1.f, size_.height);
    wgpuQueueWriteBuffer(context_.queue(), quadBuffer_, 0, quads_.data(), quads_.size() * sizeof(WebGpuQuadInstance));
    wgpuQueueWriteBuffer(context_.queue(), imageFrameUniformBuffer_, 0, &uniforms, sizeof(uniforms));
    ensureImageFrameBindGroup();
  }

  void ensureRectBindGroup() {
    if (rectBindGroup_) {
      return;
    }
    ensureRectPipeline();
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

  void ensureRectPipeline() {
    if (rectPipeline_ && pipelineFormat_ == surfaceFormat_) {
      return;
    }
    releaseRectPipeline();

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

    WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pipelineLayoutDescriptor.label = stringView("LambdaUI WebGPU Rect Pipeline Layout");
    pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
    pipelineLayoutDescriptor.bindGroupLayouts = &rectBindGroupLayout_;
    rectPipelineLayout_ = wgpuDeviceCreatePipelineLayout(context_.device(), &pipelineLayoutDescriptor);
    if (!rectPipelineLayout_) {
      throw std::runtime_error("Lambda WebGPU: failed to create rect pipeline layout");
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
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

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

    rectPipeline_ = wgpuDeviceCreateRenderPipeline(context_.device(), &pipelineDescriptor);
    wgpuShaderModuleRelease(shader);
    if (!rectPipeline_) {
      throw std::runtime_error("Lambda WebGPU: failed to create rect pipeline");
    }
    pipelineFormat_ = surfaceFormat_;
  }

  void ensureImageFrameBindGroup() {
    if (imageFrameBindGroup_) {
      return;
    }
    ensureImagePipeline();
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
    ensureImagePipeline();
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

  void ensureImagePipeline() {
    if (imagePipeline_ && imagePipelineFormat_ == surfaceFormat_) {
      return;
    }
    releaseImagePipeline();

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
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

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

    imagePipeline_ = wgpuDeviceCreateRenderPipeline(context_.device(), &pipelineDescriptor);
    wgpuShaderModuleRelease(shader);
    if (!imagePipeline_) {
      throw std::runtime_error("Lambda WebGPU: failed to create image pipeline");
    }
    imagePipelineFormat_ = surfaceFormat_;
    ensureImageSampler();
  }

  void releaseRectPipeline() noexcept {
    if (rectBindGroup_) {
      wgpuBindGroupRelease(rectBindGroup_);
      rectBindGroup_ = nullptr;
    }
    if (rectPipeline_) {
      wgpuRenderPipelineRelease(rectPipeline_);
      rectPipeline_ = nullptr;
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
    if (imagePipeline_) {
      wgpuRenderPipelineRelease(imagePipeline_);
      imagePipeline_ = nullptr;
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

  void releaseDrawResources() noexcept {
    releaseRectPipeline();
    releaseImagePipeline();
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
    if (imageSampler_) {
      wgpuSamplerRelease(imageSampler_);
      imageSampler_ = nullptr;
    }
    rectBufferCapacity_ = 0;
    quadBufferCapacity_ = 0;
    frameUniformBufferCapacity_ = 0;
    imageFrameUniformBufferCapacity_ = 0;
  }

  WebGpuNativeSurface nativeSurface_{};
  unsigned int handle_ = 0;
  Size size_{};
  bool transparentSurface_ = false;
  WebGpuContext context_;
  WGPUSurface surface_ = nullptr;
  WGPUTextureFormat surfaceFormat_ = WGPUTextureFormat_Undefined;

  WGPUTexture currentTexture_ = nullptr;
  WGPUTextureView currentView_ = nullptr;
  WGPUCommandEncoder commandEncoder_ = nullptr;
  bool frameActive_ = false;

  float dpiScale_ = 1.f;
  Mat3 transform_{Mat3::identity()};
  float opacity_ = 1.f;
  BlendMode blendMode_ = BlendMode::Normal;
  Color clearColor_ = Colors::transparent;
  std::vector<State> stateStack_;
  std::vector<WebGpuRectInstance> rects_;
  std::vector<WebGpuQuadInstance> quads_;
  std::vector<WebGpuDrawOp> drawOps_;

  WGPUBindGroupLayout rectBindGroupLayout_ = nullptr;
  WGPUPipelineLayout rectPipelineLayout_ = nullptr;
  WGPURenderPipeline rectPipeline_ = nullptr;
  WGPUTextureFormat pipelineFormat_ = WGPUTextureFormat_Undefined;
  WGPUBuffer rectBuffer_ = nullptr;
  std::uint64_t rectBufferCapacity_ = 0;
  WGPUBuffer frameUniformBuffer_ = nullptr;
  std::uint64_t frameUniformBufferCapacity_ = 0;
  WGPUBindGroup rectBindGroup_ = nullptr;

  WGPUBindGroupLayout imageFrameBindGroupLayout_ = nullptr;
  WGPUBindGroupLayout imageTextureBindGroupLayout_ = nullptr;
  WGPUPipelineLayout imagePipelineLayout_ = nullptr;
  WGPURenderPipeline imagePipeline_ = nullptr;
  WGPUTextureFormat imagePipelineFormat_ = WGPUTextureFormat_Undefined;
  WGPUBuffer quadBuffer_ = nullptr;
  std::uint64_t quadBufferCapacity_ = 0;
  WGPUBuffer imageFrameUniformBuffer_ = nullptr;
  std::uint64_t imageFrameUniformBufferCapacity_ = 0;
  WGPUBindGroup imageFrameBindGroup_ = nullptr;
  WGPUSampler imageSampler_ = nullptr;
};

} // namespace

std::unique_ptr<Canvas> createWebGpuCanvas(WebGpuNativeSurface nativeSurface,
                                           unsigned int handle,
                                           TextSystem&,
                                           Size initialSize,
                                           bool transparentSurface) {
  return std::make_unique<WebGpuCanvas>(nativeSurface, handle, initialSize, transparentSurface);
}

} // namespace lambdaui::webgpu

#if LAMBDAUI_WEBGPU
namespace lambdaui {

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

} // namespace lambdaui
#endif
