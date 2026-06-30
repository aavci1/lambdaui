if(NOT DEFINED LAMBDAUI_FRAMEWORK_DIR)
  get_filename_component(LAMBDAUI_FRAMEWORK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  set(LAMBDAUI_FRAMEWORK_INCLUDE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/include")
  set(LAMBDAUI_FRAMEWORK_SOURCE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/src")
  set(LAMBDAUI_FRAMEWORK_CMAKE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/cmake")
  set(LAMBDAUI_FRAMEWORK_RESOURCE_DIR "${LAMBDAUI_FRAMEWORK_DIR}/resources")
  set(LAMBDAUI_FRAMEWORK_PROTOCOL_DIR "${LAMBDAUI_FRAMEWORK_DIR}/protocols")
  set(LAMBDAUI_FRAMEWORK_VENDOR_DIR "${LAMBDAUI_FRAMEWORK_DIR}/vendor")
endif()

set(LAMBDA_CORE_SOURCES
  Core/Identity.cpp
)

set(LAMBDA_REACTIVE_SOURCES
  Reactive/Observer.cpp
  Reactive/Easing.cpp
  Reactive/Transition.cpp
  Reactive/AnimationClock.cpp
)

set(LAMBDA_GRAPHICS_SOURCES
  Graphics/Styles.cpp
  Graphics/AttributedString.cpp
  Graphics/Canvas.cpp
  Graphics/GlyphAtlasAllocator.cpp
  Graphics/ImageLoader.cpp
  Graphics/RenderTarget.cpp
  Graphics/TextCacheDebugOverlay.cpp
  Graphics/TextSystem.cpp
  Graphics/xxhash_impl.c
  Graphics/Path.cpp
  Graphics/PathFlattener.cpp
  Graphics/SvgPath.cpp
)

set(LAMBDA_SCENEGRAPH_SOURCES
  SceneGraph/SceneNode.cpp
  SceneGraph/RasterCacheNode.cpp
  SceneGraph/RectNode.cpp
  SceneGraph/TextNode.cpp
  SceneGraph/ImageNode.cpp
  SceneGraph/PathNode.cpp
  SceneGraph/RenderNode.cpp
  SceneGraph/RenderTargetScene.cpp
  SceneGraph/SceneGraph.cpp
  SceneGraph/SceneInteraction.cpp
  SceneGraph/SceneRenderer.cpp
)

set(LAMBDA_LAYOUT_SOURCES
  Layout/Algorithms/StackLayout.cpp
)

set(LAMBDA_UI_SOURCES
  UI/EventQueue.cpp
  UI/MenuRoleDefaults.cpp
  UI/Window.cpp
  UI/WindowRender.cpp
  UI/MeasureContext.cpp
  UI/CommandRegistry.cpp
  UI/InputFieldChrome.cpp
  UI/InputFieldLayout.cpp
  UI/Detail/TraversalContext.cpp
  UI/Environment.cpp
  UI/Theme.cpp
  UI/Element/ElementMeasurement.cpp
  UI/Element/ElementModifiers.cpp
  UI/Element/ElementTypeErasure.cpp
  UI/LayoutDebugDump.cpp
  UI/ViewLayout/ScrollLayout.cpp
  UI/ViewLayout/OverlayLayout.cpp
  UI/MountContext.cpp
  UI/MountRoot.cpp
  UI/Runtime.cpp
  UI/TransientPopoverHost.cpp
  UI/Overlay.cpp
  UI/PopupMenu.cpp
  UI/OverlaySurfaceHelpers.cpp
  UI/Views/Alert.cpp
  UI/Views/BackdropBlur.cpp
  UI/Views/Badge.cpp
  UI/Views/Button.cpp
  UI/Views/Card.cpp
  UI/Views/Checkbox.cpp
  UI/Views/Dialog.cpp
  UI/Views/Divider.cpp
  UI/Views/FileDialog.cpp
  UI/Views/Grid.cpp
  UI/Views/Icon.cpp
  UI/Views/Image.cpp
  UI/Views/ListView.cpp
  UI/Views/Rectangle.cpp
  UI/Views/Render.cpp
  UI/Views/Text.cpp
  UI/Views/TextEditUtils.cpp
  UI/Views/TextInput.cpp
  UI/Views/LayoutContainers.cpp
  UI/Views/Popover.cpp
  UI/Views/PopoverCalloutPath.cpp
  UI/Views/PopoverCalloutShape.cpp
  UI/Views/ProgressBar.cpp
  UI/Views/ScrollView.cpp
  UI/Views/Select.cpp
  UI/Views/SegmentedControl.cpp
  UI/Views/Slider.cpp
  UI/Views/Svg.cpp
  UI/Views/TableView.cpp
  UI/Views/Toast.cpp
  UI/Views/Tooltip.cpp
  UI/Views/TransformViews.cpp
  UI/Views/Toggle.cpp
)

add_library(lambdaui SHARED
  ${LAMBDA_CORE_SOURCES}
  ${LAMBDA_REACTIVE_SOURCES}
  ${LAMBDA_GRAPHICS_SOURCES}
  ${LAMBDA_SCENEGRAPH_SOURCES}
  ${LAMBDA_LAYOUT_SOURCES}
  ${LAMBDA_UI_SOURCES}
)
add_library(lambdaui::lambdaui ALIAS lambdaui)

if(LAMBDAUI_RENDERER_WEBGPU)
  include(${LAMBDAUI_FRAMEWORK_CMAKE_DIR}/LambdaDawn.cmake)
endif()

set_target_properties(lambdaui PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_custom_target(lambdaui_module_dependency_check
  COMMAND ${LAMBDAUI_FRAMEWORK_DIR}/scripts/check_module_dependencies.sh
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  COMMENT "Check LambdaUI module dependencies"
  VERBATIM
)

if(LAMBDAUI_PLATFORM_MACOS AND LAMBDAUI_RENDERER_WEBGPU)
  target_sources(lambdaui PRIVATE
    UI/Application.mm
    Graphics/CoreTextSystem.mm
    Graphics/WebGPU/WebGpuContext.cpp
    Graphics/WebGPU/WebGpuCanvas.cpp
    Graphics/WebGPU/WebGpuRenderTarget.cpp
    Platform/Mac/MacApplication.mm
    Platform/Mac/MacMetalWindow.mm
    Platform/Mac/MacClipboard.mm
  )
elseif(LAMBDAUI_PLATFORM_LINUX_WAYLAND)
  include(${LAMBDAUI_FRAMEWORK_CMAKE_DIR}/LambdaWaylandProtocols.cmake)
  find_package(PkgConfig REQUIRED)
  find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
  pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  if(NOT WAYLAND_PROTOCOLS_DIR)
    message(FATAL_ERROR "Lambda: wayland-protocols did not provide pkgdatadir.")
  endif()
  pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
  pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
  pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
  pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
  pkg_check_modules(LIBRSVG REQUIRED IMPORTED_TARGET librsvg-2.0)
  pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
  lambda_setup_wayland_protocol_paths("${WAYLAND_PROTOCOLS_DIR}")
  add_library(lambda_wayland_client_protocols OBJECT
    ${LAMBDAUI_FRAMEWORK_PROTOCOL_DIR}/wayland/tablet-v2-tool-interface-stub.c)
  target_include_directories(lambda_wayland_client_protocols PUBLIC ${LAMBDA_WAYLAND_PROTOCOL_DIR})
  lambda_wayland_client_protocols(lambda_wayland_client_protocols
    "${LAMBDA_XDG_SHELL_XML}|xdg-shell"
    "${LAMBDA_XDG_DECORATION_XML}|xdg-decoration-unstable-v1"
    "${LAMBDA_VIEWPORTER_XML}|viewporter"
    "${LAMBDA_FRACTIONAL_SCALE_XML}|fractional-scale-v1"
    "${LAMBDA_XX_CUTOUTS_XML}|xx-cutouts-v1"
    "${LAMBDA_WLR_LAYER_SHELL_XML}|wlr-layer-shell-unstable-v1"
    "${LAMBDA_EXT_BACKGROUND_EFFECT_XML}|ext-background-effect-v1"
    "${LAMBDA_LINUX_DMABUF_XML}|linux-dmabuf-unstable-v1"
    "${LAMBDA_CURSOR_SHAPE_XML}|cursor-shape-v1"
    "${LAMBDA_IDLE_INHIBIT_XML}|idle-inhibit-unstable-v1"
    "${LAMBDA_PRESENTATION_TIME_XML}|presentation-time"
    "${LAMBDA_RELATIVE_POINTER_XML}|relative-pointer-unstable-v1"
    "${LAMBDA_POINTER_CONSTRAINTS_XML}|pointer-constraints-unstable-v1"
    "${LAMBDA_PRIMARY_SELECTION_XML}|primary-selection-unstable-v1"
    "${LAMBDA_XDG_ACTIVATION_XML}|xdg-activation-v1")
  target_sources(lambdaui PRIVATE
    UI/Application.cpp
    Graphics/Linux/FreeTypeTextSystem.cpp
    Platform/Linux/Common/XkbState.cpp
    Platform/Linux/WaylandApplication.cpp
    Platform/Linux/WaylandWindow.cpp
  )
  if(LAMBDAUI_RENDERER_WEBGPU)
    target_sources(lambdaui PRIVATE
      Graphics/WebGPU/WebGpuContext.cpp
      Graphics/WebGPU/WebGpuCanvas.cpp
      Graphics/WebGPU/WebGpuRenderTarget.cpp
    )
  endif()
  target_link_libraries(lambdaui PRIVATE lambda_wayland_client_protocols)
endif()

target_include_directories(lambdaui PUBLIC
  ${LAMBDAUI_FRAMEWORK_INCLUDE_DIR}
)
target_include_directories(lambdaui PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LAMBDAUI_FRAMEWORK_SOURCE_DIR}
  ${LAMBDAUI_FRAMEWORK_VENDOR_DIR}
)

target_compile_options(lambdaui PRIVATE -Wall -Wextra -Wpedantic)

if(APPLE)
  target_compile_options(lambdaui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

if(LAMBDAUI_ENABLE_ASAN)
  target_compile_options(lambdaui PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(lambdaui PRIVATE -fsanitize=address)
endif()

target_compile_definitions(lambdaui PRIVATE
  LAMBDAUI_ENABLE_DEFAULT_EVENT_LOGGING=$<BOOL:${LAMBDAUI_ENABLE_DEFAULT_EVENT_LOGGING}>
)

if(LAMBDAUI_PROFILE_REACTIVE)
  target_compile_definitions(lambdaui PUBLIC LAMBDAUI_PROFILE_REACTIVE=1)
else()
  target_compile_definitions(lambdaui PUBLIC LAMBDAUI_PROFILE_REACTIVE=0)
endif()

if(LAMBDAUI_DISABLE_VARIANT_REFS)
  target_compile_definitions(lambdaui PRIVATE LAMBDAUI_DISABLE_VARIANT_REFS=1)
endif()

if(LAMBDAUI_ENABLE_PARAGRAPH_CACHE_PARALLEL_ASSERT)
  target_compile_definitions(lambdaui PRIVATE LAMBDAUI_PARAGRAPH_CACHE_PARALLEL_ASSERT=1)
endif()

target_compile_definitions(lambdaui PUBLIC
  LAMBDAUI_NATIVE_RENDERERS=0)

if(LAMBDAUI_PLATFORM_LINUX_WAYLAND AND LAMBDAUI_RENDERER_WEBGPU)
  target_compile_definitions(lambdaui PUBLIC LAMBDAUI_METAL=0 LAMBDAUI_VULKAN=0 LAMBDAUI_WEBGPU=1 PRIVATE LAMBDAUI_PLATFORM_LINUX_WAYLAND=1)
  target_include_directories(lambdaui PRIVATE ${LAMBDA_WAYLAND_PROTOCOL_DIR})
  target_link_libraries(lambdaui PRIVATE
    dawn::webgpu_dawn
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::WAYLAND_CURSOR
    PkgConfig::XKBCOMMON
    PkgConfig::FREETYPE
    PkgConfig::FONTCONFIG
    PkgConfig::HARFBUZZ
    PkgConfig::LIBRSVG
    PkgConfig::ZLIB
    ${CMAKE_DL_LIBS})
elseif(LAMBDAUI_PLATFORM_MACOS AND LAMBDAUI_RENDERER_WEBGPU)
  target_compile_definitions(lambdaui PUBLIC LAMBDAUI_METAL=0 LAMBDAUI_VULKAN=0 LAMBDAUI_WEBGPU=1 PRIVATE LAMBDAUI_PLATFORM_MACOS=1)
  target_link_libraries(lambdaui PRIVATE
    dawn::webgpu_dawn
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework CoreText"
    "-framework CoreVideo")
endif()

target_link_libraries(lambdaui PRIVATE libtess2)

install(TARGETS lambdaui
  EXPORT LambdaTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
