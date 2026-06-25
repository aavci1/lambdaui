#include <Lambda/UI/EventQueue.hpp>

#include <Lambda/UI/Application.hpp>

#include <algorithm>
#include <any>
#include <array>
#include <deque>
#include <functional>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lambdaui {

namespace {

constexpr std::size_t kLifecycle = 0;
constexpr std::size_t kWindow = 1;
constexpr std::size_t kInput = 2;
constexpr std::size_t kTimer = 3;
constexpr std::size_t kFrame = 4;
constexpr std::size_t kCustom = 5;
constexpr std::size_t kBucketCount = kCustom + 1;
static_assert(kBucketCount == 6);

template<typename T>
constexpr std::size_t bucketFor() {
  if constexpr (std::is_same_v<T, WindowLifecycleEvent>) {
    return kLifecycle;
  } else if constexpr (std::is_same_v<T, WindowEvent>) {
    return kWindow;
  } else if constexpr (std::is_same_v<T, InputEvent>) {
    return kInput;
  } else if constexpr (std::is_same_v<T, TimerEvent>) {
    return kTimer;
  } else if constexpr (std::is_same_v<T, FrameEvent>) {
    return kFrame;
  } else if constexpr (std::is_same_v<T, CustomEvent>) {
    return kCustom;
  } else {
    static_assert(sizeof(T) == 0, "unknown event type");
    return 0;
  }
}

} // namespace

struct EventQueue::Impl {
  using AnyHandler = Reactive::SmallFn<void(std::any const&)>;
  struct HandlerEntry {
    std::uint64_t id = 0;
    AnyHandler handler;
    bool removed = false;
  };

  std::mutex mutex_{};
  std::array<std::deque<Event>, kBucketCount> buckets_{};
  std::unordered_map<std::type_index, std::vector<HandlerEntry>> frameworkHandlers_;
  std::unordered_map<std::uint32_t, std::vector<HandlerEntry>> customPayloadHandlers_;
  std::uint64_t nextHandlerId_ = 1;

  bool dispatching_ = false;

  std::uint64_t nextHandlerId() noexcept {
    std::uint64_t const id = nextHandlerId_++;
    if (nextHandlerId_ == 0) {
      nextHandlerId_ = 1;
    }
    return id == 0 ? nextHandlerId() : id;
  }

  template<typename Key>
  void removeFrom(std::unordered_map<Key, std::vector<HandlerEntry>>& handlers,
                  Key const& key,
                  std::uint64_t id) noexcept {
    auto it = handlers.find(key);
    if (it == handlers.end()) {
      return;
    }
    auto& entries = it->second;
    auto entryIt = std::find_if(entries.begin(), entries.end(), [&](HandlerEntry const& entry) {
      return entry.id == id;
    });
    if (entryIt == entries.end()) {
      return;
    }
    if (dispatching_) {
      entryIt->removed = true;
      entryIt->handler = {};
      return;
    }
    entries.erase(entryIt);
    if (entries.empty()) {
      handlers.erase(it);
    }
  }

  template<typename Key>
  void compact(std::unordered_map<Key, std::vector<HandlerEntry>>& handlers) {
    for (auto it = handlers.begin(); it != handlers.end();) {
      auto& entries = it->second;
      entries.erase(std::remove_if(entries.begin(), entries.end(), [](HandlerEntry const& entry) {
        return entry.removed;
      }), entries.end());
      if (entries.empty()) {
        it = handlers.erase(it);
      } else {
        ++it;
      }
    }
  }

  void compactRemovedHandlers() {
    compact(frameworkHandlers_);
    compact(customPayloadHandlers_);
  }

  template<typename Key>
  std::vector<std::uint64_t> liveIds(std::unordered_map<Key, std::vector<HandlerEntry>> const& handlers,
                                     Key const& key) const {
    std::vector<std::uint64_t> ids;
    auto it = handlers.find(key);
    if (it == handlers.end()) {
      return ids;
    }
    ids.reserve(it->second.size());
    for (HandlerEntry const& entry : it->second) {
      if (!entry.removed && entry.handler) {
        ids.push_back(entry.id);
      }
    }
    return ids;
  }

  template<typename Key>
  AnyHandler handlerForId(std::unordered_map<Key, std::vector<HandlerEntry>> const& handlers,
                          Key const& key,
                          std::uint64_t id) const {
    auto it = handlers.find(key);
    if (it == handlers.end()) {
      return {};
    }
    auto entryIt = std::find_if(it->second.begin(), it->second.end(), [&](HandlerEntry const& entry) {
      return entry.id == id;
    });
    if (entryIt == it->second.end() || entryIt->removed || !entryIt->handler) {
      return {};
    }
    return entryIt->handler;
  }

  template<typename Key>
  std::size_t liveCount(std::unordered_map<Key, std::vector<HandlerEntry>> const& handlers) const {
    std::size_t count = 0;
    for (auto const& [_, entries] : handlers) {
      for (HandlerEntry const& entry : entries) {
        if (!entry.removed && entry.handler) {
          ++count;
        }
      }
    }
    return count;
  }
};

EventQueue::EventQueue() : d(std::make_unique<Impl>()) {}

EventQueue::~EventQueue() = default;

EventSubscription::EventSubscription(EventQueue* queue, Kind kind, std::type_index frameworkType,
                                     std::uint32_t customType, std::uint64_t id) noexcept
    : queue_(queue)
    , kind_(kind)
    , frameworkType_(frameworkType)
    , customType_(customType)
    , id_(id) {}

