cmake_minimum_required(VERSION 3.16)
project(obs-delay-stream VERSION 3.0.0)

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

# ---- Plugin target ----
add_library(obs-delay-stream MODULE src/plugin-main.cpp)

# ---- Embed receiver/index.html into the plugin ----
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/receiver/index.html"
               "${CMAKE_CURRENT_BINARY_DIR}/receiver/index.html" @ONLY)
set(RECEIVER_HTML_IN  "${CMAKE_CURRENT_BINARY_DIR}/receiver/index.html")
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
add_dependencies(obs-delay-stream embed_receiver_html)

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
  mswsock
  iphlpapi
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

file(GLOB CONFIGURE_DEPENDS RECEIVER_SVG_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/receiver/*.svg"
)

install(TARGETS obs-delay-stream LIBRARY DESTINATION "${OBS_PLUGIN_BIN_DIR}")
install(DIRECTORY data/locale/ DESTINATION "${OBS_PLUGIN_DATA_DIR}/locale")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/receiver/index.html"
  receiver/ui.css
  ${RECEIVER_SVG_FILES}
  DESTINATION "${OBS_PLUGIN_DATA_DIR}/receiver"
)
install(DIRECTORY receiver/third_party/ DESTINATION "${OBS_PLUGIN_DATA_DIR}/receiver/third_party")
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
  COMMAND ${CMAKE_COMMAND} -E copy
    "${CMAKE_CURRENT_BINARY_DIR}/receiver/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/receiver/ui.css"
    ${RECEIVER_SVG_FILES}
    "$<TARGET_FILE_DIR:obs-delay-stream>/data/obs-plugins/obs-delay-stream/receiver/"
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:obs-delay-stream>/data/obs-plugins/obs-delay-stream/receiver/third_party"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/receiver/third_party"
    "$<TARGET_FILE_DIR:obs-delay-stream>/data/obs-plugins/obs-delay-stream/receiver/third_party"
)
