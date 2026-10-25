add_executable(RendererSampleTapsTest
               renderer_sample_taps_test.c)

target_link_libraries(RendererSampleTapsTest
                      PRIVATE
                      renderers
                      ${GST_LINK_LIBRARIES})

if(WIN32 AND PKG_CONFIG_EXECUTABLE)
  get_filename_component(GSTREAMER_RUNTIME_BIN_DIR
                         "${PKG_CONFIG_EXECUTABLE}"
                         DIRECTORY)
  add_test(NAME RendererSampleTapsTest
           COMMAND "${CMAKE_COMMAND}" -E env
                   "PATH=${GSTREAMER_RUNTIME_BIN_DIR};$ENV{PATH}"
                   "$<TARGET_FILE:RendererSampleTapsTest>")
else()
  add_test(NAME RendererSampleTapsTest
           COMMAND RendererSampleTapsTest)
endif()

add_executable(RtpCleanupTest rtp_cleanup_test.c)
add_executable(MirrorCleanupTest mirror_cleanup_test.c)
foreach(cleanup_target RtpCleanupTest MirrorCleanupTest)
  target_link_libraries(${cleanup_target} PRIVATE airplay)
  target_include_directories(${cleanup_target} PRIVATE ${PLIST_INCLUDE_DIRS})
  add_test(NAME ${cleanup_target} COMMAND ${cleanup_target})
  set_tests_properties(${cleanup_target} PROPERTIES TIMEOUT 15
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_BIN_DIR}")
endforeach()

set_tests_properties(RendererSampleTapsTest PROPERTIES TIMEOUT 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TEST_GLIB REQUIRED IMPORTED_TARGET glib-2.0)
add_executable(HttpdTimeoutTest httpd_timeout_test.c)
target_link_libraries(HttpdTimeoutTest PRIVATE airplay PkgConfig::TEST_GLIB)

add_test(NAME HttpdTimeoutTest COMMAND HttpdTimeoutTest)
set_tests_properties(HttpdTimeoutTest PROPERTIES TIMEOUT 15
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_BIN_DIR}")



add_executable(PairingRegressionTest pairing_regression_test.c)
target_link_libraries(PairingRegressionTest PRIVATE airplay)
add_test(NAME PairingRegressionTest COMMAND PairingRegressionTest)
set_tests_properties(PairingRegressionTest PROPERTIES TIMEOUT 20
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_BIN_DIR}")

add_executable(RaopNtpRegressionTest ntp_regression_test.c)
target_link_libraries(RaopNtpRegressionTest PRIVATE airplay)
add_test(NAME RaopNtpRegressionTest COMMAND RaopNtpRegressionTest)
set_tests_properties(RaopNtpRegressionTest PROPERTIES TIMEOUT 15
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_BIN_DIR}")

add_executable(MirrorNalRegressionTest mirror_nal_regression_test.c)
target_link_libraries(MirrorNalRegressionTest PRIVATE airplay)
if(WIN32)
  target_link_libraries(MirrorNalRegressionTest PRIVATE ws2_32)
endif()
add_test(NAME MirrorNalRegressionTest COMMAND MirrorNalRegressionTest)
set_tests_properties(MirrorNalRegressionTest PROPERTIES TIMEOUT 15
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_BIN_DIR}")

add_executable(PairingHandlerRegressionTest pairing_handler_regression_test.c)
# Keep the header-first compile check effective even when GCC defaults to C23.
set_target_properties(PairingHandlerRegressionTest PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_link_libraries(PairingHandlerRegressionTest PRIVATE airplay)
target_include_directories(PairingHandlerRegressionTest PRIVATE ${PLIST_INCLUDE_DIRS})
if(PLIST_21_FOUND)
  target_compile_definitions(PairingHandlerRegressionTest PRIVATE PLIST_210)
endif()
if(PLIST_23_FOUND)
  target_compile_definitions(PairingHandlerRegressionTest PRIVATE PLIST_230)
endif()
add_test(NAME PairingHandlerRegressionTest COMMAND PairingHandlerRegressionTest)
add_test(NAME SetupLengthRegressionTest COMMAND PairingHandlerRegressionTest setup)
set_tests_properties(PairingHandlerRegressionTest SetupLengthRegressionTest PROPERTIES TIMEOUT 20
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_BIN_DIR}")

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
  add_executable(PairingMemoryTest pairing_memory_test.c)
  target_link_libraries(PairingMemoryTest PRIVATE airplay)
  target_link_options(PairingMemoryTest PRIVATE
      "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=free")
  add_test(NAME PairingMemoryTest COMMAND PairingMemoryTest)
  set_tests_properties(PairingMemoryTest PROPERTIES TIMEOUT 30
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_BIN_DIR}")
endif()
