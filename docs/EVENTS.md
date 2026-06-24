# Event Queue

The event queue is the central dispatch path for application-visible work: window lifecycle, window events, input, timer ticks, and user-defined payloads.

There is one queue per `Application`, owned by the application and accessed through:

```cpp
Application::instance().eventQueue();
app.eventQueue();
```

## Design Goals

- Unified framework and custom event delivery.
- Defined bucket order for each dispatch pass.
- Batched draining until pending buckets are empty.
- Main-thread contract for `post`, `dispatch`, and `on`.

## Event Types

`Event` is a `std::variant` of:

```cpp
WindowLifecycleEvent
WindowEvent
InputEvent
TimerEvent
CustomEvent
```

`CustomEvent` stores a type id plus `std::any` payload. `EventQueue::post<T>()` and `EventQueue::on<T>()` map non-framework payload types to custom events.

## Queue API

```cpp
void post(Event event);

template<typename T>
void post(T&& value);

template<typename T>
void on(std::function<void(T const&)> handler);

void dispatch();
```

Only `Application` constructs the queue.

## Dispatch Order

Pending events drain in this bucket order:

| Priority | Event type |
| --- | --- |
| 1 | `WindowLifecycleEvent` |
| 2 | `WindowEvent` |
| 3 | `InputEvent` |
| 4 | `TimerEvent` |
| 5 | `CustomEvent` |

Within each bucket, order is FIFO.

## Threading

LambdaUI expects `post`, `dispatch`, and `on` to be called on the application main thread.

Background systems should marshal results to the UI thread before posting custom events. The event queue is not a general cross-thread synchronization primitive.

## Re-entrancy

`dispatch()` has a guard. Nested dispatch calls return immediately. Events posted by a handler remain queued and are processed by the outer dispatch pass.

## Timers

`Application::scheduleRepeatingTimer` registers a repeating steady-clock timer. Timer ticks post `TimerEvent` values and are dispatched through the queue.

Use `Application::cancelTimer` with the returned timer id to stop the timer. Scope-owned UI code should register cleanup when it owns a timer.

## Platform Loop Integration

`Application::exec()` alternates platform event waits, due-timer processing, reactive and next-frame work, redraw presentation, and `EventQueue::dispatch()`.

Platform callbacks may post events for the next dispatch pass. Some synchronous setup/teardown paths may post and dispatch immediately when the platform integration requires it.

## Custom Payload Example

```cpp
struct DataReadyEvent {
  User user;
};

app.eventQueue().on<DataReadyEvent>([&](DataReadyEvent const& event) {
  updateUser(event.user);
});

app.eventQueue().post(DataReadyEvent{user});
```

## Useful Files

- `include/Lambda/UI/EventQueue.hpp`
- `src/UI/EventQueue.cpp`
- `include/Lambda/UI/Events.hpp`
- `include/Lambda/UI/Application.hpp`
