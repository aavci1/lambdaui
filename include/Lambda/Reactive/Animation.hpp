#pragma once

#include <Lambda/Reactive/Interpolatable.hpp>
#include <Lambda/Reactive/AnimationClock.hpp>
#include <Lambda/Reactive/Transition.hpp>
#include <Lambda/Reactive/Signal.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lambdaui {

struct AnimationOptions {
  static constexpr int kRepeatForever = -1;

  Transition transition = Transition::ease();
  int repeat = 1;
  bool autoreverse = false;
};

class AnimationBase {
public:
  virtual ~AnimationBase() = default;
  virtual bool tick(double nowSeconds) = 0;
  virtual bool requestsRedraw() const noexcept { return false; }
  virtual void cancelFromClock(double) {}
};

namespace detail {

inline double steadyNowSeconds() {
  auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
  return static_cast<double>(ns.count()) * 1e-9;
}

inline float applyTimelineEasing(Transition const& transition, float progress) {
  float t = std::clamp(progress, 0.f, 1.f);
  if (transition.springFn) {
    return (*transition.springFn)(t);
  }
  if (transition.easing) {
    return std::clamp(transition.easing(t), 0.f, 1.f);
  }
  return t;
}

enum class TimelinePhase {
  Pending,
  Running,
  Finished,
};

struct TimelineEvaluation {
  TimelinePhase phase = TimelinePhase::Pending;
  double activeElapsed = 0.0;
  float progress = 0.f;
  float easedProgress = 0.f;
  int repeat = 1;
  int iteration = 0;
};

inline int normalizedTimelineRepeat(int repeat) {
  return repeat == 0 ? 1 : repeat;
}

inline float finalTimelineProgress(int repeat, bool autoreverse) {
  int const normalizedRepeat = normalizedTimelineRepeat(repeat);
  if (autoreverse &&
      normalizedRepeat != AnimationOptions::kRepeatForever &&
      (normalizedRepeat % 2) == 0) {
    return 0.f;
  }
  return 1.f;
}

inline TimelineEvaluation evaluateTimeline(double elapsed,
                                           double duration,
                                           double delay,
                                           Transition const& transition,
                                           int repeat,
                                           bool autoreverse) {
  TimelineEvaluation result{};
  result.repeat = normalizedTimelineRepeat(repeat);
  result.activeElapsed = elapsed - delay;
  if (result.activeElapsed < 0.0) {
    return result;
  }

  if (duration <= 0.0) {
    result.phase = TimelinePhase::Finished;
    result.progress = finalTimelineProgress(result.repeat, autoreverse);
    result.easedProgress = applyTimelineEasing(transition, result.progress);
    return result;
  }

  if (result.repeat != AnimationOptions::kRepeatForever &&
      result.activeElapsed >= duration * static_cast<double>(result.repeat)) {
    result.phase = TimelinePhase::Finished;
    result.progress = finalTimelineProgress(result.repeat, autoreverse);
    result.easedProgress = applyTimelineEasing(transition, result.progress);
    return result;
  }

  result.phase = TimelinePhase::Running;
  result.iteration = static_cast<int>(std::floor(result.activeElapsed / duration));
  double const local =
      result.activeElapsed - static_cast<double>(result.iteration) * duration;
  result.progress = static_cast<float>(std::clamp(local / duration, 0.0, 1.0));
  if (autoreverse && (result.iteration % 2) == 1) {
    result.progress = 1.f - result.progress;
  }
  result.easedProgress = applyTimelineEasing(transition, result.progress);
  return result;
}

inline void validateTimelineTiming(double duration, double delay, char const* apiName) {
  if (duration < 0.0) {
    throw std::invalid_argument(std::string{apiName} + ": duration must be non-negative");
  }
  if (delay < 0.0) {
    throw std::invalid_argument(std::string{apiName} + ": delay must be non-negative");
  }
}

template<typename T>
bool animationValuesEqual(T const& lhs, T const& rhs) {
  if constexpr (std::equality_comparable<T>) {
    return lhs == rhs;
  } else {
    return false;
  }
}

enum class TimelineClipStatus {
  Running,
  Finished,
  Cancelled,
};

} // namespace detail

template<Interpolatable T>
struct AnimationParams {
  T from{};
  T to{};
  /// Clip duration in seconds, excluding delay. Zero-duration clips finish on the next clock tick.
  double duration = 0.0;
  /// Seconds to hold `from` before interpolation begins.
  double delay = 0.0;
  /// Absolute monotonic clock time at which the delay begins.
  double startedAt = AnimationClock::nowSeconds();
  /// Interpolation curve. `duration` and `delay` are taken from this parameter struct.
  Transition transition = Transition::ease();
  /// Fired once on natural completion. Explicit cancellation and shutdown do not fire it.
  Reactive::SmallFn<void()> onComplete;
};

template<Interpolatable T>
class Animation;

/// Submits a single-segment timeline clip to the shared animation clock.
template<Interpolatable T>
Animation<T> addAnimation(AnimationParams<T> params);

