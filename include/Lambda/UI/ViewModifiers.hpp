#pragma once

#include <Lambda/Reactive/SmallFn.hpp>

/// \file Lambda/UI/ViewModifiers.hpp
///
/// CRTP base providing modifier methods on all view types, removing the need for
/// explicit Element{...} wrapping before calling modifiers.
///
/// Inline bodies live in `Lambda/UI/Detail/ViewModifierInlines.hpp` and require `Element` to be complete.

#include <Lambda/UI/Cursor.hpp>
#include <Lambda/UI/Input.hpp>
#include <Lambda/UI/WindowChrome.hpp>
#include <Lambda/Core/Geometry.hpp>
#include <Lambda/Core/Color.hpp>
#include <Lambda/Graphics/Styles.hpp>
#include <Lambda/Reactive/Bindable.hpp>
#include <Lambda/UI/Environment.hpp>

#include <cstddef>
#include <functional>
#include <string>

namespace lambdaui {

class Element;

/// CRTP base that gives every view struct chained modifier methods.
/// Each method constructs an \ref Element from the derived view and delegates to Element's modifier.
template<typename Derived>
struct ViewModifiers {
  bool operator==(ViewModifiers const&) const noexcept = default;

  Element padding(Reactive::Bindable<float> all) &&;
  Element padding(Reactive::Bindable<EdgeInsets> insets) &&;
  Element padding(Reactive::Bindable<float> top, Reactive::Bindable<float> right,
                  Reactive::Bindable<float> bottom, Reactive::Bindable<float> left) &&;
  Element fill(Reactive::Bindable<FillStyle> style) &&;
  Element fill(Reactive::Bindable<Color> color) &&;
  Element shadow(Reactive::Bindable<ShadowStyle> style) &&;
  Element size(Reactive::Bindable<float> width, Reactive::Bindable<float> height) &&;
  Element width(Reactive::Bindable<float> w) &&;
  Element height(Reactive::Bindable<float> h) &&;
  Element stroke(Reactive::Bindable<StrokeStyle> style) &&;
  Element stroke(Reactive::Bindable<Color> color, Reactive::Bindable<float> width) &&;
  Element cornerRadius(Reactive::Bindable<CornerRadius> radius) &&;
  Element cornerRadius(Reactive::Bindable<float> radius) &&;
  Element opacity(Reactive::Bindable<float> opacity) &&;
  Element position(Reactive::Bindable<Vec2> p) &&;
  Element position(Reactive::Bindable<float> x, Reactive::Bindable<float> y) &&;
  Element translate(Reactive::Bindable<Vec2> delta) &&;
  Element translate(Reactive::Bindable<float> dx, Reactive::Bindable<float> dy) &&;
  Element rotate(Reactive::Bindable<float> radians) &&;
  Element scale(Reactive::Bindable<float> factor) &&;
  Element scale(Reactive::Bindable<Vec2> factors) &&;
  Element scale(Reactive::Bindable<float> sx, Reactive::Bindable<float> sy) &&;
  Element clipContent(bool clip) &&;
  /// Renders this subtree into an offscreen texture and reuses that texture until the subtree,
  /// bounds, or DPI scale changes.
  Element rasterize() &&;
  template<typename T>
  Element rasterizeInvalidateOn(Reactive::Bindable<T> binding) &&;
  Element overlay(Element over) &&;
  Element key(std::string key) &&;

  Element onTap(Reactive::SmallFn<void()> handler, MouseButton button = MouseButton::Left) &&;
  Element onTap(Reactive::SmallFn<void(MouseButton)> handler) &&;
  Element onTap(Reactive::SmallFn<void(MouseButton, Modifiers)> handler) &&;
  Element onPointerEnter(Reactive::SmallFn<void()> handler) &&;
  Element onPointerExit(Reactive::SmallFn<void()> handler) &&;
  Element onFocus(Reactive::SmallFn<void()> handler) &&;
  Element onBlur(Reactive::SmallFn<void()> handler) &&;
  Element onPointerDown(Reactive::SmallFn<void(Point)> handler, MouseButton button = MouseButton::Left) &&;
  Element onPointerDown(Reactive::SmallFn<void(Point, MouseButton)> handler) &&;
  Element onPointerUp(Reactive::SmallFn<void(Point)> handler, MouseButton button = MouseButton::Left) &&;
  Element onPointerUp(Reactive::SmallFn<void(Point, MouseButton)> handler) &&;
  Element onPointerMove(Reactive::SmallFn<void(Point)> handler) &&;
  Element onScroll(Reactive::SmallFn<void(Vec2)> handler) &&;
  Element onKeyDown(Reactive::SmallFn<void(KeyCode, Modifiers)> handler) &&;
  Element onKeyUp(Reactive::SmallFn<void(KeyCode, Modifiers)> handler) &&;
  Element onTextInput(Reactive::SmallFn<void(std::string const&)> handler) &&;
  Element focusable(bool enabled) &&;
  Element focusable(Reactive::Bindable<bool> enabled) &&;
  Element cursor(Cursor c) &&;
  Element cursor(Reactive::Bindable<Cursor> c) &&;
  Element windowDragRegion(bool enabled = true) &&;
  Element windowResizeRegion(WindowResizeEdge edge) &&;

  Element flex(float grow) &&;
  Element flex(float grow, float shrink) &&;
  Element flex(float grow, float shrink, float basis) &&;
  Element minMainSize(float size) &&;
  Element colSpan(std::size_t span) &&;
  Element rowSpan(std::size_t span) &&;
  template<typename Key>
  Element environment(typename EnvironmentKey<Key>::Value value) &&;

  template<typename Key>
  Element environment(Reactive::Signal<typename EnvironmentKey<Key>::Value> signal) &&;
};

} // namespace lambdaui
