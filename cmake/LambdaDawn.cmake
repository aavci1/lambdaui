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
  set(DAWN_ENABLE_INSTALL OFF CACHE BOOL "Install Dawn from the LambdaUI build." FORCE)
endfunction()

if(LAMBDAUI_DAWN_SOURCE_DIR)
  if(NOT EXISTS "${LAMBDAUI_DAWN_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "LAMBDAUI_DAWN_SOURCE_DIR must point at a Dawn source checkout containing CMakeLists.txt.")
  endif()
  lambda_configure_dawn_subproject()
  add_subdirectory("${LAMBDAUI_DAWN_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/dawn-build" EXCLUDE_FROM_ALL)
elseif(LAMBDAUI_DAWN_FETCH)
  include(FetchContent)
  lambda_configure_dawn_subproject()
  FetchContent_Declare(lambda_dawn
    GIT_REPOSITORY "${LAMBDAUI_DAWN_GIT_REPOSITORY}"
    GIT_TAG "${LAMBDAUI_DAWN_GIT_TAG}"
  )
  FetchContent_MakeAvailable(lambda_dawn)
else()
  find_package(Dawn CONFIG QUIET)
endif()

if(NOT TARGET dawn::webgpu_dawn AND TARGET webgpu_dawn)
  add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
endif()

if(NOT TARGET dawn::webgpu_dawn)
  message(FATAL_ERROR
    "LambdaUI WebGPU builds require Dawn's CMake package target dawn::webgpu_dawn. "
    "Build and install Dawn with -DDAWN_ENABLE_INSTALL=ON and pass its install prefix via "
    "CMAKE_PREFIX_PATH, pass a Dawn checkout with -DLAMBDAUI_DAWN_SOURCE_DIR=/path/to/dawn, "
    "or let LambdaUI fetch it with -DLAMBDAUI_DAWN_FETCH=ON.")
endif()
