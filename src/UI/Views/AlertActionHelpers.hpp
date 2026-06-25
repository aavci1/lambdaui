#pragma once

#include <Lambda/Reactive/SmallFn.hpp>

#include <functional>
#include <utility>

namespace lambdaui::detail {

inline Reactive::SmallFn<void()> wrapDismissThenInvoke(Reactive::SmallFn<void()> dismiss,
                                                   Reactive::SmallFn<void()> action) {
  return [dismiss = std::move(dismiss), action = std::move(action)]() {
    Reactive::SmallFn<void()> const dismissCopy = dismiss;
    Reactive::SmallFn<void()> const actionCopy = action;
    if (dismissCopy) {
      dismissCopy();
    }
    if (actionCopy) {
      actionCopy();
    }
  };
}

} // namespace lambdaui::detail