EventSubscription::~EventSubscription() {
  reset();
}

EventSubscription::EventSubscription(EventSubscription&& other) noexcept
    : queue_(std::exchange(other.queue_, nullptr))
    , kind_(std::exchange(other.kind_, Kind::None))
    , frameworkType_(std::exchange(other.frameworkType_, std::type_index(typeid(void))))
    , customType_(std::exchange(other.customType_, 0))
    , id_(std::exchange(other.id_, 0)) {}

EventSubscription& EventSubscription::operator=(EventSubscription&& other) noexcept {
  if (this != &other) {
    reset();
    queue_ = std::exchange(other.queue_, nullptr);
    kind_ = std::exchange(other.kind_, Kind::None);
    frameworkType_ = std::exchange(other.frameworkType_, std::type_index(typeid(void)));
    customType_ = std::exchange(other.customType_, 0);
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

void EventSubscription::reset() noexcept {
  if (!queue_ || id_ == 0) {
    return;
  }
  detail::EventQueueImplAccess::removeHandler(*queue_, kind_, frameworkType_, customType_, id_);
  queue_ = nullptr;
  kind_ = Kind::None;
  frameworkType_ = std::type_index(typeid(void));
  customType_ = 0;
  id_ = 0;
}

void EventQueue::dispatch() {
  if (d->dispatching_) {
    return;
  }
  d->dispatching_ = true;
  bool progress = true;
  while (progress) {
    progress = false;
    for (auto& bucket : d->buckets_) {
      while (true) {
        Event e;
        {
          std::lock_guard lock(d->mutex_);
          if (bucket.empty()) {
            break;
          }
          e = std::move(bucket.front());
          bucket.pop_front();
        }
        detail::EventQueueImplAccess::dispatchOne(*this, e);
        progress = true;
      }
    }
  }
  d->dispatching_ = false;
  d->compactRemovedHandlers();
}

void EventQueue::post(Event event) {
  detail::EventQueueImplAccess::postInner(*this, std::move(event));
  if (Application::hasInstance() && !Application::instance().isMainThread()) {
    Application::instance().wakeEventLoop();
  }
}

EventSubscription detail::EventQueueImplAccess::addFrameworkHandler(
    EventQueue& q, std::type_index idx, Reactive::SmallFn<void(std::any const&)> handler) {
  std::uint64_t const id = q.d->nextHandlerId();
  q.d->frameworkHandlers_[idx].push_back(EventQueue::Impl::HandlerEntry{
      .id = id,
      .handler = std::move(handler),
      .removed = false,
  });
  return EventSubscription{&q, EventSubscription::Kind::Framework, idx, 0, id};
}

EventSubscription detail::EventQueueImplAccess::addCustomHandler(
    EventQueue& q, std::uint32_t tid, Reactive::SmallFn<void(std::any const&)> handler) {
  std::uint64_t const id = q.d->nextHandlerId();
  q.d->customPayloadHandlers_[tid].push_back(EventQueue::Impl::HandlerEntry{
      .id = id,
      .handler = std::move(handler),
      .removed = false,
  });
  return EventSubscription{&q, EventSubscription::Kind::Custom, std::type_index(typeid(void)), tid, id};
}

void detail::EventQueueImplAccess::removeHandler(EventQueue& q, EventSubscription::Kind kind,
                                                 std::type_index frameworkType,
                                                 std::uint32_t customType,
                                                 std::uint64_t id) noexcept {
  if (id == 0) {
    return;
  }
  if (kind == EventSubscription::Kind::Framework) {
    q.d->removeFrom(q.d->frameworkHandlers_, frameworkType, id);
  } else if (kind == EventSubscription::Kind::Custom) {
    q.d->removeFrom(q.d->customPayloadHandlers_, customType, id);
  }
}

void detail::EventQueueImplAccess::postInner(EventQueue& q, Event&& event) {
  std::visit(
      [&q](auto&& ev) {
        using T = std::decay_t<decltype(ev)>;
        constexpr std::size_t b = bucketFor<T>();
        std::lock_guard lock(q.d->mutex_);
        q.d->buckets_[b].push_back(Event(std::move(ev)));
      },
      std::move(event));
}

void detail::EventQueueImplAccess::dispatchOne(EventQueue& q, Event& event) {
  std::visit(
      [&q](auto& ev) {
        using T = std::decay_t<decltype(ev)>;
        std::type_index const eventType{typeid(T)};
        std::vector<std::uint64_t> handlerIds = q.d->liveIds(q.d->frameworkHandlers_, eventType);
        if (!handlerIds.empty()) {
          std::any a = ev;
          for (std::uint64_t const id : handlerIds) {
            auto handler = q.d->handlerForId(q.d->frameworkHandlers_, eventType, id);
            if (handler) {
              handler(a);
            }
          }
        }
        if constexpr (std::is_same_v<T, CustomEvent>) {
          std::vector<std::uint64_t> customIds = q.d->liveIds(q.d->customPayloadHandlers_, ev.type);
          for (std::uint64_t const id : customIds) {
            auto handler = q.d->handlerForId(q.d->customPayloadHandlers_, ev.type, id);
            if (handler) {
              handler(ev.payload);
            }
          }
        }
      },
      event);
}

std::size_t detail::EventQueueImplAccess::liveHandlerCountForTesting(EventQueue const& q) {
  return q.d->liveCount(q.d->frameworkHandlers_) + q.d->liveCount(q.d->customPayloadHandlers_);
}

} // namespace lambdaui