namespace detail {

template<Interpolatable T>
class AnimationClipState final : public AnimationBase {
public:
  explicit AnimationClipState(AnimationParams<T> params)
      : from_(std::move(params.from))
      , to_(std::move(params.to))
      , duration_(params.duration)
      , delay_(params.delay)
      , startedAt_(params.startedAt)
      , transition_(std::move(params.transition))
      , onComplete_(std::move(params.onComplete)) {}

  bool tick(double nowSeconds) override {
    if (isTerminal()) {
      return false;
    }
    TimelineEvaluation const timeline = timelineAt(nowSeconds);
    if (timeline.phase == TimelinePhase::Pending) {
      return true;
    }
    if (timeline.phase == TimelinePhase::Finished) {
      finish();
      return false;
    }
    return true;
  }

  bool requestsRedraw() const noexcept override {
    return !isTerminal();
  }

  void cancelFromClock(double nowSeconds) override {
    cancel(nowSeconds);
  }

  T sample(double nowSeconds) const {
    if (status_ == TimelineClipStatus::Finished || status_ == TimelineClipStatus::Cancelled) {
      return cachedValue_;
    }
    TimelineEvaluation const timeline = timelineAt(nowSeconds);
    if (timeline.activeElapsed <= 0.0) {
      return from_;
    }
    if (timeline.phase == TimelinePhase::Finished) {
      return to_;
    }
    return lerp(from_, to_, timeline.easedProgress);
  }

  bool isStarted(double nowSeconds) const {
    if (status_ == TimelineClipStatus::Cancelled) {
      return true;
    }
    return timelineAt(nowSeconds).phase != TimelinePhase::Pending;
  }

  bool isFinished(double nowSeconds) const {
    if (status_ == TimelineClipStatus::Finished || status_ == TimelineClipStatus::Cancelled) {
      return true;
    }
    return timelineAt(nowSeconds).phase == TimelinePhase::Finished;
  }

  float progress(double nowSeconds) const {
    if (status_ == TimelineClipStatus::Finished || status_ == TimelineClipStatus::Cancelled) {
      return 1.f;
    }
    TimelineEvaluation const timeline = timelineAt(nowSeconds);
    if (timeline.activeElapsed <= 0.0) {
      return 0.f;
    }
    if (timeline.phase == TimelinePhase::Finished) {
      return 1.f;
    }
    return timeline.progress;
  }

  void cancel(double nowSeconds) {
    if (isTerminal()) {
      return;
    }
    cachedValue_ = sample(nowSeconds);
    onComplete_ = {};
    status_ = TimelineClipStatus::Cancelled;
  }

#if defined(LAMBDAUI_TESTING)
  void testSetStartedAt(double startedAt) { startedAt_ = startedAt; }
#endif

private:
  bool isTerminal() const noexcept {
    return status_ == TimelineClipStatus::Finished || status_ == TimelineClipStatus::Cancelled;
  }

  TimelineEvaluation timelineAt(double nowSeconds) const {
    return evaluateTimeline(
        nowSeconds - startedAt_,
        duration_,
        delay_,
        transition_,
        1,
        false);
  }

  void finish() {
    if (isTerminal()) {
      return;
    }
    cachedValue_ = to_;
    status_ = TimelineClipStatus::Finished;
    Reactive::SmallFn<void()> onComplete = std::move(onComplete_);
    onComplete_ = {};
    if (onComplete) {
      onComplete();
    }
  }

  T from_{};
  T to_{};
  double duration_ = 0.0;
  double delay_ = 0.0;
  double startedAt_ = 0.0;
  Transition transition_ = Transition::ease();
  Reactive::SmallFn<void()> onComplete_;
  T cachedValue_{};
  TimelineClipStatus status_ = TimelineClipStatus::Running;
};

} // namespace detail

/// Handle to a single-segment timeline clip. The clock owns the running clip; dropping the handle does not cancel it.
template<Interpolatable T>
class Animation {
public:
  Animation() = default;

  /// Samples the clip at the current animation-clock time.
  T value() const {
    if (!state_) {
      return T{};
    }
    return state_->sample(AnimationClock::nowSeconds());
  }

  bool isStarted() const {
    return !state_ || state_->isStarted(AnimationClock::nowSeconds());
  }

  bool isFinished() const {
    return !state_ || state_->isFinished(AnimationClock::nowSeconds());
  }

  bool isActive() const {
    return isStarted() && !isFinished();
  }

  float progress() const {
    if (!state_) {
      return 1.f;
    }
    return state_->progress(AnimationClock::nowSeconds());
  }

  bool operator==(Animation const& other) const {
    return state_ == other.state_;
  }

  explicit operator bool() const noexcept {
    return static_cast<bool>(state_);
  }

