# orOS Bridge: hosts the person's own VST3 and CLAP plugins for orOS Studio,
# on this computer only (127.0.0.1). See DESIGN.md.
cmake_minimum_required(VERSION 3.28)
project(oros_bridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  add_compile_options(/utf-8)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(OROS_BRIDGE_TESTS "Build the test plugins and the protocol test" ON)

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
FetchContent_MakeAvailable(clap json vst3sdk)

add_library(bridge_core STATIC
  src/base64.cpp
  src/sha1.cpp
  src/config.cpp
  src/mainloop.cpp
  src/server.cpp
  src/session.cpp
  src/scan.cpp
  src/clap_host.cpp
  src/vst3_host.cpp
  src/editor.cpp
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
target_link_libraries(bridge_core PUBLIC clap nlohmann_json::nlohmann_json sdk_hosting)
target_compile_definitions(bridge_core PUBLIC OROS_BRIDGE_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_link_libraries(bridge_core PUBLIC ws2_32 bcrypt shell32 ole32 user32)
  target_compile_definitions(bridge_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(bridge_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_executable(oros-bridge src/main.cpp)
target_link_libraries(oros-bridge PRIVATE bridge_core)

if(OROS_BRIDGE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
