find_package(Vulkan 1.3 REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(SLANGC_EXECUTABLE NAMES slangc HINTS "${PROJECT_SOURCE_DIR}/.cache/slang/bin" "$ENV{VULKAN_SDK}/bin")
if(NOT SLANGC_EXECUTABLE)
  message(FATAL_ERROR "Slang compiler missing. Run: python tools/fetch_slang.py, or set SLANGC_EXECUTABLE.")
endif()
set(FASET_SHADER_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${FASET_SHADER_DIRECTORY}")
set(FASET_SHADER_OUTPUTS)
foreach(FASET_ENTRY vertexMain fragmentMain shadowMain)
  set(FASET_SHADER_OUTPUT "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.spv")
  add_custom_command(OUTPUT "${FASET_SHADER_OUTPUT}" "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/compile_shader.py"
            --compiler "${SLANGC_EXECUTABLE}" --source "${PROJECT_SOURCE_DIR}/shaders/baseline.slang"
            --entry "${FASET_ENTRY}" --output "${FASET_SHADER_DIRECTORY}"
    BYPRODUCTS "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.slang-reflection.json"
    DEPENDS "${PROJECT_SOURCE_DIR}/shaders/baseline.slang" "${PROJECT_SOURCE_DIR}/tools/compile_shader.py" VERBATIM)
  list(APPEND FASET_SHADER_OUTPUTS "${FASET_SHADER_OUTPUT}" "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json")
endforeach()
foreach(FASET_ENTRY gpuVertexMain gpuShadowMain gpuCullMain gpuHzbMain gpuPostCullMain)
  if(FASET_ENTRY STREQUAL "gpuVertexMain" OR FASET_ENTRY STREQUAL "gpuShadowMain")
    set(FASET_GPU_DEFINE FASET_GPU_GRAPHICS=1)
  elseif(FASET_ENTRY STREQUAL "gpuHzbMain")
    set(FASET_GPU_DEFINE FASET_GPU_HZB=1)
  else()
    set(FASET_GPU_DEFINE FASET_GPU_CULL=1)
  endif()
  set(FASET_SHADER_OUTPUT "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.spv")
  add_custom_command(OUTPUT "${FASET_SHADER_OUTPUT}" "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/compile_shader.py"
            --compiler "${SLANGC_EXECUTABLE}" --source "${PROJECT_SOURCE_DIR}/shaders/gpu_scene.slang"
            --entry "${FASET_ENTRY}" --define "${FASET_GPU_DEFINE}" --output "${FASET_SHADER_DIRECTORY}"
    BYPRODUCTS "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.slang-reflection.json"
    DEPENDS "${PROJECT_SOURCE_DIR}/shaders/gpu_scene.slang" "${PROJECT_SOURCE_DIR}/tools/compile_shader.py" VERBATIM)
  list(APPEND FASET_SHADER_OUTPUTS "${FASET_SHADER_OUTPUT}" "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json")
endforeach()
foreach(FASET_ENTRY temporalResolveMain temporalCompositeVertexMain temporalCompositeFragmentMain)
  if(FASET_ENTRY STREQUAL "temporalResolveMain")
    set(FASET_TEMPORAL_DEFINE FASET_TEMPORAL_RESOLVE=1)
  else()
    set(FASET_TEMPORAL_DEFINE FASET_TEMPORAL_COMPOSITE=1)
  endif()
  set(FASET_SHADER_OUTPUT "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.spv")
  add_custom_command(OUTPUT "${FASET_SHADER_OUTPUT}" "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/compile_shader.py"
            --compiler "${SLANGC_EXECUTABLE}" --source "${PROJECT_SOURCE_DIR}/shaders/temporal.slang"
            --entry "${FASET_ENTRY}" --define "${FASET_TEMPORAL_DEFINE}" --output "${FASET_SHADER_DIRECTORY}"
    BYPRODUCTS "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.slang-reflection.json"
    DEPENDS "${PROJECT_SOURCE_DIR}/shaders/temporal.slang" "${PROJECT_SOURCE_DIR}/tools/compile_shader.py" VERBATIM)
  list(APPEND FASET_SHADER_OUTPUTS "${FASET_SHADER_OUTPUT}" "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json")
endforeach()
add_custom_command(OUTPUT "${FASET_SHADER_DIRECTORY}/compatibility.spv"
  COMMAND "${SLANGC_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/shaders/compatibility.hlsl"
          -entry compatibilityMain -stage compute -target spirv -profile spirv_1_6
          -o "${FASET_SHADER_DIRECTORY}/compatibility.spv"
  DEPENDS "${PROJECT_SOURCE_DIR}/shaders/compatibility.hlsl" VERBATIM)
add_custom_target(faset_shaders DEPENDS ${FASET_SHADER_OUTPUTS} "${FASET_SHADER_DIRECTORY}/compatibility.spv")
add_library(faset_render "${PROJECT_SOURCE_DIR}/src/render/renderer.cpp" "${PROJECT_SOURCE_DIR}/src/render/math.cpp" "${PROJECT_SOURCE_DIR}/src/render/render_graph.cpp" "${PROJECT_SOURCE_DIR}/src/render/shader_contract.cpp" "${PROJECT_SOURCE_DIR}/src/render/lighting.cpp" "${PROJECT_SOURCE_DIR}/src/render/temporal.cpp" "${PROJECT_SOURCE_DIR}/src/render/temporal_reference.cpp")
target_include_directories(faset_render PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faset_render PUBLIC cxx_std_20)
target_link_libraries(faset_render PRIVATE Vulkan::Vulkan SDL3::SDL3 faset_core)
target_compile_definitions(faset_render PRIVATE FASET_SHADER_DIRECTORY="${FASET_SHADER_DIRECTORY}")
add_dependencies(faset_render faset_shaders)
if(BUILD_TESTING)
  add_executable(faset_render_lighting_gpu_tests "${PROJECT_SOURCE_DIR}/tests/render_lighting_gpu_tests.cpp")
  target_link_libraries(faset_render_lighting_gpu_tests PRIVATE faset_render)
  add_test(NAME render_lighting_sun COMMAND faset_render_lighting_gpu_tests --sun)
  set_tests_properties(render_lighting_sun PROPERTIES LABELS "gpu;p3")
  add_test(NAME render_lighting_local COMMAND faset_render_lighting_gpu_tests --local)
  set_tests_properties(render_lighting_local PROPERTIES LABELS "gpu;p3")
  add_executable(faset_render_lighting_policy_tests "${PROJECT_SOURCE_DIR}/tests/render_lighting_policy_tests.cpp")
  target_link_libraries(faset_render_lighting_policy_tests PRIVATE faset_render)
  add_test(NAME render_lighting_policy COMMAND faset_render_lighting_policy_tests)
  set_tests_properties(render_lighting_policy PROPERTIES LABELS "p3")
  add_executable(faset_render_temporal_shader_contract_tests "${PROJECT_SOURCE_DIR}/tests/render_temporal_shader_contract_tests.cpp")
  target_include_directories(faset_render_temporal_shader_contract_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/render")
  target_link_libraries(faset_render_temporal_shader_contract_tests PRIVATE faset_render faset_core)
  target_compile_definitions(faset_render_temporal_shader_contract_tests PRIVATE FASET_TEST_SHADER_DIRECTORY="${FASET_SHADER_DIRECTORY}")
  add_test(NAME render_temporal_shader_contract COMMAND faset_render_temporal_shader_contract_tests)
  add_executable(faset_render_temporal_reference_tests "${PROJECT_SOURCE_DIR}/tests/render_temporal_reference_tests.cpp")
  target_link_libraries(faset_render_temporal_reference_tests PRIVATE faset_render)
  add_test(NAME render_temporal_reference COMMAND faset_render_temporal_reference_tests)
  add_executable(faset_render_temporal_lifecycle_tests "${PROJECT_SOURCE_DIR}/tests/render_temporal_lifecycle_tests.cpp")
  target_link_libraries(faset_render_temporal_lifecycle_tests PRIVATE faset_render)
  add_test(NAME render_temporal_lifecycle COMMAND faset_render_temporal_lifecycle_tests)
  set_tests_properties(render_temporal_lifecycle PROPERTIES LABELS "gpu")
  add_executable(faset_render_temporal_motion_tests "${PROJECT_SOURCE_DIR}/tests/render_temporal_motion_tests.cpp")
  target_link_libraries(faset_render_temporal_motion_tests PRIVATE faset_render)
  add_test(NAME render_temporal_motion COMMAND faset_render_temporal_motion_tests)
  add_executable(faset_render_temporal_policy_tests "${PROJECT_SOURCE_DIR}/tests/render_temporal_policy_tests.cpp")
  target_link_libraries(faset_render_temporal_policy_tests PRIVATE faset_render)
  add_test(NAME render_temporal_policy COMMAND faset_render_temporal_policy_tests)
  add_executable(faset_render_tests "${PROJECT_SOURCE_DIR}/tests/render_tests.cpp")
  target_link_libraries(faset_render_tests PRIVATE faset_render SDL3::SDL3)
  add_test(NAME render_graph COMMAND faset_render_tests --unit)
  add_test(NAME render_offscreen COMMAND faset_render_tests --gpu "${CMAKE_BINARY_DIR}/render-test.ppm")
  set_tests_properties(render_offscreen PROPERTIES LABELS "gpu")
  add_executable(faset_render_sprite_tests "${PROJECT_SOURCE_DIR}/tests/render_sprite_tests.cpp")
  target_link_libraries(faset_render_sprite_tests PRIVATE faset_render)
  add_test(NAME render_sprite_alpha COMMAND faset_render_sprite_tests)
  set_tests_properties(render_sprite_alpha PROPERTIES LABELS "gpu")
  add_executable(faset_render_reload_tests "${PROJECT_SOURCE_DIR}/tests/render_reload_tests.cpp")
  target_link_libraries(faset_render_reload_tests PRIVATE faset_render faset_core)
  target_compile_definitions(faset_render_reload_tests PRIVATE
    FASET_TEST_SHADER_DIRECTORY="${FASET_SHADER_DIRECTORY}"
    FASET_TEST_SHADER_SOURCE="${PROJECT_SOURCE_DIR}/shaders/baseline.slang"
    FASET_SHADER_COMPILE_TOOL="${PROJECT_SOURCE_DIR}/tools/compile_shader.py"
    FASET_TEST_SLANGC="${SLANGC_EXECUTABLE}"
    FASET_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}")
  add_test(NAME render_shader_reload COMMAND faset_render_reload_tests)
  set_tests_properties(render_shader_reload PROPERTIES LABELS "gpu")
  add_executable(faset_render_gpu_shader_contract_tests "${PROJECT_SOURCE_DIR}/tests/render_gpu_shader_contract_tests.cpp")
  target_include_directories(faset_render_gpu_shader_contract_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/render")
  target_link_libraries(faset_render_gpu_shader_contract_tests PRIVATE faset_render faset_core)
  target_compile_definitions(faset_render_gpu_shader_contract_tests PRIVATE FASET_TEST_SHADER_DIRECTORY="${FASET_SHADER_DIRECTORY}")
  add_test(NAME render_gpu_shader_contract COMMAND faset_render_gpu_shader_contract_tests)
  add_test(NAME render_shader_reflection COMMAND "${CMAKE_COMMAND}" -E env
    "FASET_TEST_SLANGC=${SLANGC_EXECUTABLE}"
    "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_shader_reflection.py")
  add_executable(faset_render_window_tests "${PROJECT_SOURCE_DIR}/tests/render_window_tests.cpp")
  target_link_libraries(faset_render_window_tests PRIVATE faset_render SDL3::SDL3)
  add_test(NAME render_window_lifecycle COMMAND faset_render_window_tests "${CMAKE_BINARY_DIR}/window-test")
  set_tests_properties(render_window_lifecycle PROPERTIES LABELS "gpu;window" TIMEOUT 40 SKIP_RETURN_CODE 77)
  add_executable(faset_p3_lighting_benchmark
    "${PROJECT_SOURCE_DIR}/examples/renderer/p3_lighting_benchmark.cpp")
  target_link_libraries(faset_p3_lighting_benchmark PRIVATE faset_render faset_core)
  target_compile_definitions(faset_p3_lighting_benchmark PRIVATE
    FASET_BENCHMARK_CONFIGURATION="$<CONFIG>")
  add_test(NAME render_lighting_benchmark_schema COMMAND
    "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_p3_lighting_benchmark.py")
  set_tests_properties(render_lighting_benchmark_schema PROPERTIES LABELS "p3" TIMEOUT 90)
  add_test(NAME render_lighting_benchmark_smoke COMMAND
    "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_p3_lighting_benchmark.py"
    --real-executable "$<TARGET_FILE:faset_p3_lighting_benchmark>")
  set_tests_properties(render_lighting_benchmark_smoke PROPERTIES LABELS "gpu;p3" TIMEOUT 90)
endif()
install(FILES ${FASET_SHADER_OUTPUTS} DESTINATION shaders)
