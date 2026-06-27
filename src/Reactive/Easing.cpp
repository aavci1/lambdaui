#include <Lambda/Reactive/Easing.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace lambdaui::Easing {

namespace {

constexpr int kSpringSampleCount = 4096;

struct SpringTable {
  std::array<float, kSpringSampleCount + 1> values{};
};

using SpringKey = std::pair<float, float>;

std::mutex gSpringCacheMutex;
std::map<SpringKey, std::shared_ptr<SpringTable const>> gSpringCache;

#if defined(LAMBDAUI_TESTING)
std::atomic<std::size_t> gSpringIntegrationSteps{0};
#endif

void countSpringIntegrationStep() {
#if defined(LAMBDAUI_TESTING)
  gSpringIntegrationSteps.fetch_add(1, std::memory_order_relaxed);
#endif
}

std::shared_ptr<SpringTable const> buildSpringTable(float stiffness, float damping) {
  auto table = std::make_shared<SpringTable>();
  float x = 0.f;
  float v = 0.f;
  table->values[0] = x;
  constexpr float dt = 1.f / static_cast<float>(kSpringSampleCount);
  for (int i = 1; i <= kSpringSampleCount; ++i) {
    const float a = stiffness * (1.f - x) - damping * v;
    v += a * dt;
    x += v * dt;
    table->values[static_cast<std::size_t>(i)] = x;
    countSpringIntegrationStep();
  }
  return table;
}

std::shared_ptr<SpringTable const> springTable(float stiffness, float damping) {
  std::lock_guard lock(gSpringCacheMutex);
  SpringKey const key{stiffness, damping};
  if (auto found = gSpringCache.find(key); found != gSpringCache.end()) {
    return found->second;
  }
  auto table = buildSpringTable(stiffness, damping);
  gSpringCache.emplace(key, table);
  return table;
}

float sampleSpringTable(SpringTable const& table, float t) {
  t = std::clamp(t, 0.f, 1.f);
  float const scaled = t * static_cast<float>(kSpringSampleCount);
  int const index = std::clamp(static_cast<int>(std::floor(scaled)), 0, kSpringSampleCount - 1);
  float const fraction = scaled - static_cast<float>(index);
  float const a = table.values[static_cast<std::size_t>(index)];
  float const b = table.values[static_cast<std::size_t>(index + 1)];
  return a + (b - a) * fraction;
}

} // namespace

float linear(float t) {
  return t;
}

float easeIn(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t * t * t;
}

float easeOut(float t) {
  t = std::clamp(t, 0.f, 1.f);
  const float u = 1.f - t;
  return 1.f - u * u * u;
}

float easeInOut(float t) {
  t = std::clamp(t, 0.f, 1.f);
  if (t < 0.5f) {
    return 4.f * t * t * t;
  }
  const float u = -2.f * t + 2.f;
  return 1.f - (u * u * u) / 2.f;
}

std::function<float(float)> spring(float stiffness, float damping) {
  auto table = springTable(stiffness, damping);
  return [table = std::move(table)](float t) {
    return sampleSpringTable(*table, t);
  };
}

#if defined(LAMBDAUI_TESTING)
std::size_t debugSpringIntegrationStepCount() {
  return gSpringIntegrationSteps.load(std::memory_order_relaxed);
}

void debugResetSpringIntegrationStepCount() {
  gSpringIntegrationSteps.store(0, std::memory_order_relaxed);
}

void debugClearSpringCacheForTesting() {
  std::lock_guard lock(gSpringCacheMutex);
  gSpringCache.clear();
}
#endif

} // namespace lambdaui::Easing
