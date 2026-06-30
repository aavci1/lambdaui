#pragma once

/// \file Lambda/Config.hpp
///
/// Public compile-time backend configuration exported by the LambdaUI CMake target.

#ifndef LAMBDAUI_NATIVE_RENDERERS
#define LAMBDAUI_NATIVE_RENDERERS 0
#endif

#ifndef LAMBDAUI_METAL
#define LAMBDAUI_METAL 0
#endif

#ifndef LAMBDAUI_VULKAN
#define LAMBDAUI_VULKAN 0
#endif

#ifndef LAMBDAUI_WEBGPU
#define LAMBDAUI_WEBGPU 0
#endif
