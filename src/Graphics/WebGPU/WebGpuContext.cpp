#include "Graphics/WebGPU/WebGpuContext.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace lambdaui::webgpu {

namespace {

struct AdapterRequestState {
  WGPUAdapter adapter = nullptr;
  WGPURequestAdapterStatus status = WGPURequestAdapterStatus_Error;
  std::string message;
};

struct DeviceRequestState {
  WGPUDevice device = nullptr;
  WGPURequestDeviceStatus status = WGPURequestDeviceStatus_Error;
  std::string message;
};

void adapterRequestCallback(WGPURequestAdapterStatus status,
                            WGPUAdapter adapter,
                            WGPUStringView message,
                            void* userdata1,
                            void*) {
  auto* state = static_cast<AdapterRequestState*>(userdata1);
  state->status = status;
  state->adapter = adapter;
  state->message = stringFromView(message);
}

void deviceRequestCallback(WGPURequestDeviceStatus status,
                           WGPUDevice device,
                           WGPUStringView message,
                           void* userdata1,
                           void*) {
  auto* state = static_cast<DeviceRequestState*>(userdata1);
  state->status = status;
  state->device = device;
  state->message = stringFromView(message);
}

void uncapturedErrorCallback(WGPUDevice const*,
                             WGPUErrorType,
                             WGPUStringView message,
                             void*,
                             void*) {
#ifndef NDEBUG
  std::string const text = stringFromView(message);
  if (!text.empty()) {
    std::fprintf(stderr, "Lambda WebGPU uncaptured error: %s\n", text.c_str());
  }
#else
  (void)message;
#endif
}

void waitFor(WGPUInstance instance, WGPUFuture future, char const* operation) {
  WGPUFutureWaitInfo waitInfo = WGPU_FUTURE_WAIT_INFO_INIT;
  waitInfo.future = future;
  WGPUWaitStatus const waitStatus = wgpuInstanceWaitAny(instance, 1, &waitInfo, UINT64_MAX);
  if (waitStatus != WGPUWaitStatus_Success || !waitInfo.completed) {
    throw std::runtime_error(std::string("Lambda WebGPU: ") + operation + " did not complete");
  }
}

void releaseHandles(WGPUInstance& instance, WGPUAdapter& adapter, WGPUDevice& device, WGPUQueue& queue) noexcept {
  if (queue) {
    wgpuQueueRelease(queue);
    queue = nullptr;
  }
  if (device) {
    wgpuDeviceRelease(device);
    device = nullptr;
  }
  if (adapter) {
    wgpuAdapterRelease(adapter);
    adapter = nullptr;
  }
  if (instance) {
    wgpuInstanceRelease(instance);
    instance = nullptr;
  }
}

} // namespace

WGPUStringView stringView(char const* value) noexcept {
  return WGPUStringView{value, value ? WGPU_STRLEN : 0};
}

std::string stringFromView(WGPUStringView value) {
  if (!value.data) {
    return {};
  }
  if (value.length == WGPU_STRLEN) {
    return std::string(value.data);
  }
  return std::string(value.data, value.length);
}

WebGpuContext::WebGpuContext() {
  createInstance();
}

WebGpuContext::WebGpuContext(WGPUSurface compatibleSurface) : WebGpuContext() {
  initializeDevice(compatibleSurface);
}

WebGpuContext::WebGpuContext(WGPUDevice device, WGPUQueue queue) {
  initializeExternalDevice(device, queue);
}

WebGpuContext::~WebGpuContext() {
  releaseHandles(instance_, adapter_, device_, queue_);
}

WebGpuContext::WebGpuContext(WebGpuContext&& other) noexcept
    : instance_(std::exchange(other.instance_, nullptr)),
      adapter_(std::exchange(other.adapter_, nullptr)),
      device_(std::exchange(other.device_, nullptr)),
      queue_(std::exchange(other.queue_, nullptr)) {}

WebGpuContext& WebGpuContext::operator=(WebGpuContext&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  releaseHandles(instance_, adapter_, device_, queue_);
  instance_ = std::exchange(other.instance_, nullptr);
  adapter_ = std::exchange(other.adapter_, nullptr);
  device_ = std::exchange(other.device_, nullptr);
  queue_ = std::exchange(other.queue_, nullptr);
  return *this;
}

void WebGpuContext::createInstance() {
  static constexpr WGPUInstanceFeatureName kInstanceFeatures[] = {
      WGPUInstanceFeatureName_TimedWaitAny,
  };

  WGPUInstanceDescriptor descriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
  descriptor.requiredFeatureCount = 1;
  descriptor.requiredFeatures = kInstanceFeatures;

  instance_ = wgpuCreateInstance(&descriptor);
  if (!instance_) {
    throw std::runtime_error("Lambda WebGPU: failed to create Dawn instance");
  }
}

void WebGpuContext::initializeDevice(WGPUSurface compatibleSurface) {
  if (!instance_) {
    createInstance();
  }
  if (device_) {
    throw std::runtime_error("Lambda WebGPU: device already initialized");
  }

  WGPURequestAdapterOptions adapterOptions = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  adapterOptions.compatibleSurface = compatibleSurface;

  AdapterRequestState adapterState;
  WGPURequestAdapterCallbackInfo adapterCallback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  adapterCallback.mode = WGPUCallbackMode_WaitAnyOnly;
  adapterCallback.callback = adapterRequestCallback;
  adapterCallback.userdata1 = &adapterState;
  waitFor(instance_, wgpuInstanceRequestAdapter(instance_, &adapterOptions, adapterCallback), "request adapter");
  if (adapterState.status != WGPURequestAdapterStatus_Success || !adapterState.adapter) {
    throw std::runtime_error("Lambda WebGPU: failed to request adapter: " + adapterState.message);
  }
  adapter_ = adapterState.adapter;

  WGPUDeviceDescriptor deviceDescriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
  deviceDescriptor.label = stringView("LambdaUI WebGPU Device");
  deviceDescriptor.uncapturedErrorCallbackInfo.callback = uncapturedErrorCallback;

  DeviceRequestState deviceState;
  WGPURequestDeviceCallbackInfo deviceCallback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  deviceCallback.mode = WGPUCallbackMode_WaitAnyOnly;
  deviceCallback.callback = deviceRequestCallback;
  deviceCallback.userdata1 = &deviceState;
  waitFor(instance_, wgpuAdapterRequestDevice(adapter_, &deviceDescriptor, deviceCallback), "request device");
  if (deviceState.status != WGPURequestDeviceStatus_Success || !deviceState.device) {
    throw std::runtime_error("Lambda WebGPU: failed to request device: " + deviceState.message);
  }
  device_ = deviceState.device;

  queue_ = wgpuDeviceGetQueue(device_);
  if (!queue_) {
    throw std::runtime_error("Lambda WebGPU: failed to get device queue");
  }
}

void WebGpuContext::initializeExternalDevice(WGPUDevice device, WGPUQueue queue) {
  if (device_) {
    throw std::runtime_error("Lambda WebGPU: device already initialized");
  }
  if (!device) {
    throw std::runtime_error("Lambda WebGPU: external device is null");
  }

  device_ = device;
  wgpuDeviceAddRef(device_);

  if (queue) {
    queue_ = queue;
    wgpuQueueAddRef(queue_);
  } else {
    queue_ = wgpuDeviceGetQueue(device_);
  }
  if (!queue_) {
    throw std::runtime_error("Lambda WebGPU: failed to get external device queue");
  }
}

} // namespace lambdaui::webgpu
