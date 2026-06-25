#include <doctest/doctest.h>

#include "Graphics/CoreTextSystem.hpp"
#include "Graphics/Metal/MetalCanvas.hpp"

#include <Lambda/Graphics/Canvas.hpp>
#include <Lambda/Graphics/RenderTarget.hpp>
#include <Lambda/SceneGraph/RectNode.hpp>
#include <Lambda/SceneGraph/SceneGraph.hpp>
#include <Lambda/SceneGraph/SceneNode.hpp>
#include <Lambda/SceneGraph/SceneRenderer.hpp>

#import <Metal/Metal.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace lambdaui;
using namespace lambdaui::scenegraph;

static std::uint8_t capturedChannel(std::vector<std::uint8_t> const& pixels, std::uint32_t width,
                                    std::uint32_t x, std::uint32_t y, int channel) {
  std::size_t const idx =
      (static_cast<std::size_t>(y) * width + x) * 4u + static_cast<std::size_t>(channel);
  return pixels[idx];
}

struct HeadlessOpacityTarget {
  id<MTLDevice> device = nil;
  id<MTLTexture> texture = nil;
  std::unique_ptr<Canvas> targetCanvas;
  int logicalW = 1;
  int logicalH = 1;
  float dpiScale = 1.f;
  std::uint32_t pixelW = 1;
  std::uint32_t pixelH = 1;

  HeadlessOpacityTarget(TextSystem& textSystem, int width, int height, float scale = 1.f)
      : device(MTLCreateSystemDefaultDevice()), logicalW(width), logicalH(height), dpiScale(scale),
        pixelW(static_cast<std::uint32_t>(std::ceil(static_cast<float>(width) * scale))),
        pixelH(static_cast<std::uint32_t>(std::ceil(static_cast<float>(height) * scale))) {
    if (!device) {
      return;
    }
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:pixelW
                                                          height:pixelH
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    texture = [device newTextureWithDescriptor:desc];
    if (!texture) {
      return;
    }
    targetCanvas = createMetalRenderTargetCanvas(MetalRenderTargetSpec{
        .texture = (__bridge void*)texture,
        .width = pixelW,
        .height = pixelH,
    }, textSystem);
    canvas().resize(logicalW, logicalH);
    canvas().updateDpiScale(dpiScale, dpiScale);
  }

  explicit operator bool() const noexcept { return targetCanvas != nullptr; }

  Canvas& canvas() { return *targetCanvas; }

  void begin(Color clearColor = Colors::transparent) {
    canvas().beginFrame();
    canvas().clear(clearColor);
  }

  void end() { canvas().present(); }

  void render(SceneGraph const& graph, Color clearColor) {
    begin(clearColor);
    SceneRenderer renderer{canvas()};
    renderer.render(graph);
    end();
  }

  std::vector<std::uint8_t> readPixels() {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
      return {};
    }
    id<MTLBuffer> readback = [device newBufferWithLength:pixelW * pixelH * 4u
                                                 options:MTLResourceStorageModeShared];
    if (!readback) {
      return {};
    }
    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(pixelW, pixelH, 1)
                 toBuffer:readback
        destinationOffset:0
   destinationBytesPerRow:pixelW * 4u
 destinationBytesPerImage:pixelW * pixelH * 4u];
    [blit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
      return {};
    }
    auto const* bytes = static_cast<std::uint8_t const*>([readback contents]);
    return std::vector<std::uint8_t>(bytes, bytes + pixelW * pixelH * 4u);
  }
};

} // namespace

