# Reactivity

LambdaUI uses a fine-grained reactive graph under `include/Lambda/Reactive`. The UI layer builds on that graph to update retained scene nodes without remounting entire component trees.

## Namespaces

Core reactive types live in `lambdaui::Reactive`. Common aliases are also exported into `lambdaui` through `Lambda/Reactive/Reactive.hpp`.

For application code that includes `Lambda.hpp`, common names such as `Signal<T>`, `Computed<T>`, `Effect`, `Scope`, `makeComputed`, `withOwner`, `onCleanup`, and `untrack` are available in namespace `lambdaui`.

## Primitives

- `Signal<T>` stores mutable state and notifies dependents when its value changes.
- `Computed<T>` derives a value from tracked reads.
- `Effect` runs side-effecting code and reruns when tracked dependencies change.
- `Scope` owns effects, child scopes, and cleanup callbacks.
- `Bindable<T>` stores either a constant value or a closure used by mounted bindings.
- `Animated<T>` and `Transition` provide interpolated values driven by `AnimationClock`.

## Tracking

Reactive dependencies are captured when a signal is read during a computed or effect run:

```cpp
auto count = Signal<int>{0};
auto doubled = makeComputed([count] {
  return count() * 2;
});
```

Use:

- `signal()` or `signal.get()` for tracking reads.
- `signal.peek()` for a deliberate non-tracking read.
- `untrack(fn)` to run a block without subscribing to reads.

`Signal::set` only propagates when the value changes.

## Effects

Effects rerun when tracked dependencies change:

```cpp
useEffect([count] {
  std::printf("count = %d\n", count());
});
```

Effects created during UI mounting are owned by the active mount scope. When a component, branch, or row is unmounted, its scope disposes effects and runs cleanup callbacks.

## Scopes And Cleanup

Scopes are the lifetime backbone of the retained UI model:

- A mounted root owns a root scope.
- Component mounts create child scopes.
- `For`, `Show`, and `Switch` create row and branch scopes.
- Disposing a scope disposes nested scopes in reverse ownership order.

Use `onCleanup` for external resources:

```cpp
useEffect([subscription] {
  subscription.start();
  Reactive::onCleanup([subscription] {
    subscription.stop();
  });
});
```

## Bindables In UI Code

`Bindable<T>` lets views accept either a constant or reactive value. During mount, LambdaUI installs an effect that applies the resolved value to the retained scene node:

```cpp
auto width = useState(120.f);

return Rectangle{}
    .size([width] { return width(); }, 24.f)
    .fill(Color::accent());
```

This is the normal path for changing colors, text, opacity, size, transforms, and other mounted node properties without rebuilding a subtree.

## Computed Values

Use `useComputed` when derived state is shared by multiple bindings or effects:

```cpp
auto first = useState(std::string{"Ada"});
auto last = useState(std::string{"Lovelace"});
auto fullName = useComputed([first, last] {
  return first() + " " + last();
});
```

Keep computed functions pure. Put side effects in `useEffect`.

## Animation

Use `useAnimated` for interpolated values:

```cpp
auto expanded = useState(false);
auto height = useAnimated(
    [expanded] { return expanded() ? 240.f : 80.f; },
    Transition{.duration = std::chrono::milliseconds{180}});
```

Use `useFrame` for custom per-frame work that is not simply interpolating one value. Prefer `useAnimated` for normal control transitions because it scopes ownership and frame subscriptions cleanly.

## Environment Reactivity

Environment values can be static or signal-backed. `useEnvironment<Key>()` always returns a signal-shaped handle:

```cpp
auto theme = useEnvironment<ThemeKey>();

return Text{
    .text = "Status",
    .color = [theme] { return theme().labelColor; },
};
```

Reading `theme()` inside a bindable closure or effect subscribes to updates. Reading it directly during body construction is a one-time mount-time read.

## Profiling

Set `LAMBDAUI_PROFILE_REACTIVE=ON` to compile deterministic reactive profiling counters and display-link reports. This is intended for targeted performance work, not normal application builds.
