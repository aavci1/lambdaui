#pragma once

/// \file Lambda/Reactive/Easing.hpp
///
/// Part of the Lambda public API.


#include <Lambda/Reactive/SmallFn.hpp>

#include <cstddef>

namespace lambdaui {

using EasingFn = float (*)(float);
using SpringEasingFn = Reactive::SmallFn<float(float), 32>;

namespace Easing {

float linear(float t);
float easeIn(float t);
float easeOut(float t);
float easeInOut(float t);

/// Generates a spring easing function. The returned function is stateless
/// and maps [0,1] → [0,1] by numerically integrating a damped spring.
/// Note: spring output may overshoot 1.0 (that is the intended behaviour).
///
/// `stiffness` and `damping` tune the heuristic integrator; they are not physical SI constants.
/// Integration uses normalized time in [0,1] (aligned with `Transition::duration`), so tuning is empirical.
SpringEasingFn spring(float stiffness = 300.f, float damping = 20.f);

#if defined(LAMBDAUI_TESTING)
std::size_t debugSpringIntegrationStepCount();
void debugResetSpringIntegrationStepCount();
void debugClearSpringCacheForTesting();
#endif

} // namespace Easing
} // namespace lambdaui
