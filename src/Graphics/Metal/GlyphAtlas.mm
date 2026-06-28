#import <Metal/Metal.h>

#include <Lambda/Debug/PerfCounters.hpp>

#include "Graphics/Metal/GlyphAtlas.hpp"

#include <cstring>
#include <stdexcept>

namespace lambdaui {

GlyphAtlas::GlyphAtlas(id<MTLDevice> device, TextSystem& textSystem, id<MTLCommandQueue> queue)
    : device_(device), queue_(queue ? queue : [device_ newCommandQueue]), textSystem_(textSystem) {
  texture_ = createTexture(allocator_.width(), allocator_.height());
  if (!texture_ || !queue_) {
    throw std::runtime_error("GlyphAtlas: failed to create atlas resources");
  }
  if (!clearTexture(texture_, allocator_.width(), allocator_.height())) {
    throw std::runtime_error("GlyphAtlas: failed to clear atlas texture");
  }
}

id<MTLTexture> GlyphAtlas::createTexture(std::uint32_t width, std::uint32_t height) const {
  MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
  d.usage = MTLTextureUsageShaderRead;
  d.storageMode = MTLStorageModePrivate;
  return [device_ newTextureWithDescriptor:d];
}

bool GlyphAtlas::encodeClearTexture(id<MTLBlitCommandEncoder> blit,
                                    id<MTLTexture> texture,
                                    std::uint32_t width,
                                    std::uint32_t height) const {
  if (!blit || !texture || width == 0 || height == 0) {
    return false;
  }
  std::size_t const byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  id<MTLBuffer> zeroBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(byteCount) options:MTLResourceStorageModeShared];
  if (!zeroBuffer) {
    return false;
  }
  std::memset([zeroBuffer contents], 0, byteCount);
  [blit copyFromBuffer:zeroBuffer
          sourceOffset:0
     sourceBytesPerRow:width
   sourceBytesPerImage:width * height
            sourceSize:MTLSizeMake(width, height, 1)
             toTexture:texture
      destinationSlice:0
      destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
  return true;
}

bool GlyphAtlas::clearTexture(id<MTLTexture> texture, std::uint32_t width, std::uint32_t height) const {
  id<MTLCommandBuffer> commandBuffer = queue_ ? [queue_ commandBuffer] : nil;
  id<MTLBlitCommandEncoder> blit = commandBuffer ? [commandBuffer blitCommandEncoder] : nil;
  if (!commandBuffer || !blit) {
    return false;
  }
  if (!encodeClearTexture(blit, texture, width, height)) {
    [blit endEncoding];
    return false;
  }
  [blit endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  return commandBuffer.status == MTLCommandBufferStatusCompleted;
}

void GlyphAtlas::queueUpload(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                             std::uint32_t height, std::vector<std::uint8_t> const& r8) {
  if (!texture_ || width == 0 || height == 0 || r8.empty()) {
    return;
  }
  id<MTLBuffer> buffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(r8.size()) options:MTLResourceStorageModeShared];
  if (!buffer) {
    throw std::runtime_error("GlyphAtlas: failed to allocate glyph upload buffer");
  }
  std::memcpy([buffer contents], r8.data(), r8.size());
  pendingUploads_.push_back(PendingUpload{
      .buffer = buffer,
      .texture = texture_,
      .x = x,
      .y = y,
      .width = width,
      .height = height,
  });
}

void GlyphAtlas::flushUploads(id<MTLCommandBuffer> commandBuffer) {
  if (!commandBuffer || pendingUploads_.empty()) {
    return;
  }
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (!blit) {
    return;
  }
  for (PendingUpload const& upload : pendingUploads_) {
    if (!upload.buffer || !upload.texture || upload.width == 0 || upload.height == 0) {
      continue;
    }
    [blit copyFromBuffer:upload.buffer
            sourceOffset:0
       sourceBytesPerRow:upload.width
     sourceBytesPerImage:upload.width * upload.height
              sourceSize:MTLSizeMake(upload.width, upload.height, 1)
               toTexture:upload.texture
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(upload.x, upload.y, 0)];
  }
  [blit endEncoding];
  pendingUploads_.clear();
}

bool GlyphAtlas::pressureHighForHeadroom() const {
  return allocator_.pressureHighForHeadroom();
}

void GlyphAtlas::prepareForFrameBegin() {
  // Intentionally empty. `grow()` can submit a texture-copy command buffer; keeping
  // it after present avoids frame-begin hitches while preserving queue ordering.
}

void GlyphAtlas::afterPresent() {
  if (pendingGrow_ || pressureHighForHeadroom()) {
    (void)grow();
  }
}

bool GlyphAtlas::grow() {
  if (beforeGrow_ && !beforeGrow_()) {
    pendingGrow_ = true;
    return false;
  }
  std::optional<GlyphAtlasSize> const nextSize = allocator_.nextDoubledSize();
  if (!nextSize) {
    throw std::runtime_error("GlyphAtlas: atlas exceeds maximum size");
  }

  std::uint32_t const oldWidth = allocator_.width();
  std::uint32_t const oldHeight = allocator_.height();
  std::uint32_t const newWidth = nextSize->width;
  std::uint32_t const newHeight = nextSize->height;
  id<MTLTexture> oldTexture = texture_;
  id<MTLTexture> newTex = createTexture(newWidth, newHeight);
  if (!newTex) {
    throw std::runtime_error("GlyphAtlas: grow failed to allocate texture");
  }

  id<MTLCommandBuffer> commandBuffer = [queue_ commandBuffer];
  id<MTLBlitCommandEncoder> blit = commandBuffer ? [commandBuffer blitCommandEncoder] : nil;
  if (!commandBuffer || !blit) {
    throw std::runtime_error("GlyphAtlas: grow failed to create blit command buffer");
  }
  if (!encodeClearTexture(blit, newTex, newWidth, newHeight)) {
    [blit endEncoding];
    throw std::runtime_error("GlyphAtlas: grow failed to clear new texture");
  }
  if (oldTexture) {
    [blit copyFromTexture:oldTexture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(oldWidth, oldHeight, 1)
                toTexture:newTex
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
  }
  for (PendingUpload& upload : pendingUploads_) {
    upload.texture = newTex;
    if (!upload.buffer || upload.width == 0 || upload.height == 0) {
      continue;
    }
    [blit copyFromBuffer:upload.buffer
            sourceOffset:0
       sourceBytesPerRow:upload.width
     sourceBytesPerImage:upload.width * upload.height
              sourceSize:MTLSizeMake(upload.width, upload.height, 1)
               toTexture:newTex
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(upload.x, upload.y, 0)];
  }
  [blit endEncoding];
  [commandBuffer commit];

  allocator_.resize(newWidth, newHeight);
  texture_ = newTex;
  ++generation_;
  pendingUploads_.clear();
  pendingGrow_ = false;
  debug::perf::recordGlyphAtlasGrowth(static_cast<std::uint64_t>(oldWidth) * oldHeight,
                                      static_cast<std::uint64_t>(newWidth) * newHeight);
  return true;
}

std::optional<AtlasEntry> GlyphAtlas::allocateAndUpload(GlyphKey const& key) {
  float const size = static_cast<float>(key.size) / 4.f;
  std::uint32_t gw = 0;
  std::uint32_t gh = 0;
  Point bearing{};
  std::vector<std::uint8_t> bits =
      textSystem_.rasterizeGlyph(key.fontId, key.glyphId, size, gw, gh, bearing);
  if (gw == 0 || gh == 0 || bits.empty()) {
    return AtlasEntry{};
  }

  while (!allocator_.hasSpace(gw, gh)) {
    if (!grow()) {
      return std::nullopt;
    }
  }
  std::optional<GlyphAtlasPlacement> const placement = allocator_.allocate(gw, gh);
  if (!placement) {
    return std::nullopt;
  }
  queueUpload(placement->x, placement->y, gw, gh, bits);

  AtlasEntry e{};
  e.u = static_cast<std::uint16_t>(placement->x);
  e.v = static_cast<std::uint16_t>(placement->y);
  e.width = static_cast<std::uint16_t>(gw);
  e.height = static_cast<std::uint16_t>(gh);
  e.bearing = bearing;
  return e;
}

AtlasEntry const& GlyphAtlas::getOrUpload(GlyphKey const& key) {
  if (allocator_.find(key)) {
    return allocator_.touch(key);
  }
  std::optional<AtlasEntry> e = allocateAndUpload(key);
  if (!e) {
    return transientEmptyEntry_;
  }
  return allocator_.insert(key,
                           GlyphAtlasPlacement{.x = e->u, .y = e->v},
                           e->width,
                           e->height,
                           e->bearing);
}

} // namespace lambdaui
