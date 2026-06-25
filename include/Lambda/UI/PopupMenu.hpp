#pragma once

#include <Lambda/Reactive/SmallFn.hpp>

/// \file Lambda/UI/PopupMenu.hpp
///
/// Native/platform popup menu presentation.

#include <Lambda/UI/MenuItem.hpp>

#include <functional>

namespace lambdaui {

/// Hook: returns a function that presents a platform popup menu anchored to the last tapped element.
///
/// The returned function is safe to call from the tap handler that opened the menu. On platforms with
/// transient popup primitives, this uses the native/Wayland popup building block instead of Lambda overlays.
Reactive::SmallFn<bool(PopupMenu)> usePopupMenu();

} // namespace lambdaui
