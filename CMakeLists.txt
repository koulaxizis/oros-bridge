# orOS Bridge: hosts the person's own VST3 and CLAP plugins for orOS Studio,
# on this computer only (127.0.0.1). See DESIGN.md.
cmake_minimum_required(VERSION 3.28)
project(oros_bridge VERSION 0.8.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  add_compile_options(/utf-8)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(OROS_BRIDGE_TESTS "Build the test plugins and the protocol test" ON)
# the ASIO module is GPLv3 (asio/LICENSE) and a separate dll; the bridge stays MIT without it
option(OROS_BRIDGE_ASIO "Build oros-bridge-asio.dll (Windows, GPLv3)" ON)

include(FetchContent)
# CLAP 1.2.9 (MIT)
FetchContent_Declare(clap
  GIT_REPOSITORY https://github.com/free-audio/clap.git
  GIT_TAG 1.2.9
  GIT_SHALLOW TRUE)
# VST 3 SDK 3.8.1 (MIT since 3.8.0); only the libraries, no examples, no VSTGUI
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_ADD_VST3_UTILITIES OFF CACHE BOOL "" FORCE)
set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE) # same /MT as the bridge
FetchContent_Declare(vst3sdk
  GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
  GIT_TAG v3.8.1_build_84
  GIT_SHALLOW TRUE
  GIT_SUBMODULES base cmake pluginterfaces public.sdk
  EXCLUDE_FROM_ALL)
# nlohmann/json 3.11.3 (MIT), single header
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
# miniaudio 0.11.25 (public domain or MIT-0): the sound card
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.25
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none)   # not its own CMake (examples, tests): we build miniaudio.c below
FetchContent_MakeAvailable(clap json vst3sdk miniaudio)

# only playback and capture, only the backends Studio offers; every system
# library is loaded at run time, so a missing JACK or PulseAudio hides one backend
add_library(oros_miniaudio STATIC ${miniaudio_SOURCE_DIR}/miniaudio.c)
target_include_directories(oros_miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_compile_definitions(oros_miniaudio PUBLIC
  MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE
  MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI MA_ENABLE_JACK MA_ENABLE_PULSEAUDIO MA_ENABLE_ALSA MA_ENABLE_COREAUDIO)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(oros_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

add_library(bridge_core STATIC
  src/base64.cpp
  src/sha1.cpp
  src/sha256.cpp
  src/config.cpp
  src/mainloop.cpp
  src/server.cpp
  src/session.cpp
  src/scan.cpp
  src/clap_host.cpp
  src/vst3_host.cpp
  src/editor.cpp
  src/audio.cpp
  src/audio_ma.cpp
  src/audio_module.cpp
  src/midi.cpp
  src/update.cpp
)
# the SDK's hosting library leaves these to the host
set(VST3_SRC ${vst3sdk_SOURCE_DIR})
target_sources(bridge_core PRIVATE
  ${VST3_SRC}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${VST3_SRC}/public.sdk/source/common/memorystream.cpp)
if(WIN32)
  target_sources(bridge_core PRIVATE ${VST3_SRC}/public.sdk/source/vst/hosting/module_win32.cpp)
elseif(APPLE)
  target_sources(bridge_core PRIVATE ${VST3_SRC}/public.sdk/source/vst/hosting/module_mac.mm)
else()
  target_sources(bridge_core PRIVATE ${VST3_SRC}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()
target_include_directories(bridge_core PUBLIC src)
target_link_libraries(bridge_core PUBLIC clap nlohmann_json::nlohmann_json sdk_hosting oros_miniaudio)
target_compile_definitions(bridge_core PUBLIC OROS_BRIDGE_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_link_libraries(bridge_core PUBLIC ws2_32 bcrypt shell32 ole32 user32 winmm winhttp)
  target_compile_definitions(bridge_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(bridge_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_executable(oros-bridge src/main.cpp)
target_link_libraries(oros-bridge PRIVATE bridge_core)
if(WIN32)
  # a window and a tray icon instead of a console (--console keeps the old way)
  set(OROS_BRIDGE_ICO ${CMAKE_CURRENT_SOURCE_DIR}/res/oros-bridge.ico)
  set(OROS_BRIDGE_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/res/oros-bridge.manifest)
  configure_file(res/oros-bridge.rc.in ${CMAKE_CURRENT_BINARY_DIR}/oros-bridge.rc @ONLY)
  target_sources(oros-bridge PRIVATE src/gui_win32.cpp ${CMAKE_CURRENT_BINARY_DIR}/oros-bridge.rc)
  set_target_properties(oros-bridge PROPERTIES WIN32_EXECUTABLE ON)
  target_link_libraries(oros-bridge PRIVATE comctl32 advapi32)
  if(MSVC)
    # main() stays the entry point; our manifest is in the .rc
    target_link_options(oros-bridge PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # the window and tray icon (GTK 3, AppIndicator), both loaded at run time: no build dependency
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/res/oros-bridge.png OROS_ICON_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," OROS_ICON_BYTES "${OROS_ICON_HEX}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/gui_icon.h
    "#pragma once\n// made by CMake from res/oros-bridge.png\nstatic const unsigned char kIconPng[] = {${OROS_ICON_BYTES}};\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS res/oros-bridge.png)
  target_include_directories(oros-bridge PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
  target_sources(oros-bridge PRIVATE src/gui_gtk.cpp)
else()
  target_sources(oros-bridge PRIVATE src/gui_none.cpp)
endif()

if(WIN32 AND OROS_BRIDGE_ASIO)
  add_subdirectory(asio)
endif()

if(OROS_BRIDGE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
