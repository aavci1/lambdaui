#include "Graphics/WebGPU/WebGpuContext.hpp"

#if LAMBDAUI_DAWN_LEGACY_NATIVE
#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lambdaui::webgpu {

namespace {

#if LAMBDAUI_DAWN_LEGACY_NATIVE

void uncapturedErrorCallback(WGPUErrorType, char const* message, void*) {
#ifndef NDEBUG
  std::string const text = stringFromView(message);
  if (!text.empty()) {
    std::fprintf(stderr, "Lambda WebGPU uncaptured error: %s\n", text.c_str());
  }
#else
  (void)message;
#endif
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

#else

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

#endif

} // namespace

#if LAMBDAUI_DAWN_LEGACY_NATIVE
WebGpuLabel stringView(char const* value) noexcept {
  return value ? value : "";
}

std::string stringFromView(WebGpuLabel value) {
  return value ? std::string(value) : std::string{};
}
#else
WebGpuLabel stringView(char const* value) noexcept {
  return WGPUStringView{value, value ? WGPU_STRLEN : 0};
}

std::string stringFromView(WebGpuLabel value) {
  if (!value.data) {
    return {};
  }
  if (value.length == WGPU_STRLEN) {
    return std::string(value.data);
  }
  return std::string(value.data, value.length);
}
#endif

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
#if LAMBDAUI_DAWN_LEGACY_NATIVE
  nativeInstance_.reset();
#endif
}

WebGpuContext::WebGpuContext(WebGpuContext&& other) noexcept
    : instance_(std::exchange(other.instance_, nullptr)),
      adapter_(std::exchange(other.adapter_, nullptr)),
      device_(std::exchange(other.device_, nullptr)),
      queue_(std::exchange(other.queue_, nullptr))
#if LAMBDAUI_DAWN_LEGACY_NATIVE
      ,
      nativeInstance_(std::move(other.nativeInstance_))
#endif
{}

WebGpuContext& WebGpuContext::operator=(WebGpuContext&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  releaseHandles(instance_, adapter_, device_, queue_);
  instance_ = std::exchange(other.instance_, nullptr);
  adapter_ = std::exchange(other.adapter_, nullptr);
  device_ = std::exchange(other.device_, nullptr);
  queue_ = std::exchange(other.queue_, nullptr);
#if LAMBDAUI_DAWN_LEGACY_NATIVE
  nativeInstance_ = std::move(other.nativeInstance_);
#endif
  return *this;
}

void WebGpuContext::createInstance() {
#if LAMBDAUI_DAWN_LEGACY_NATIVE
  WGPUInstanceDescriptor descriptor{};
  nativeInstance_ = std::make_unique<dawn::native::Instance>(&descriptor);
  dawnProcSetProcs(&dawn::native::GetProcs());
  instance_ = nativeInstance_->Get();
  if (instance_) {
    wgpuInstanceReference(instance_);
  }
  if (!instance_) {
    throw std::runtime_error("Lambda WebGPU: failed to create Dawn native instance");
  }
#else
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
#endif
}

void WebGpuContext::initializeDevice(WGPUSurface compatibleSurface) {
  if (!instance_) {
    createInstance();
  }
  if (device_) {
    throw std::runtime_error("Lambda WebGPU: device already initialized");
  }

#if LAMBDAUI_DAWN_LEGACY_NATIVE
  (void)compatibleSurface;
  nativeInstance_->DiscoverDefaultAdapters();
  std::vector<dawn::native::Adapter> adapters = nativeInstance_->GetAdapters();
  if (adapters.empty()) {
    throw std::runtime_error("Lambda WebGPU: no Dawn adapters are available");
  }

#if defined(__APPLE__)
  constexpr WGPUBackendType kPreferredBackend = WGPUBackendType_Metal;
#else
  constexpr WGPUBackendType kPreferredBackend = WGPUBackendType_Vulkan;
#endif
  auto selected = std::find_if(adapters.begin(), adapters.end(), [](dawn::native::Adapter const& adapter) {
    WGPUAdapterProperties properties{};
    adapter.GetProperties(&properties);
    return properties.backendType == kPreferredBackend;
  });
  if (selected == adapters.end()) {
    selected = adapters.begin();
  }

  adapter_ = selected->Get();
  if (adapter_) {
    wgpuAdapterReference(adapter_);
  }
  WGPUDeviceDescriptor deviceDescriptor{};
  deviceDescriptor.label = stringView("LambdaUI WebGPU Device");
  device_ = selected->CreateDevice(&deviceDescriptor);
  if (!device_) {
    throw std::runtime_error("Lambda WebGPU: failed to create Dawn native device");
  }
  wgpuDeviceSetUncapturedErrorCallback(device_, uncapturedErrorCallback, nullptr);

  queue_ = wgpuDeviceGetQueue(device_);
  if (!queue_) {
    throw std::runtime_error("Lambda WebGPU: failed to get device queue");
  }
#else
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
#endif
}

void WebGpuContext::initializeExternalDevice(WGPUDevice device, WGPUQueue queue) {
  if (device_) {
    throw std::runtime_error("Lambda WebGPU: device already initialized");
  }
  if (!device) {
    throw std::runtime_error("Lambda WebGPU: external device is null");
  }

  device_ = device;
#if LAMBDAUI_DAWN_LEGACY_NATIVE
  wgpuDeviceReference(device_);
#else
  wgpuDeviceAddRef(device_);
#endif

  if (queue) {
    queue_ = queue;
#if LAMBDAUI_DAWN_LEGACY_NATIVE
    wgpuQueueReference(queue_);
#else
    wgpuQueueAddRef(queue_);
#endif
  } else {
    queue_ = wgpuDeviceGetQueue(device_);
  }
  if (!queue_) {
    throw std::runtime_error("Lambda WebGPU: failed to get external device queue");
  }
}

} // namespace lambdaui::webgpu
