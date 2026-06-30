# Style Guide

This guide captures project conventions that help future contributors and agents make changes that fit the existing codebase.

## General Source Style

- Use C++23.
- Prefer existing local abstractions over new helper layers.
- Keep public API in `include/Lambda/...` and implementation in the matching `src/...` module.
- Keep platform-specific code in the existing platform folders.
- Add comments only when they explain non-obvious ownership, lifetime, platform, or performance constraints.
- Prefer targeted tests for behavior changes.

## Module Boundaries

The broad layering is:

```text
Core
Reactive
Graphics
SceneGraph
Layout
UI
Platform integrations
```

Lower modules should not depend on higher UI concepts. When adding a feature, start from the lowest module that actually owns the behavior.

Examples:

- A new control belongs in `UI/Views`.
- A new draw primitive usually touches `Graphics`, `SceneGraph`, and the WebGPU canvas.
- A new reactive primitive belongs in `Reactive`, not `UI`.
- A platform event quirk belongs under `src/Platform/...`, with normalized behavior exposed to `UI/Runtime`.

## Component Body Formatting

Use these conventions for `Element body() const` or `auto body() const` members that return LambdaUI view trees.

- Use 2 spaces per nesting level.
- Put `return ViewType{` one indent inside `body()`.
- Put designated-initializer fields one indent deeper than the view opener.
- Inside `.children = children(...)`, put each sibling on its own line when the list is not trivial.
- Prefer `ViewType{}` designated-initializer trees.
- Use one `.member = value` per line in multi-line initializers.
- Keep short nested aggregates on one line when readable.
- Trailing commas are fine in multi-line initializers.

Example:

```cpp
struct StatusCard {
  Signal<bool> connected;

  auto body() const {
    auto theme = useEnvironment<ThemeKey>();

    return Card{
        .children = children(
            Text{
                .text = [connected] {
                  return connected() ? "Connected" : "Offline";
                },
                .color = [theme] { return theme().labelColor; },
            },
            Button{
                .label = "Retry",
                .variant = ButtonVariant::Secondary,
                .onTap = [] { reconnect(); },
            }),
    };
  }
};
```

## Layout Structure

- Use `VStack`, `HStack`, and `ZStack` for common layout.
- Use `Grid` for repeated two-dimensional structure.
- Use `ScrollView` when content can exceed the viewport.
- Use `For`, `Show`, and `Switch` when subtree shape changes reactively.
- Use `Element{...}` only when a composite needs explicit type erasure.

## Modifiers

Views that support modifiers can chain calls:

```cpp
Text{
    .text = "Ready",
    .font = Font::headline(),
    .color = Color::primary(),
}.padding(12.f)
 .fill(Color::controlBackground())
 .cornerRadius(8.f);
```

Use `Bindable<T>` overloads when a mounted scene-node property should update after mount.

## Includes

Small apps and demos commonly include:

```cpp
#include <Lambda.hpp>
#include <Lambda/UI/UI.hpp>
#include <Lambda/UI/Views/Views.hpp>
```

Library code and larger apps should include narrower headers where practical.

Suggested order:

1. Umbrella or module headers.
2. Specific LambdaUI headers.
3. Platform/system headers.
4. Standard library headers.

Follow nearby files when editing existing code.

## Tests

Add or update tests when changing:

- Reactive graph behavior.
- Mounting, cleanup, or control-flow lifetime.
- Input routing, focus, or event dispatch.
- Layout measurement or scene traversal.
- Text editing, text layout, path/SVG parsing, or render-target behavior.
- Platform-independent rendering contracts.

Use existing focused test files as the first place to extend coverage. Create a new test file when the behavior does not fit an existing area.

## Demos

Update demos when adding or materially changing user-facing controls. A good demo should show the normal state, disabled or edge states, and at least one interactive path.

## Documentation

Docs should be current, navigable, and durable.

- Keep the root `README.md` as the GitHub entry point.
- Keep `docs/README.md` as the docs index.
- Prefer conventional uppercase topic names in `docs/`, such as `ARCHITECTURE.md`.
- Remove stale working notes, dated review reports, and obsolete benchmark logs.
- When recording a temporary investigation, keep it outside `docs/` unless it has been rewritten as current guidance.
