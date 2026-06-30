set(LAMBDAUI_DAWN_SOURCE_DIR "" CACHE PATH
  "Optional path to a Dawn source checkout. When empty, LambdaUI uses find_package(Dawn).")
option(LAMBDAUI_DAWN_FETCH
  "Fetch Dawn from source with CMake FetchContent when no installed package or source checkout is provided."
  OFF)
set(LAMBDAUI_DAWN_GIT_REPOSITORY "https://dawn.googlesource.com/dawn" CACHE STRING
  "Git repository used when LAMBDAUI_DAWN_FETCH is enabled.")
set(LAMBDAUI_DAWN_GIT_TAG "main" CACHE STRING
  "Git tag, branch, or commit used when LAMBDAUI_DAWN_FETCH is enabled.")

function(lambda_configure_dawn_subproject)
  set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "Fetch Dawn third-party dependencies." FORCE)
  set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "Build Dawn samples." FORCE)
  set(DAWN_BUILD_TESTS OFF CACHE BOOL "Build Dawn tests." FORCE)
  set(DAWN_BUILD_BENCHMARKS OFF CACHE BOOL "Build Dawn benchmarks." FORCE)
  set(DAWN_BUILD_NODE_BINDINGS OFF CACHE BOOL "Build Dawn NodeJS bindings." FORCE)
  set(DAWN_BUILD_FUZZERS OFF CACHE BOOL "Build Dawn fuzzers." FORCE)
  set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "Build Dawn protobuf dependencies." FORCE)
  set(DAWN_ENABLE_INSTALL OFF CACHE BOOL "Install Dawn from the LambdaUI build." FORCE)
  set(DAWN_ENABLE_NULL OFF CACHE BOOL "Enable Dawn's null backend." FORCE)
  set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "Enable Dawn's D3D11 backend." FORCE)
  set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "Enable Dawn's D3D12 backend." FORCE)
  set(DAWN_ENABLE_METAL OFF CACHE BOOL "Enable Dawn's Metal backend." FORCE)
  set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "Enable Dawn's Vulkan backend." FORCE)
  set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "Enable Dawn's desktop OpenGL backend." FORCE)
  set(DAWN_ENABLE_OPENGLES OFF CACHE BOOL "Enable Dawn's OpenGL ES backend." FORCE)
  set(DAWN_ENABLE_WEBGPU_ON_WEBGPU OFF CACHE BOOL "Enable Dawn's WebGPU-on-WebGPU backend." FORCE)
  set(DAWN_ENABLE_SWIFTSHADER OFF CACHE BOOL "Build SwiftShader as part of Dawn." FORCE)
  set(DAWN_USE_GLFW OFF CACHE BOOL "Build Dawn GLFW helpers." FORCE)
  set(DAWN_USE_WAYLAND OFF CACHE BOOL "Enable Dawn Wayland surface support." FORCE)
  set(DAWN_USE_X11 OFF CACHE BOOL "Enable Dawn X11 surface support." FORCE)
  set(DAWN_USE_WINDOWS_UI OFF CACHE BOOL "Enable Dawn Windows UI surface support." FORCE)
  set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "Build Tint command line tools." FORCE)
  set(TINT_BUILD_TESTS OFF CACHE BOOL "Build Tint tests." FORCE)
  set(TINT_BUILD_BENCHMARKS OFF CACHE BOOL "Build Tint benchmarks." FORCE)
  set(TINT_BUILD_FUZZERS OFF CACHE BOOL "Build Tint fuzzers." FORCE)
  set(TINT_BUILD_GLSL_VALIDATOR OFF CACHE BOOL "Build Tint GLSL validator." FORCE)

  if(LAMBDAUI_PLATFORM_MACOS)
    set(DAWN_ENABLE_METAL ON CACHE BOOL "Enable Dawn's Metal backend." FORCE)
    set(DAWN_TARGET_MACOS ON CACHE BOOL "Link Dawn's macOS platform frameworks." FORCE)
  elseif(LAMBDAUI_PLATFORM_LINUX_WAYLAND)
    set(DAWN_ENABLE_VULKAN ON CACHE BOOL "Enable Dawn's Vulkan backend." FORCE)
    set(DAWN_USE_WAYLAND ON CACHE BOOL "Enable Dawn Wayland surface support." FORCE)
  endif()
