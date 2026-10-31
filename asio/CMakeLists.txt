# oros-bridge-asio.dll (GPLv3, see LICENSE here): ASIO sound cards for the
# bridge, built with the Steinberg ASIO SDK 2.3.4 under its GPLv3 licence.
# The bridge (MIT) loads it only when it lies beside oros-bridge.exe; the
# boundary is src/audio_module.h. Windows only.
FetchContent_Declare(asiosdk
  URL https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
  URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(asiosdk)
set(ASIO_SDK ${asiosdk_SOURCE_DIR})

add_library(oros_asio_sdk STATIC
  ${ASIO_SDK}/common/asio.cpp
  ${ASIO_SDK}/host/asiodrivers.cpp
  ${ASIO_SDK}/host/pc/asiolist.cpp)
target_include_directories(oros_asio_sdk SYSTEM PUBLIC ${ASIO_SDK}/common ${ASIO_SDK}/host ${ASIO_SDK}/host/pc)
target_link_libraries(oros_asio_sdk PUBLIC ole32 advapi32 user32)
if(MSVC)
  target_compile_options(oros_asio_sdk PRIVATE /W0)
else()
  target_compile_options(oros_asio_sdk PRIVATE -w)
endif()

add_library(oros_bridge_asio SHARED oros_asio.cpp)
target_link_libraries(oros_bridge_asio PRIVATE oros_asio_sdk nlohmann_json::nlohmann_json)
# beside oros-bridge.exe (build/ or build/<config>/), where the bridge looks
set_target_properties(oros_bridge_asio PROPERTIES PREFIX "" OUTPUT_NAME "oros-bridge-asio"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(OROS_BRIDGE_TESTS)
  # a loopback ASIO driver for the protocol test; never shipped
  add_library(oros_test_asio MODULE test_driver.cpp test_driver.def)
  target_link_libraries(oros_test_asio PRIVATE oros_asio_sdk)
  set_target_properties(oros_test_asio PROPERTIES PREFIX "" OUTPUT_NAME "oros-test-asio"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/testplugins
    LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/testplugins
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/testplugins)
endif()
