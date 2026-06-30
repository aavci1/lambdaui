cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.0")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(lambdaui VERSION 5.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(FetchContent)

set(LAMBDAUI_FRAMEWORK_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(LAMBDAUI_FRAMEWORK_INCLUDE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/include")
set(LAMBDAUI_FRAMEWORK_SOURCE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/src")
set(LAMBDAUI_FRAMEWORK_CMAKE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/cmake")
set(LAMBDAUI_FRAMEWORK_RESOURCE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/resources")
set(LAMBDAUI_FRAMEWORK_PROTOCOL_DIR "${LAMBDAUI_FRAMEWORK_DIR}/protocols")
set(LAMBDAUI_FRAMEWORK_VENDOR_DIR "${LAMBDAUI_FRAMEWORK_DIR}/vendor")

include(${LAMBDAUI_FRAMEWORK_CMAKE_DIR}/LambdaApp.cmake)

set(LAMBDAUI_PLATFORM "AUTO" CACHE STRING
    "LambdaUI backend: AUTO, MACOS, LINUX_WAYLAND.")
set_property(CACHE LAMBDAUI_PLATFORM PROPERTY STRINGS AUTO MACOS LINUX_WAYLAND)

set(LAMBDAUI_DAWN_SOURCE_DIR "" CACHE PATH
  "Optional path to a Dawn source checkout. When empty, LambdaUI uses find_package(Dawn).")
option(LAMBDAUI_DAWN_FETCH
  "Fetch Dawn from source with CMake FetchContent when no installed package or source checkout is provided."
  OFF)
set(LAMBDAUI_DAWN_GIT_REPOSITORY "https://dawn.googlesource.com/dawn" CACHE STRING
  "Git repository used when LAMBDAUI_DAWN_FETCH is enabled.")
set(LAMBDAUI_DAWN_GIT_TAG "main" CACHE STRING
  "Git tag, branch, or commit used when LAMBDAUI_DAWN_FETCH is enabled.")

set(LAMBDAUI_RENDERER "AUTO" CACHE STRING
    "LambdaUI renderer: AUTO, NATIVE or WEBGPU.")
set_property(CACHE LAMBDAUI_RENDERER PROPERTY STRINGS AUTO NATIVE WEBGPU)
option(LAMBDAUI_ENABLE_NATIVE_RENDERERS
  "Opt in to the legacy Metal/Vulkan renderers while Dawn/WebGPU replaces the native backends."
  OFF)
set(LAMBDAUI_RENDERER_NATIVE OFF)
set(LAMBDAUI_RENDERER_WEBGPU OFF)
if(LAMBDAUI_RENDERER STREQUAL "AUTO")
  if(LAMBDAUI_DAWN_SOURCE_DIR OR LAMBDAUI_DAWN_FETCH)
    set(LAMBDAUI_RENDERER_WEBGPU ON)
    message(STATUS "LambdaUI: LAMBDAUI_RENDERER=AUTO selected WEBGPU because Dawn is explicitly configured.")
  else()
    find_package(Dawn CONFIG QUIET)
    if(NOT TARGET dawn::webgpu_dawn AND TARGET webgpu_dawn)
      add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
    endif()
    if(TARGET dawn::webgpu_dawn)
      set(LAMBDAUI_RENDERER_WEBGPU ON)
      message(STATUS "LambdaUI: LAMBDAUI_RENDERER=AUTO selected WEBGPU because Dawn was found.")
    elseif(LAMBDAUI_ENABLE_NATIVE_RENDERERS)
      set(LAMBDAUI_RENDERER_NATIVE ON)
      message(STATUS "LambdaUI: LAMBDAUI_RENDERER=AUTO selected NATIVE because Dawn was not found.")
    else()
      message(FATAL_ERROR
        "LambdaUI: LAMBDAUI_RENDERER=AUTO could not select WEBGPU because Dawn was not found "
        "and legacy native renderers are disabled. Configure Dawn with CMAKE_PREFIX_PATH, "
        "LAMBDAUI_DAWN_SOURCE_DIR, or LAMBDAUI_DAWN_FETCH=ON; or opt in to the legacy "
        "Metal/Vulkan renderers with LAMBDAUI_ENABLE_NATIVE_RENDERERS=ON.")
    endif()
  endif()
elseif(LAMBDAUI_RENDERER STREQUAL "NATIVE")
  if(NOT LAMBDAUI_ENABLE_NATIVE_RENDERERS)
    message(FATAL_ERROR
      "LambdaUI: LAMBDAUI_RENDERER=NATIVE requires LAMBDAUI_ENABLE_NATIVE_RENDERERS=ON.")
  endif()
  set(LAMBDAUI_RENDERER_NATIVE ON)
elseif(LAMBDAUI_RENDERER STREQUAL "WEBGPU")
  set(LAMBDAUI_RENDERER_WEBGPU ON)
else()
  message(FATAL_ERROR "LambdaUI: LAMBDAUI_RENDERER=${LAMBDAUI_RENDERER} is not implemented yet.")
endif()

if(LAMBDAUI_PLATFORM STREQUAL "AUTO")
  if(APPLE)
    set(LAMBDAUI_PLATFORM_MACOS ON)
    set(LAMBDAUI_PLATFORM "MACOS" CACHE STRING
      "LambdaUI backend: AUTO, MACOS, LINUX_WAYLAND." FORCE)
  elseif(UNIX)
    set(LAMBDAUI_PLATFORM_LINUX_WAYLAND ON)
    set(LAMBDAUI_PLATFORM "LINUX_WAYLAND" CACHE STRING
      "LambdaUI backend: AUTO, MACOS, LINUX_WAYLAND." FORCE)
  else()
    message(FATAL_ERROR "LambdaUI: no default backend for this host.")
  endif()
elseif(LAMBDAUI_PLATFORM STREQUAL "MACOS")
  if(NOT APPLE)
    message(FATAL_ERROR "LAMBDAUI_PLATFORM=MACOS requires an Apple host.")
  endif()
  set(LAMBDAUI_PLATFORM_MACOS ON)
elseif(LAMBDAUI_PLATFORM STREQUAL "LINUX_WAYLAND")
  if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "LAMBDAUI_PLATFORM=LINUX_WAYLAND requires a Linux/Unix host.")
  endif()
  set(LAMBDAUI_PLATFORM_LINUX_WAYLAND ON)
else()
  message(FATAL_ERROR "LambdaUI: LAMBDAUI_PLATFORM=${LAMBDAUI_PLATFORM} is not implemented yet.")
endif()

if(LAMBDAUI_PLATFORM_MACOS)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

if(LAMBDAUI_PLATFORM_MACOS AND LAMBDAUI_RENDERER_NATIVE)
  find_program(LAMBDAUI_XCRUN_EXECUTABLE xcrun)
  if(NOT LAMBDAUI_XCRUN_EXECUTABLE)
    message(FATAL_ERROR
      "LambdaUI macOS builds require xcrun. Install Xcode from the App Store or Apple Developer, "
      "then select it with: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer")
  endif()

  execute_process(
    COMMAND ${LAMBDAUI_XCRUN_EXECUTABLE} -sdk macosx -find metal
    RESULT_VARIABLE LAMBDAUI_METAL_FIND_RESULT
    OUTPUT_VARIABLE LAMBDAUI_METAL_FIND_OUTPUT
    ERROR_VARIABLE LAMBDAUI_METAL_FIND_ERROR
  )
  if(NOT LAMBDAUI_METAL_FIND_RESULT EQUAL 0)
    string(STRIP "${LAMBDAUI_METAL_FIND_OUTPUT}" LAMBDAUI_METAL_FIND_OUTPUT)
    string(STRIP "${LAMBDAUI_METAL_FIND_ERROR}" LAMBDAUI_METAL_FIND_ERROR)
    message(FATAL_ERROR
      "LambdaUI macOS builds require the Metal shader compiler (`xcrun -sdk macosx metal`). "
      "Install full Xcode, not only the Command Line Tools, then run: "
      "sudo xcode-select -s /Applications/Xcode.app/Contents/Developer\n"
      "If Xcode was just installed or updated, accept the license with: "
      "sudo xcodebuild -license accept\n"
      "xcrun output: ${LAMBDAUI_METAL_FIND_OUTPUT}${LAMBDAUI_METAL_FIND_ERROR}")
  endif()

  execute_process(
    COMMAND ${LAMBDAUI_XCRUN_EXECUTABLE} -sdk macosx metal --help
    RESULT_VARIABLE LAMBDAUI_METAL_HELP_RESULT
    OUTPUT_VARIABLE LAMBDAUI_METAL_HELP_OUTPUT
    ERROR_VARIABLE LAMBDAUI_METAL_HELP_ERROR
  )
  if(LAMBDAUI_METAL_HELP_OUTPUT MATCHES "missing Metal Toolchain" OR
      LAMBDAUI_METAL_HELP_ERROR MATCHES "missing Metal Toolchain")
    message(FATAL_ERROR
      "LambdaUI macOS builds require Xcode's optional Metal Toolchain component. "
      "Install it with: xcodebuild -downloadComponent MetalToolchain\n"
      "xcrun output: ${LAMBDAUI_METAL_HELP_OUTPUT}${LAMBDAUI_METAL_HELP_ERROR}")
  endif()

  execute_process(
    COMMAND ${LAMBDAUI_XCRUN_EXECUTABLE} -sdk macosx -find metallib
    RESULT_VARIABLE LAMBDAUI_METALLIB_FIND_RESULT
    OUTPUT_VARIABLE LAMBDAUI_METALLIB_FIND_OUTPUT
    ERROR_VARIABLE LAMBDAUI_METALLIB_FIND_ERROR
  )
  if(NOT LAMBDAUI_METALLIB_FIND_RESULT EQUAL 0)
    string(STRIP "${LAMBDAUI_METALLIB_FIND_OUTPUT}" LAMBDAUI_METALLIB_FIND_OUTPUT)
    string(STRIP "${LAMBDAUI_METALLIB_FIND_ERROR}" LAMBDAUI_METALLIB_FIND_ERROR)
    message(FATAL_ERROR
      "LambdaUI macOS builds require the Metal library tool (`xcrun -sdk macosx metallib`). "
      "Install full Xcode, not only the Command Line Tools, then run: "
      "sudo xcode-select -s /Applications/Xcode.app/Contents/Developer\n"
      "If Xcode was just installed or updated, accept the license with: "
      "sudo xcodebuild -license accept\n"
      "If this Xcode install is missing optional Metal tools, install them with: "
      "xcodebuild -downloadComponent MetalToolchain\n"
      "xcrun output: ${LAMBDAUI_METALLIB_FIND_OUTPUT}${LAMBDAUI_METALLIB_FIND_ERROR}")
  endif()

  find_program(LAMBDAUI_XXD_EXECUTABLE xxd)
  if(NOT LAMBDAUI_XXD_EXECUTABLE)
    message(FATAL_ERROR
      "LambdaUI macOS builds require xxd to embed the compiled Metal shader library. "
      "Install Xcode or Vim/xxd and make sure xxd is on PATH.")
  endif()
endif()

FetchContent_Declare(libtess2
  GIT_REPOSITORY https://github.com/memononen/libtess2.git
  GIT_TAG 8dbd6483e920311a58c9af10a10beb278efebc36
)
FetchContent_MakeAvailable(libtess2)
if(NOT TARGET libtess2)
  add_library(libtess2 STATIC
    ${libtess2_SOURCE_DIR}/Source/bucketalloc.c
    ${libtess2_SOURCE_DIR}/Source/dict.c
    ${libtess2_SOURCE_DIR}/Source/geom.c
    ${libtess2_SOURCE_DIR}/Source/mesh.c
    ${libtess2_SOURCE_DIR}/Source/priorityq.c
    ${libtess2_SOURCE_DIR}/Source/sweep.c
    ${libtess2_SOURCE_DIR}/Source/tess.c
  )
  set_target_properties(libtess2 PROPERTIES LINKER_LANGUAGE C)
endif()
set_target_properties(libtess2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
set_target_properties(libtess2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(libtess2 PUBLIC
  $<BUILD_INTERFACE:${libtess2_SOURCE_DIR}/Include>)

option(LAMBDAUI_ENABLE_DEFAULT_EVENT_LOGGING
  "Print built-in Application event handlers to stdout (disable for quiet or embedded builds)."
  OFF)
option(LAMBDAUI_ENABLE_ASAN
  "Build LambdaUI targets with AddressSanitizer instrumentation."
  OFF)
option(LAMBDAUI_ENABLE_WARNINGS
  "Build LambdaUI core with common non-fatal compiler warnings."
  ON)
option(LAMBDAUI_PROFILE_REACTIVE
  "Compile deterministic reactive profiling counters and 5-second display-link reports."
  OFF)
option(LAMBDAUI_DISABLE_VARIANT_REFS
  "Deep-copy paragraph cache layouts (revert to clone on fast path); disables variant shared_ptr retention."
  OFF)
option(LAMBDAUI_ENABLE_PARAGRAPH_CACHE_PARALLEL_ASSERT
  "Debug: compare incremental vs full paragraph-cache assembly (non-NDEBUG only)."
  OFF)

option(LAMBDAUI_BUILD_DEMOS "Build standalone demos under demos/." OFF)
option(LAMBDAUI_BUILD_TESTS "Build unit and integration tests." OFF)
option(LAMBDAUI_BUILD_BENCHMARKS "Build micro-benchmarks in bench/." OFF)

add_subdirectory(src)

if(LAMBDAUI_ENABLE_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lambdaui PRIVATE -Wall -Wextra)
endif()

if(LAMBDAUI_BUILD_DEMOS)
  add_subdirectory(demos)
endif()

if(LAMBDAUI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LAMBDAUI_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
