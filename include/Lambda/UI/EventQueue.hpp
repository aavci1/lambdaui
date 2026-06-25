#pragma once

#include <Lambda/Reactive/SmallFn.hpp>

/// \file Lambda/UI/EventQueue.hpp
///
/// Part of the Lambda public API.


#include <Lambda/UI/Events.hpp>

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <typeinfo>

namespace lambdaui {

class Application;
class EventQueue;

namespace detail {

template<typename T>
inline constexpr bool isEventAlternativeV =
    std::is_same_v<T, WindowLifecycleEvent> || std::is_same_v<T, WindowEvent> ||
    std::is_same_v<T, InputEvent> || std::is_same_v<T, TimerEvent> ||
    std::is_same_v<T, FrameEvent> ||
    std::is_same_v<T, CustomEvent>;

template<typename T>
inline std::uint32_t eventQueueCustomTypeId() {
  return static_cast<std::uint32_t>(std::type_index(typeid(T)).hash_code());
}

struct EventQueueImplAccess;

} // namespace detail

class EventSubscription {
public:
  EventSubscription() noexcept = default;
  ~EventSubscription();

  EventSubscription(EventSubscription const&) = delete;
  EventSubscription& operator=(EventSubscription const&) = delete;

  EventSubscription(EventSubscription&& other) noexcept;
  EventSubscription& operator=(EventSubscription&& other) noexcept;

  void reset() noexcept;
  explicit operator bool() const noexcept { return queue_ != nullptr && id_ != 0; }

private:
  enum class Kind : std::uint8_t {
    None,
    Framework,
    Custom,
  };

  EventSubscription(EventQueue* queue, Kind kind, std::type_index frameworkType,
                    std::uint32_t customType, std::uint64_t id) noexcept;

  EventQueue* queue_ = nullptr;
  Kind kind_ = Kind::None;
  std::type_index frameworkType_{typeid(void)};
  std::uint32_t customType_ = 0;
  std::uint64_t id_ = 0;

  friend struct detail::EventQueueImplAccess;
};

/// Application-owned queue; obtain via `Application::instance().eventQueue()`.
/// `post`, `dispatch`, and `on` are main-thread-only by contract (not enforced at runtime).
class EventQueue {
public:
  ~EventQueue();

  EventQueue(const EventQueue&) = delete;
  EventQueue& operator=(const EventQueue&) = delete;

  void post(Event event);

  /// User-defined payloads: wrapped in `CustomEvent`. Not for `Event` or first-class alternatives.
  template<typename T>
    requires(!detail::isEventAlternativeV<std::remove_cvref_t<T>> &&
             !std::is_same_v<std::remove_cvref_t<T>, Event>)
  void post(T&& value) {
    CustomEvent ce;
    ce.type = detail::eventQueueCustomTypeId<std::remove_cvref_t<T>>();
    ce.payload = std::forward<T>(value);
    post(Event(std::move(ce)));
  }

  /// First-class events (`WindowEvent`, `InputEvent`, …) and `CustomEvent` payloads.
  template<typename T>
    requires(detail::isEventAlternativeV<std::remove_cvref_t<T>>)
  void post(T&& value) {
    post(Event(std::forward<T>(value)));
  }

  template<typename T>
  [[nodiscard]] EventSubscription on(Reactive::SmallFn<void(T const&)> handler);

  void dispatch();

private:
  friend class Application;

  EventQueue();

  friend struct detail::EventQueueImplAccess;
  struct Impl;
  std::unique_ptr<Impl> d;
};

namespace detail {

/// Internal; used by `EventQueue` templates and implementation. Do not call from application code.
struct EventQueueImplAccess {
  static void postInner(EventQueue& q, Event&& event);
  static EventSubscription addFrameworkHandler(EventQueue& q, std::type_index idx,
                                               Reactive::SmallFn<void(std::any const&)> handler);
  static EventSubscription addCustomHandler(EventQueue& q, std::uint32_t tid,
                                            Reactive::SmallFn<void(std::any const&)> handler);
  static void removeHandler(EventQueue& q, EventSubscription::Kind kind, std::type_index frameworkType,
                            std::uint32_t customType, std::uint64_t id) noexcept;
  static void dispatchOne(EventQueue& q, Event& event);
  static std::size_t liveHandlerCountForTesting(EventQueue const& q);
};

} // namespace detail

template<typename T>
EventSubscription EventQueue::on(Reactive::SmallFn<void(T const&)> handler) {
  if constexpr (detail::isEventAlternativeV<T>) {
    auto wrapped = [h = std::move(handler)](std::any const& a) { h(std::any_cast<T const&>(a)); };
    return detail::EventQueueImplAccess::addFrameworkHandler(*this, std::type_index(typeid(T)),
                                                             std::move(wrapped));
  } else {
    std::uint32_t tid = detail::eventQueueCustomTypeId<T>();
    return detail::EventQueueImplAccess::addCustomHandler(
        *this, tid, [h = std::move(handler)](std::any const& a) {
          h(std::any_cast<T const&>(a));
        });
  }
}

} // namespace lambdaui
