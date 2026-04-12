cmake_minimum_required(VERSION 3.16)
project(obs-delay-stream VERSION 7.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OBS_PLUGIN_LEGACY_INSTALL "Use legacy obs-plugins/64bit install layout" ON)

if(NOT DEFINED OBS_SOURCE_DIR)
  message(FATAL_ERROR "OBS_SOURCE_DIR not set.")
endif()

file(TO_CMAKE_PATH "${OBS_SOURCE_DIR}" OBS_DIR)

# Find OBS deps directory (version-independent)
file(GLOB _DEPS "${OBS_DIR}/.deps/obs-deps-*-x64")
if(_DEPS)
  list(GET _DEPS 0 OBS_DEPS_DIR)
  message(STATUS "OBS deps: ${OBS_DEPS_DIR}")
endif()

# ---- Resolve libobs dependencies ----
list(APPEND CMAKE_PREFIX_PATH
  "${OBS_DIR}/build_x64"
  "${OBS_DIR}/build_x64/libobs"
  "${OBS_DIR}/build_x64/deps/w32-pthreads"
  "${OBS_DIR}/build_x64/deps/libcaption"
  "${OBS_DEPS_DIR}"
  "${OBS_DEPS_DIR}/lib/cmake"
  "${OBS_DIR}/cmake"
)
list(APPEND CMAKE_MODULE_PATH
  "${OBS_DIR}/cmake/finders"
  "${OBS_DIR}/cmake"
)

find_package(w32-pthreads QUIET HINTS "${OBS_DIR}/build_x64/deps/w32-pthreads" "${OBS_DIR}/build_x64")
find_package(SIMDe QUIET   HINTS "${OBS_DEPS_DIR}" "${OBS_DEPS_DIR}/lib/cmake")
find_package(Threads REQUIRED)
find_package(libobs REQUIRED HINTS "${OBS_DIR}/build_x64/libobs" "${OBS_DIR}/build_x64" "${OBS_DIR}/cmake")

# ---- Qt (for custom stepper widget) ----
file(GLOB _QT6_DEPS "${OBS_DIR}/.deps/obs-deps-qt6-*-x64")
if(_QT6_DEPS)
  list(GET _QT6_DEPS 0 _QT6_DEP_DIR)
  list(APPEND CMAKE_PREFIX_PATH "${_QT6_DEP_DIR}")