TEST_CASE("RenderOpacity flattens overlapping child opacity into one Metal layer") {
#if !defined(__APPLE__)
  SUCCEED();
#else
  @autoreleasepool {
    CoreTextSystem textSystem;
    HeadlessOpacityTarget target{textSystem, 96, 64, 1.f};
    REQUIRE(target);

    auto root = std::make_unique<SceneNode>(lambdaui::Rect{0.f, 0.f, 96.f, 64.f});
    auto parent = std::make_unique<RectNode>(
        lambdaui::Rect{0.f, 0.f, 96.f, 64.f}, FillStyle::none(), StrokeStyle::none());
    parent->setOpacity(0.5f);
    parent->setFlattenOpacity(true);
    parent->appendChild(std::make_unique<RectNode>(
        lambdaui::Rect{12.f, 12.f, 40.f, 32.f}, FillStyle::solid(Colors::red)));
    parent->appendChild(std::make_unique<RectNode>(
        lambdaui::Rect{32.f, 12.f, 40.f, 32.f}, FillStyle::solid(Colors::blue)));
    root->appendChild(std::move(parent));
    SceneGraph graph{std::move(root)};

    target.render(graph, Colors::black);
    std::vector<std::uint8_t> pixels = target.readPixels();
    REQUIRE(!pixels.empty());

    CHECK(capturedChannel(pixels, target.pixelW, 40, 24, 0) >= 110);
    CHECK(capturedChannel(pixels, target.pixelW, 40, 24, 2) <= 32);
  }
#endif
}

TEST_CASE("RenderOpacity composes nested flattened Metal opacity multiplicatively") {
#if !defined(__APPLE__)
  SUCCEED();
#else
  @autoreleasepool {
    CoreTextSystem textSystem;
    HeadlessOpacityTarget target{textSystem, 64, 64, 1.f};
    REQUIRE(target);

    auto root = std::make_unique<SceneNode>(lambdaui::Rect{0.f, 0.f, 64.f, 64.f});
    auto parent = std::make_unique<RectNode>(
        lambdaui::Rect{0.f, 0.f, 64.f, 64.f}, FillStyle::none(), StrokeStyle::none());
    parent->setOpacity(0.5f);
    parent->setFlattenOpacity(true);
    auto child = std::make_unique<RectNode>(
        lambdaui::Rect{16.f, 16.f, 32.f, 32.f}, FillStyle::solid(Colors::red));
    child->setOpacity(0.5f);
    child->setFlattenOpacity(true);
    parent->appendChild(std::move(child));
    root->appendChild(std::move(parent));
    SceneGraph graph{std::move(root)};

    target.render(graph, Colors::black);
    std::vector<std::uint8_t> pixels = target.readPixels();
    REQUIRE(!pixels.empty());

    std::uint8_t const red = capturedChannel(pixels, target.pixelW, 32, 32, 2);
    CHECK(red >= 52);
    CHECK(red <= 76);
  }
#endif
}

TEST_CASE("RenderOpacity reuses static flattened Metal opacity layer") {
#if !defined(__APPLE__)
  SUCCEED();
#else
  @autoreleasepool {
    CoreTextSystem textSystem;
    HeadlessOpacityTarget target{textSystem, 64, 64, 1.f};
    REQUIRE(target);
    Canvas& canvas = target.canvas();

    auto root = std::make_unique<SceneNode>(lambdaui::Rect{0.f, 0.f, 64.f, 64.f});
    auto parent = std::make_unique<RectNode>(
        lambdaui::Rect{0.f, 0.f, 64.f, 64.f}, FillStyle::none(), StrokeStyle::none());
    RectNode* parentNode = parent.get();
    parent->setOpacity(0.5f);
    parent->setFlattenOpacity(true);
    parent->appendChild(std::make_unique<RectNode>(
        lambdaui::Rect{16.f, 16.f, 32.f, 32.f}, FillStyle::solid(Colors::red)));
    root->appendChild(std::move(parent));
    SceneGraph graph{std::move(root)};
    SceneRenderer renderer{canvas};

    target.begin(Colors::black);
    renderer.render(graph);
    target.end();
    CHECK(parentNode->opacityLayerRasterizeCount() == 1);

    target.begin(Colors::black);
    renderer.render(graph);
    target.end();
    CHECK(parentNode->opacityLayerRasterizeCount() == 1);
  }
#endif
}
