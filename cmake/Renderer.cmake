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
  add_custom_command(OUTPUT "${FASET_SHADER_OUTPUT}"
    COMMAND "${SLANGC_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/shaders/baseline.slang"
            -entry "${FASET_ENTRY}" -target spirv -profile spirv_1_6 -matrix-layout-column-major
            -o "${FASET_SHADER_OUTPUT}" -reflection-json "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json"
    BYPRODUCTS "${FASET_SHADER_DIRECTORY}/${FASET_ENTRY}.reflection.json"
    DEPENDS "${PROJECT_SOURCE_DIR}/shaders/baseline.slang" VERBATIM)
  list(APPEND FASET_SHADER_OUTPUTS "${FASET_SHADER_OUTPUT}")
endforeach()
add_custom_command(OUTPUT "${FASET_SHADER_DIRECTORY}/compatibility.spv"
  COMMAND "${SLANGC_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/shaders/compatibility.hlsl"
          -entry compatibilityMain -stage compute -target spirv -profile spirv_1_6
          -o "${FASET_SHADER_DIRECTORY}/compatibility.spv"
  DEPENDS "${PROJECT_SOURCE_DIR}/shaders/compatibility.hlsl" VERBATIM)
add_custom_target(faset_shaders DEPENDS ${FASET_SHADER_OUTPUTS} "${FASET_SHADER_DIRECTORY}/compatibility.spv")
add_library(faset_render "${PROJECT_SOURCE_DIR}/src/render/renderer.cpp" "${PROJECT_SOURCE_DIR}/src/render/math.cpp" "${PROJECT_SOURCE_DIR}/src/render/render_graph.cpp")
target_include_directories(faset_render PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faset_render PUBLIC cxx_std_20)
target_link_libraries(faset_render PRIVATE Vulkan::Vulkan SDL3::SDL3)
target_compile_definitions(faset_render PRIVATE FASET_SHADER_DIRECTORY="${FASET_SHADER_DIRECTORY}")
add_dependencies(faset_render faset_shaders)
if(BUILD_TESTING)
  add_executable(faset_render_tests "${PROJECT_SOURCE_DIR}/tests/render_tests.cpp")
  target_link_libraries(faset_render_tests PRIVATE faset_render)
  add_test(NAME render_graph COMMAND faset_render_tests --unit)
  add_test(NAME render_offscreen COMMAND faset_render_tests --gpu "${CMAKE_BINARY_DIR}/render-test.ppm")
  set_tests_properties(render_offscreen PROPERTIES LABELS "gpu")
endif()
install(FILES ${FASET_SHADER_OUTPUTS} DESTINATION shaders)
