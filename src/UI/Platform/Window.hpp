#pragma once

#include <memory>
#include <optional>
#include <cstdint>
#include <string>

#include <Lambda/UI/Cursor.hpp>
#include <Lambda/Core/Geometry.hpp>
#include <Lambda/UI/MenuItem.hpp>
#include <Lambda/UI/Views/Popover.hpp>
#include <Lambda/UI/WindowChrome.hpp>
#include <Lambda/UI/Window.hpp>

namespace lambdaui {

class Window;
class Canvas;
struct PopupMenu;
struct Popover;

namespace platform {

class WindowEventPump;

/// Internal abstract platform window; implemented in platform translation units. Not part of the public API.
class Window {
public:
  virtual ~Window() = default;

  virtual void setLambdaWindow(::lambdaui::Window* window) = 0;

  /// Present the native window after the Lambda `Window` is registered and `setLambdaWindow` has run.
  /// Implementations should not order the window on screen before this (so lifecycle callbacks see a
  /// valid `Window*`). Default: no-op.
  virtual void show() {}

  virtual std::unique_ptr<::lambdaui::Canvas> createWebGpuCanvas(::lambdaui::Window& owner) = 0;

  virtual void resize(const Size& newSize) = 0;
  virtual void setMinSize(Size /*size*/) {}
  virtual void setMaxSize(Size /*size*/) {}
  virtual void setFullscreen(bool fullscreen) = 0;
  virtual void setTitle(const std::string& title) = 0;
  virtual void setTitlebarMode(WindowTitlebarMode /*mode*/) {}
  virtual WindowTitlebarMode titlebarMode() const { return WindowTitlebarMode::System; }
  virtual void setTransientParent(unsigned int /*parentHandle*/, bool /*modal*/) {}
  virtual void setBackground(WindowBackground const& /*background*/) {}
  virtual WindowChromeMetrics chromeMetrics() const { return {}; }
  virtual void beginWindowDrag(std::uint32_t /*platformSerial*/ = 0) {}
  virtual void beginWindowResize(WindowResizeEdge /*edge*/, std::uint32_t /*platformSerial*/ = 0) {}
  virtual bool showPopupMenu(PopupMenu /*menu*/, Rect /*anchor*/, std::uint32_t /*platformSerial*/ = 0) {
    return false;
  }
  virtual PopoverSurfaceId showPopover(Popover /*popover*/, Rect /*anchor*/,
                                       std::uint32_t /*platformSerial*/ = 0) {
    return kInvalidPopoverSurfaceId;
  }
  virtual void repositionPopover(PopoverSurfaceId /*id*/, Popover const& /*popover*/, Rect /*anchor*/) {}
  virtual void dismissPopover(PopoverSurfaceId /*id*/) {}

  virtual Size currentSize() const = 0;
  virtual std::optional<Rect> currentFrame() const { return std::nullopt; }
  virtual void setFrame(Rect /*frame*/) {}
  virtual bool isFullscreen() const = 0;
  virtual unsigned int handle() const = 0;

  virtual WindowEventPump* eventPump() { return nullptr; }
  virtual WindowEventPump const* eventPump() const { return nullptr; }

  virtual void setCursor(Cursor /*kind*/) {}

  /// Layer-shell surfaces only. No-op on xdg toplevel windows.
  virtual void setLayerShellKeyboardInteractive(bool /*enabled*/) {}
  /// Layer-shell surfaces only. No-op on xdg toplevel windows.
  virtual void setLayerShellOptions(LayerShellOptions const& /*options*/) {}

  [[nodiscard]] virtual PlatformWindowCapabilities capabilities() const { return {}; }
};

} // namespace platform
} // namespace lambdaui
