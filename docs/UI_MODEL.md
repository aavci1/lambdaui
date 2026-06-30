# UI Model

LambdaUI uses retained mounting with fine-grained reactive updates. Components describe a subtree once for a mount cycle; signals, bindables, effects, and control-flow views update retained nodes after that.

## Component Contract

A normal component exposes a body:

```cpp
struct CounterView {
  Element body() const {
    auto count = useState(0);

    return VStack{
        .children = children(
            Text{.text = [count] { return std::to_string(count()); }},
            Button{
                .label = "Increment",
                .onTap = [count] { count.set(count.peek() + 1); },
            }),
    };
  }
};
```

Supported component and view shapes include:

- `auto body() const` or `Element body() const` for declarative view trees.
- `measure(...)` for custom layout measurement.
- `mount(MountContext&)` for custom retained scene-node construction.

Prefer body-based components unless a view needs specialized measurement or direct scene-node ownership.

## Retained Mounting

The retained scene tree is the identity layer. LambdaUI does not rerun arbitrary component bodies for every signal change.

Instead:

- `Bindable<T>` modifier values install effects against mounted scene nodes.
- `For` keeps keyed row scopes alive across reorder and disposes removed rows.
- `Show` and `Switch` own branch scopes and replace only the active branch subtree.
- Environment values can be static or signal-backed.
- Effects and cleanups are owned by the active mount scope.

Use bindable modifiers when the shape of the subtree stays the same. Use control-flow views when the shape changes.

## Hooks

Hooks require an active reactive owner scope. Use them while a component body or control-flow factory is being mounted.

- `useState(initial)`: create local `Signal<T>` state.
- `useComputed(fn)`: create a derived value.
- `useEffect(fn)`: run an effect and rerun it when tracked dependencies change.
- `useAnimated(...)`: create an interpolated animated value.
- `useFrame(fn)`: subscribe to frame ticks for custom frame work.
- `useEnvironment<Key>()`: read an environment value as a signal-shaped handle.
- `useHover()`, `usePress()`, `useFocus()`, `useKeyboardFocus()`: read interaction state for the current mounted scope.
- `useAutoFocus(generation)`: request focus after layout.
- `useViewCommand`, `useWindowCommand`, and action aliases: register scoped command handlers.
- `useLayoutConstraints()` and `useBounds()`: inspect active layout constraints during mount/measure-related work.

Register external resource cleanup with `Reactive::onCleanup`.

## Bindables

Many view fields and modifiers accept either constants or closures. Closures are tracked reactively when mounted:

```cpp
auto active = useState(false);

return Rectangle{}
    .size(32.f, 32.f)
    .fill([active] {
      return active() ? Color::accent() : Color::separator();
    })
    .cornerRadius(8.f)
    .onTap([active] { active.set(!active.peek()); });
```

Use `signal()` or `signal.get()` inside reactive reads. Use `signal.peek()` only when intentionally reading without subscribing.

## Control Flow

Use control-flow views when the retained subtree shape changes:

- `For(itemsSignal, keyFn, rowFactory)` for keyed lists.
- `Show(condition, thenFactory, elseFactory)` for binary branches.
- `Switch(selector, cases)` for multi-branch selection.

Factories get their own scopes. Row and branch state lasts exactly as long as the retained row or branch exists.

## Environment

Environment values are explicit compile-time keys:

```cpp
LAMBDA_DEFINE_ENVIRONMENT_KEY(LocaleKey, Locale, Locale{});
```

Use:

- `useEnvironment<Key>()` to read a signal-shaped value.
- `.environment<Key>(value)` for a static subtree override.
- `.environment<Key>(signal)` for a signal-backed subtree override.

The built-in `ThemeKey` is signal-backed by `Window`. Calling `Window::setTheme()` updates retained theme-dependent bindings without remounting the app.

## Built-in Views

The broad include for built-in views is:

```cpp
#include <Lambda/UI/Views/Views.hpp>
```

Common categories:

- Leaf visuals: `Text`, `Rectangle`, `Image`, `Svg`, `PathShape`, `Icon`, `Render`.
- Layout: `VStack`, `HStack`, `ZStack`, `Grid`, `Spacer`, `Divider`, `ScrollView`, `ListView`.
- Controls: `Button`, `IconButton`, `LinkButton`, `Checkbox`, `Toggle`, `Slider`, `SegmentedControl`, `Select`, `TextInput`, `TableView`.
- Surfaces and feedback: `Card`, `Dialog`, `Alert`, `Popover`, `Tooltip`, `Toast`, `Badge`, `ProgressBar`.
- Control flow: `For`, `Show`, `Switch`.

The demos under `demos/` are the best place to see each control in context.

## Interaction

View modifiers can attach pointer, scroll, focus, keyboard, cursor, and window-region behavior. The runtime converts platform input into retained interaction updates and callbacks.

Interaction state that should affect visuals is usually best modeled as hook signals:

```cpp
auto hovered = useHover();

return Button{
    .label = [hovered] { return hovered() ? "Ready" : "Hover me"; },
};
```

For direct pointer handling, use modifiers such as `onPointerDown`, `onPointerMove`, `onPointerUp`, `onTap`, and `onScroll` on elements or views that support modifiers.

## Commands And Menus

Application-level commands are registered on `Application`. Window and view commands can be registered from mounted scopes with hooks. Command handlers are cleaned up with their owning scope.

Use command hooks when the enabled state or handler lifetime belongs to a view. Use application command registration for process-level commands and menus.

## Direct Scene Graph Use

Most applications should use views. Direct scene-graph work is useful for custom rendering tools, specialized controls, and low-level demos.

Entry points:

- `include/Lambda/SceneGraph/SceneNode.hpp`
- `include/Lambda/SceneGraph/SceneGraph.hpp`
- `include/Lambda/SceneGraph/RectNode.hpp`
- `include/Lambda/SceneGraph/TextNode.hpp`
- `include/Lambda/SceneGraph/ImageNode.hpp`
- `include/Lambda/SceneGraph/PathNode.hpp`
- `demos/scene-graph-demo`