  void cancel() {
    if (!state_) {
      return;
    }
    state_->cancel(AnimationClock::nowSeconds());
    AnimationClock::instance().unregisterAnimation(state_.get());
  }

#if defined(LAMBDAUI_TESTING)
  T testValueAt(double nowSeconds) const { return state_ ? state_->sample(nowSeconds) : T{}; }
  bool testTick(double nowSeconds) const {
    if (!state_) {
      return false;
    }
    bool const running = state_->tick(nowSeconds);
    if (!running) {
      AnimationClock::instance().unregisterAnimation(state_.get());
    }
    return running;
  }
  void testSetStartedAt(double startedAt) const {
    if (state_) {
      state_->testSetStartedAt(startedAt);
    }
  }
#endif

private:
  explicit Animation(std::shared_ptr<detail::AnimationClipState<T>> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::AnimationClipState<T>> state_;

  friend Animation<T> addAnimation<T>(AnimationParams<T> params);
};

template<Interpolatable T>
Animation<T> addAnimation(AnimationParams<T> params) {
  detail::validateTimelineTiming(params.duration, params.delay, "addAnimation");
  auto state = std::make_shared<detail::AnimationClipState<T>>(std::move(params));
  Animation<T> handle{state};
  AnimationClock::instance().registerAnimation(std::static_pointer_cast<AnimationBase>(state));
  return handle;
}

template<Interpolatable T>
class Animated {
public:
  Animated()
      : Animated(T{}) {}

  explicit Animated(T initial)
      : state_(std::make_shared<State>(std::move(initial))) {}

  T const& get() const { return state().value.get(); }
  T const& evaluate() const { return get(); }
  T const& operator()() const { return get(); }
  T const& peek() const { return state().value.peek(); }
  T const& operator*() const { return get(); }
  operator T() const { return get(); }

  Reactive::Signal<T> signal() const { return state().value; }

  void set(T value, Transition transition = Transition::instant()) const {
    State& self = state();
    AnimationClock::instance().unregisterAnimation(&self);
    self.paused = false;
    self.start = self.value.peek();
    self.target = std::move(value);
    float const duration = transition.duration;
    self.options = AnimationOptions{.transition = std::move(transition)};
    if (duration <= 0.f || detail::animationValuesEqual(self.start, self.target)) {
      self.running = false;
      self.value.set(self.target);
      return;
    }
    self.running = true;
    self.startTime = detail::steadyNowSeconds();
    AnimationClock::instance().registerAnimation(&self);
  }

  Animated const& operator=(T value) const {
    set(std::move(value), WithTransition::current());
    return *this;
  }

  void play(T target, Transition transition) const {
    play(std::move(target), AnimationOptions{.transition = std::move(transition)});
  }

  void play(T target, AnimationOptions options = {}) const {
    State& self = state();
    AnimationClock::instance().unregisterAnimation(&self);
    self.start = self.value.peek();
    self.target = std::move(target);
    self.options = std::move(options);
    self.paused = false;
    if (self.options.transition.duration <= 0.f ||
        detail::animationValuesEqual(self.start, self.target)) {
      self.running = false;
      self.value.set(self.target);
      return;
    }
    self.running = true;
    self.startTime = detail::steadyNowSeconds();
    AnimationClock::instance().registerAnimation(&self);
  }

  void pause() const {
    State& self = state();
    if (!self.running) {
      return;
    }
    self.paused = true;
    self.running = false;
    AnimationClock::instance().unregisterAnimation(&self);
  }

  void resume() const {
    State& self = state();
    if (!self.paused) {
      return;
    }
    self.paused = false;
    self.running = true;
    AnimationClock::instance().registerAnimation(&self);
  }

  void stop() const {
    State& self = state();
    AnimationClock::instance().unregisterAnimation(&self);
    self.running = false;
    self.paused = false;
  }

  bool isRunning() const { return state().running; }
  bool isPaused() const { return state().paused; }

#if defined(LAMBDAUI_TESTING)
  void testSetStartTime(double startTime) const { state().startTime = startTime; }
  bool testTick(double nowSeconds) const { return state().tick(nowSeconds); }
#endif

private:
  struct State final : AnimationBase {
    explicit State(T initial)
        : value(std::move(initial))
        , start(value.peek())
        , target(value.peek()) {}

    ~State() override {
      AnimationClock::instance().unregisterAnimation(this);
    }

    bool tick(double nowSeconds) override {
      if (!running || paused) {
        return false;
      }
      double const duration = std::max(0.000001, static_cast<double>(options.transition.duration));
      detail::TimelineEvaluation const timeline = detail::evaluateTimeline(
          nowSeconds - startTime,
          duration,
          static_cast<double>(options.transition.delay),
          options.transition,
          options.repeat,
          options.autoreverse);
      if (timeline.phase == detail::TimelinePhase::Pending) {
        value.set(start);
        return true;
      }

      if (timeline.phase == detail::TimelinePhase::Finished) {
        running = false;
        paused = false;
        value.set(finalValueForOptions());
        return false;
      }

      value.set(lerp(start, target, timeline.easedProgress));
      return true;
    }

    T finalValueForOptions() const {
      if (detail::finalTimelineProgress(options.repeat, options.autoreverse) == 0.f) {
        return start;
      }
      return target;
    }

    Reactive::Signal<T> value{T{}};
    T start{};
    T target{};
    AnimationOptions options{};
    bool running = false;
    bool paused = false;
    double startTime = 0.0;
  };

  State& state() const {
    assert(state_ && "using an empty Animated handle");
    return *state_;
  }

  std::shared_ptr<State> state_;
};

} // namespace lambdaui
