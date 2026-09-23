if(TARGET faset_render)
  add_executable(faset_p2_visibility_example
    "${PROJECT_SOURCE_DIR}/examples/renderer/p2_visibility.cpp")
  target_link_libraries(faset_p2_visibility_example PRIVATE faset_render)
endif()

if(TARGET faset_render AND BUILD_TESTING)
  add_executable(faset_render_gpu_visibility_tests
    "${PROJECT_SOURCE_DIR}/tests/render_gpu_visibility_tests.cpp")
  target_link_libraries(faset_render_gpu_visibility_tests PRIVATE faset_render)
  add_test(NAME gpu_visibility COMMAND faset_render_gpu_visibility_tests)
  set_tests_properties(gpu_visibility PROPERTIES LABELS "gpu;p2")
endif()
