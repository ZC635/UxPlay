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