endfunction()

macro(lambda_push_dawn_compile_workarounds)
  set(_lambda_dawn_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    string(APPEND CMAKE_CXX_FLAGS " -include cstdint")
  endif()
endmacro()

macro(lambda_pop_dawn_compile_workarounds)
  set(CMAKE_CXX_FLAGS "${_lambda_dawn_saved_cxx_flags}")
  unset(_lambda_dawn_saved_cxx_flags)
endmacro()

if(LAMBDAUI_DAWN_SOURCE_DIR)
  if(NOT EXISTS "${LAMBDAUI_DAWN_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "LAMBDAUI_DAWN_SOURCE_DIR must point at a Dawn source checkout containing CMakeLists.txt.")
  endif()
  lambda_configure_dawn_subproject()
  lambda_push_dawn_compile_workarounds()
  add_subdirectory("${LAMBDAUI_DAWN_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/dawn-build" EXCLUDE_FROM_ALL)
  lambda_pop_dawn_compile_workarounds()
elseif(LAMBDAUI_DAWN_FETCH)
  include(FetchContent)
  lambda_configure_dawn_subproject()
  FetchContent_Declare(lambda_dawn
    GIT_REPOSITORY "${LAMBDAUI_DAWN_GIT_REPOSITORY}"
    GIT_TAG "${LAMBDAUI_DAWN_GIT_TAG}"
    GIT_PROGRESS TRUE
    GIT_SUBMODULES "")
  lambda_push_dawn_compile_workarounds()
  FetchContent_MakeAvailable(lambda_dawn)
  lambda_pop_dawn_compile_workarounds()
elseif(NOT TARGET dawn::webgpu_dawn)
  find_package(Dawn CONFIG QUIET)
endif()

if(NOT TARGET dawn::webgpu_dawn AND TARGET webgpu_dawn)
  add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
endif()

if(NOT TARGET dawn::webgpu_dawn)
  find_path(LAMBDAUI_DAWN_INCLUDE_DIR
    NAMES webgpu/webgpu.h dawn/webgpu.h
    HINTS /opt/homebrew /usr/local
    PATH_SUFFIXES include)
  find_library(LAMBDAUI_DAWN_NATIVE_LIBRARY
    NAMES dawn_native
    HINTS /opt/homebrew /usr/local
    PATH_SUFFIXES lib)
  find_library(LAMBDAUI_DAWN_PROC_LIBRARY
    NAMES dawn_proc
    HINTS /opt/homebrew /usr/local
    PATH_SUFFIXES lib)

  if(LAMBDAUI_DAWN_INCLUDE_DIR AND LAMBDAUI_DAWN_NATIVE_LIBRARY AND LAMBDAUI_DAWN_PROC_LIBRARY)
    add_library(lambda_dawn_homebrew INTERFACE)
    target_include_directories(lambda_dawn_homebrew INTERFACE "${LAMBDAUI_DAWN_INCLUDE_DIR}")
    target_link_libraries(lambda_dawn_homebrew INTERFACE
      "${LAMBDAUI_DAWN_NATIVE_LIBRARY}"
      "${LAMBDAUI_DAWN_PROC_LIBRARY}")
    target_compile_definitions(lambda_dawn_homebrew INTERFACE LAMBDAUI_DAWN_LEGACY_NATIVE=1)
    add_library(dawn::webgpu_dawn ALIAS lambda_dawn_homebrew)
  endif()
endif()

if(NOT TARGET dawn::webgpu_dawn)
  message(FATAL_ERROR
    "LambdaUI WebGPU builds require Dawn's CMake package target dawn::webgpu_dawn. "
    "Build and install Dawn with -DDAWN_ENABLE_INSTALL=ON and pass its install prefix via "
    "CMAKE_PREFIX_PATH, pass a Dawn checkout with -DLAMBDAUI_DAWN_SOURCE_DIR=/path/to/dawn, "
    "or let LambdaUI fetch it with -DLAMBDAUI_DAWN_FETCH=ON.")
endif()
