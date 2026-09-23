if(TARGET faset_render)
  target_sources(faset_render PRIVATE "${PROJECT_SOURCE_DIR}/src/render/visibility.cpp")
endif()
if(TARGET faset_render AND BUILD_TESTING)
  add_executable(faset_render_visibility_policy_tests
    "${PROJECT_SOURCE_DIR}/tests/render_visibility_policy_tests.cpp")
  target_link_libraries(faset_render_visibility_policy_tests PRIVATE faset_render)
  add_test(NAME visibility_policy COMMAND faset_render_visibility_policy_tests)
endif()
