#include <doctest/doctest.h>

#include <Lambda/UI/Application.hpp>
#include <Lambda/UI/EventQueue.hpp>
#include <Lambda/UI/Events.hpp>

#include <atomic>
#include <chrono>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct EventQueuePayload {
  int value = 0;
};

struct TestPipe {
  int readFd = -1;
  int writeFd = -1;

  ~TestPipe() {
    if (readFd >= 0) {
      close(readFd);
    }
    if (writeFd >= 0) {
      close(writeFd);
    }
  }

  [[nodiscard]] bool open() {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
      return false;
    }
    readFd = fds[0];
    writeFd = fds[1];
    return true;
  }
};

} // namespace

TEST_CASE("EventQueue snapshots first-class handlers registered during dispatch") {
  lambdaui::Application app;
  lambdaui::EventQueue& queue = app.eventQueue();

  int primaryCalls = 0;
  int addedCalls = 0;
  std::vector<lambdaui::EventSubscription> subscriptions;
  subscriptions.push_back(queue.on<lambdaui::InputEvent>([&](lambdaui::InputEvent const&) {
    ++primaryCalls;
    for (int i = 0; i < 64; ++i) {
      subscriptions.push_back(queue.on<lambdaui::InputEvent>([&](lambdaui::InputEvent const&) {
        ++addedCalls;
      }));
    }
  }));

  queue.post(lambdaui::InputEvent{.kind = lambdaui::InputEvent::Kind::KeyDown});
  queue.dispatch();
  CHECK(primaryCalls == 1);
  CHECK(addedCalls == 0);

  queue.post(lambdaui::InputEvent{.kind = lambdaui::InputEvent::Kind::KeyDown});
  queue.dispatch();
  CHECK(primaryCalls == 2);
  CHECK(addedCalls == 64);
}

TEST_CASE("Application poll sources honor requested event masks") {
  lambdaui::Application app;
  TestPipe pipe;
  REQUIRE(pipe.open());

  std::atomic<bool> callbackRan = false;
  std::atomic<bool> sawWritable = false;
  std::uint64_t pollSourceId = 0;
  pollSourceId = app.registerEventPollSource(
      pipe.writeFd,
      [] {
        return POLLOUT;
      },
      [&](int revents) {
        sawWritable.store((revents & POLLOUT) != 0);
        callbackRan.store(true);
        app.unregisterEventPollSource(pollSourceId);
        app.quit();
      });
  REQUIRE(pollSourceId != 0);

  std::thread watchdog([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!callbackRan.load()) {
      app.quit();
    }
  });
  int const exitCode = app.exec();
  watchdog.join();

  CHECK(exitCode == 0);
  CHECK(callbackRan.load());
  CHECK(sawWritable.load());
}

TEST_CASE("EventQueue snapshots custom handlers registered during dispatch") {
  lambdaui::Application app;
  lambdaui::EventQueue& queue = app.eventQueue();

  int primaryCalls = 0;
  int addedCalls = 0;
  std::vector<lambdaui::EventSubscription> subscriptions;
  subscriptions.push_back(queue.on<EventQueuePayload>([&](EventQueuePayload const&) {
    ++primaryCalls;
    for (int i = 0; i < 64; ++i) {
      subscriptions.push_back(queue.on<EventQueuePayload>([&](EventQueuePayload const&) {
        ++addedCalls;
      }));
    }
  }));

  queue.post(EventQueuePayload{.value = 1});
  queue.dispatch();
  CHECK(primaryCalls == 1);
  CHECK(addedCalls == 0);

  queue.post(EventQueuePayload{.value = 2});
  queue.dispatch();
  CHECK(primaryCalls == 2);
  CHECK(addedCalls == 64);
}

TEST_CASE("EventQueue subscriptions return listener count to baseline") {
  lambdaui::Application app;
  lambdaui::EventQueue& queue = app.eventQueue();
  std::size_t const baseline =
      lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue);

  int calls = 0;
  {
    std::vector<lambdaui::EventSubscription> subscriptions;
    for (int i = 0; i < 32; ++i) {
      subscriptions.push_back(queue.on<lambdaui::InputEvent>([&](lambdaui::InputEvent const&) {
        ++calls;
      }));
    }
    CHECK(lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue) == baseline + 32);
  }

  CHECK(lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue) == baseline);
  queue.post(lambdaui::InputEvent{.kind = lambdaui::InputEvent::Kind::KeyDown});
  queue.dispatch();
  CHECK(calls == 0);
}

TEST_CASE("EventQueue skips destroyed subscription on later dispatch") {
  lambdaui::Application app;
  lambdaui::EventQueue& queue = app.eventQueue();
  std::size_t const baseline =
      lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue);

  int calls = 0;
  {
    auto subscription = queue.on<lambdaui::InputEvent>([&](lambdaui::InputEvent const&) {
      ++calls;
    });
    CHECK(lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue) == baseline + 1);
  }
  CHECK(lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue) == baseline);

  queue.post(lambdaui::InputEvent{.kind = lambdaui::InputEvent::Kind::KeyDown});
  queue.dispatch();
  CHECK(calls == 0);
  CHECK(lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue) == baseline);
}

TEST_CASE("EventQueue supports unsubscribe during dispatch") {
  lambdaui::Application app;
  lambdaui::EventQueue& queue = app.eventQueue();
  std::size_t const baseline =
      lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue);

  int firstCalls = 0;
  int secondCalls = 0;
  lambdaui::EventSubscription first;
  lambdaui::EventSubscription second;
  first = queue.on<lambdaui::InputEvent>([&](lambdaui::InputEvent const&) {
    ++firstCalls;
    first.reset();
  });
  second = queue.on<lambdaui::InputEvent>([&](lambdaui::InputEvent const&) {
    ++secondCalls;
  });

  queue.post(lambdaui::InputEvent{.kind = lambdaui::InputEvent::Kind::KeyDown});
  queue.dispatch();
  CHECK(firstCalls == 1);
  CHECK(secondCalls == 1);
  CHECK(lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue) == baseline + 1);

  second.reset();
  CHECK(lambdaui::detail::EventQueueImplAccess::liveHandlerCountForTesting(queue) == baseline);
}
