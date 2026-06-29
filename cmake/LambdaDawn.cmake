set(LAMBDAUI_DAWN_SOURCE_DIR "" CACHE PATH
  "Optional path to a Dawn source checkout. When empty, LambdaUI uses find_package(Dawn).")

if(LAMBDAUI_DAWN_SOURCE_DIR)
  if(NOT EXISTS "${LAMBDAUI_DAWN_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "LAMBDAUI_DAWN_SOURCE_DIR must point at a Dawn source checkout containing CMakeLists.txt.")
  endif()
  set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "Fetch Dawn third-party dependencies." FORCE)
  set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "Build Dawn samples." FORCE)
  set(DAWN_BUILD_TESTS OFF CACHE BOOL "Build Dawn tests." FORCE)
  add_subdirectory("${LAMBDAUI_DAWN_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/dawn-build" EXCLUDE_FROM_ALL)
else()
  find_package(Dawn CONFIG REQUIRED)
endif()

if(NOT TARGET dawn::webgpu_dawn AND TARGET webgpu_dawn)
  add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
endif()

if(NOT TARGET dawn::webgpu_dawn)
  message(FATAL_ERROR
    "LambdaUI WebGPU builds require Dawn's CMake package target dawn::webgpu_dawn. "
    "Build and install Dawn with -DDAWN_ENABLE_INSTALL=ON and pass its install prefix via "
    "CMAKE_PREFIX_PATH, or pass a Dawn checkout with -DLAMBDAUI_DAWN_SOURCE_DIR=/path/to/dawn.")
endif()
