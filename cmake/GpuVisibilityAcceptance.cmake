# P2's image-equivalence tests use the real offscreen Vulkan renderer. Keep the benchmark
# executable available without making performance thresholds part of CTest.
if(BUILD_TESTING AND TARGET faset_render)
  add_executable(faset_render_gpu_acceptance_tests
    "${PROJECT_SOURCE_DIR}/tests/render_gpu_acceptance_tests.cpp")
  target_link_libraries(faset_render_gpu_acceptance_tests PRIVATE faset_render)
  foreach(FASET_VISIBILITY_CASE empty capacity dense door shadow cut resize lod)
    add_test(NAME "render_gpu_${FASET_VISIBILITY_CASE}"
      COMMAND faset_render_gpu_acceptance_tests --case "${FASET_VISIBILITY_CASE}")
    set_tests_properties("render_gpu_${FASET_VISIBILITY_CASE}"
      PROPERTIES LABELS "gpu;p2" TIMEOUT 120)
  endforeach()
endif()
