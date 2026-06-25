#pragma once

namespace lambdaui::platform {

class WindowEventPump {
public:
  virtual ~WindowEventPump() = default;

  /// Drain queued platform events without blocking when a redraw is already pending.
  virtual void processEvents() {}

  /// Block until the next event or `timeoutMs` elapses; `timeoutMs < 0` waits indefinitely.
  virtual void waitForEvents(int /*timeoutMs*/) {}
  virtual int eventFd() const { return -1; }
  virtual int wakeFd() const { return -1; }

  /// Wake `waitForEvents` after work is queued from another thread.
  virtual void wakeEventLoop() {}

  /// Arm the platform frame pump for the next display boundary.
  virtual void requestAnimationFrame() {}

  /// Marks the most recent frame boundary event as handled by the application loop.
  virtual void acknowledgeAnimationFrameTick() {}

  /// Called after a frame has been presented. `needsAnotherFrame` keeps the frame pump running.
  virtual void completeAnimationFrame(bool /*needsAnotherFrame*/) {}
};

} // namespace lambdaui::platform
