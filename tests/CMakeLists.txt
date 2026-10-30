# Test plugins (ours, MIT) and the protocol test against a running bridge.
add_library(oros_test_clap MODULE test_plugins.c)
target_link_libraries(oros_test_clap PRIVATE clap)
set_target_properties(oros_test_clap PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "oros-test"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/testplugins
  LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/testplugins
  LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/testplugins)

find_program(NODE node)
if(NODE)
  add_test(NAME protocol
    COMMAND ${NODE} --test ${CMAKE_CURRENT_SOURCE_DIR}/protocol.test.js)
  set_tests_properties(protocol PROPERTIES ENVIRONMENT
    "OROS_BRIDGE=$<TARGET_FILE:oros-bridge>;OROS_TEST_PLUGINS=${CMAKE_BINARY_DIR}/testplugins")
endif()

# a VST 3 bundle: oros-test.vst3/Contents/<arch>/oros-test.(vst3|so)
set(VST3_SRC ${vst3sdk_SOURCE_DIR})
add_library(oros_test_vst3 MODULE vst3_test_gain.cpp ${VST3_SRC}/public.sdk/source/vst/vstsinglecomponenteffect.cpp)
target_link_libraries(oros_test_vst3 PRIVATE sdk)
if(WIN32)
  target_sources(oros_test_vst3 PRIVATE ${VST3_SRC}/public.sdk/source/main/dllmain.cpp)
  set(TEST_VST3_ARCH x86_64-win)
  set(TEST_VST3_SUFFIX .vst3)
else()
  target_sources(oros_test_vst3 PRIVATE ${VST3_SRC}/public.sdk/source/main/linuxmain.cpp)
  set(TEST_VST3_ARCH ${CMAKE_SYSTEM_PROCESSOR}-linux)
  set(TEST_VST3_SUFFIX .so)
endif()
set(TEST_VST3_DIR ${CMAKE_BINARY_DIR}/testplugins/oros-test.vst3/Contents/${TEST_VST3_ARCH})
set_target_properties(oros_test_vst3 PROPERTIES PREFIX "" SUFFIX ${TEST_VST3_SUFFIX} OUTPUT_NAME "oros-test"
  LIBRARY_OUTPUT_DIRECTORY ${TEST_VST3_DIR}
  LIBRARY_OUTPUT_DIRECTORY_DEBUG ${TEST_VST3_DIR}
  LIBRARY_OUTPUT_DIRECTORY_RELEASE ${TEST_VST3_DIR})
