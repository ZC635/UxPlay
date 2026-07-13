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