endif()
find_package(Qt6 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
  find_package(Qt5 5.15 COMPONENTS Widgets REQUIRED)
endif()

# ---- Plugin target ----
add_library(obs-delay-stream MODULE
  src/plugin/plugin-main.cpp
  src/plugin/plugin-utils.cpp
  src/plugin/plugin-services.cpp
  src/plugin/plugin-assets.cpp
  src/plugin/release-check.cpp
  src/plugin/plugin-config.cpp
  src/plugin/plugin-settings.cpp
  src/plugin/plugin-helpers.cpp
  src/audio/audio-processor.cpp
  src/audio/probe-signal.cpp
  src/ui/props-refresh.cpp
  src/ui/table-theme.cpp
  src/ui/url-share-renderer.cpp
  src/ui/properties-url-share.cpp
  src/ui/properties-channels.cpp
  src/ui/properties-delay.cpp
  src/ui/properties-builder.cpp
  src/widgets/stepper-widget.cpp
  src/widgets/text-button-widget.cpp
  src/widgets/path-mode-row-widget.cpp
  src/widgets/mode-text-row-widget.cpp
  src/widgets/color-buttons-widget.cpp
  src/widgets/pulldown-row-widget.cpp
  src/widgets/delay-diagram-widget.cpp
  src/widgets/delay-table-widget.cpp
  src/widgets/flow-progress-widget.cpp
  src/widgets/flow-table-widget.cpp
  src/widgets/widget-payload-utils.cpp
  src/network/opus-encoder.cpp
  src/network/stream-router.cpp
  src/network/stream-router-utils.cpp
  src/network/rtmp-prober.cpp
  src/network/rtsp-e2e-prober.cpp
  src/tunnel/tunnel-manager.cpp
  src/sync/sync-flow.cpp
)

# ---- Build receiver assets with npm ----
set(RECEIVER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/receiver")
set(RECEIVER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/receiver_dist")
set(RECEIVER_BUILD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/receiver_dist.stamp")

if(WIN32)
  # Prefer npm.cmd on Windows. Resolving to bare "npm" can fail under MSBuild.
  find_program(NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)
else()
  find_program(NPM_EXECUTABLE NAMES npm REQUIRED)
endif()
message(STATUS "Receiver build npm: ${NPM_EXECUTABLE}")

file(GLOB_RECURSE CONFIGURE_DEPENDS RECEIVER_BUILD_INPUTS
  "${RECEIVER_SOURCE_DIR}/*"
)
list(FILTER RECEIVER_BUILD_INPUTS EXCLUDE REGEX "/receiver/dist/")
list(FILTER RECEIVER_BUILD_INPUTS EXCLUDE REGEX "/receiver/node_modules/")

add_custom_command(
  OUTPUT "${RECEIVER_BUILD_STAMP}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RECEIVER_BUILD_DIR}"
  COMMAND ${CMAKE_COMMAND} -E echo "Building receiver assets -> ${RECEIVER_BUILD_DIR}"
  COMMAND ${CMAKE_COMMAND} -E echo "Using npm command: ${NPM_EXECUTABLE}"
  COMMAND "${NPM_EXECUTABLE}" install
  COMMAND ${CMAKE_COMMAND} -E env
    "RECEIVER_BUILD_OUT_DIR=${RECEIVER_BUILD_DIR}"
    "RECEIVER_PROJECT_VERSION=${PROJECT_VERSION}"
    "NO_COLOR=1"
    "FORCE_COLOR=0"
    "NPM_CONFIG_COLOR=false"
    "${NPM_EXECUTABLE}" run build
  COMMAND ${CMAKE_COMMAND} -E touch "${RECEIVER_BUILD_STAMP}"
  WORKING_DIRECTORY "${RECEIVER_SOURCE_DIR}"
  DEPENDS ${RECEIVER_BUILD_INPUTS}
  BYPRODUCTS
    "${RECEIVER_BUILD_DIR}/index.html"
  VERBATIM
)

add_custom_target(build_receiver_assets DEPENDS "${RECEIVER_BUILD_STAMP}")

# ---- Embed receiver/index.html into the plugin ----
# Keep runtime placeholders as literals. They are replaced in plugin-main.cpp.
set(RECEIVER_HTML_IN  "${RECEIVER_BUILD_DIR}/index.html")
set(RECEIVER_HTML_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/receiver_index_html.hpp")

add_custom_command(
  OUTPUT "${RECEIVER_HTML_OUT}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT="${RECEIVER_HTML_IN}"
    -DOUTPUT="${RECEIVER_HTML_OUT}"
    -DVAR_NAME="kReceiverIndexHtml"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_html.cmake"
  DEPENDS
    "${RECEIVER_HTML_IN}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_html.cmake"
  VERBATIM
)

add_custom_target(embed_receiver_html DEPENDS "${RECEIVER_HTML_OUT}")
add_dependencies(embed_receiver_html build_receiver_assets)
add_dependencies(obs-delay-stream build_receiver_assets embed_receiver_html)

target_include_directories(obs-delay-stream PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(OBS_DEPS_DIR)
  target_include_directories(obs-delay-stream PRIVATE
    "${OBS_DEPS_DIR}/include"
  )
  target_link_directories(obs-delay-stream PRIVATE
    "${OBS_DEPS_DIR}/lib"
  )
endif()

# IMPORTANT: websocketpp and asio include dirs must come BEFORE OBS/SIMDe
# to ensure their uint32_t usage resolves from <cstdint> not SIMDe
target_include_directories(obs-delay-stream BEFORE PRIVATE
  src/
  third_party/websocketpp/
  third_party/asio/asio/include/
  third_party/asio/include/
)

# OBS include dirs come AFTER (added via target_link_libraries OBS::libobs)
target_link_libraries(obs-delay-stream
  OBS::libobs
  avcodec
  avutil
  swresample
  ws2_32
  wininet
  mswsock
  iphlpapi
  psapi
)


target_compile_definitions(obs-delay-stream PRIVATE
  ASIO_STANDALONE
  _WEBSOCKETPP_CPP11_STL_
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  # Tell SIMDe to NOT redefine standard integer types
  SIMDE_NO_NATIVE
  PLUGIN_VERSION="${PROJECT_VERSION}"
)

# ---- Qt stepper widget ----
if(Qt6_FOUND)
  target_link_libraries(obs-delay-stream Qt6::Widgets)
else()
  target_link_libraries(obs-delay-stream Qt5::Widgets)
endif()

# Force MSVC to treat source as UTF-8
if(MSVC)
  target_compile_options(obs-delay-stream PRIVATE /utf-8 /wd4005 /wd4819)
endif()

if(OBS_PLUGIN_LEGACY_INSTALL)
  set(OBS_PLUGIN_BIN_DIR "obs-plugins/64bit")
  set(OBS_PLUGIN_DATA_DIR "data/obs-plugins/obs-delay-stream")
else()
  set(OBS_PLUGIN_BIN_DIR "plugins/obs-delay-stream/bin/64bit")
  set(OBS_PLUGIN_DATA_DIR "plugins/obs-delay-stream/data")
endif()

install(TARGETS obs-delay-stream LIBRARY DESTINATION "${OBS_PLUGIN_BIN_DIR}")
install(DIRECTORY data/locale/ DESTINATION "${OBS_PLUGIN_DATA_DIR}/locale")
install(DIRECTORY "${RECEIVER_BUILD_DIR}/" DESTINATION "${OBS_PLUGIN_DATA_DIR}/receiver")
install(FILES LICENSE THIRD_PARTY_NOTICES README.md DESTINATION .)

# Copy data files next to DLL so OBS finds them during development
add_custom_command(TARGET obs-delay-stream POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:obs-delay-stream>/data/obs-plugins/obs-delay-stream/locale"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
    "$<TARGET_FILE_DIR:obs-delay-stream>/data/obs-plugins/obs-delay-stream/locale"
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:obs-delay-stream>/data/obs-plugins/obs-delay-stream/receiver"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${RECEIVER_BUILD_DIR}"
    "$<TARGET_FILE_DIR:obs-delay-stream>/data/obs-plugins/obs-delay-stream/receiver"
)
