# Native plugins deliberately require the exact SDK, compiler, CRT and build.
file(GLOB_RECURSE FASET_SDK_INPUTS CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/include/faset/*.hpp"
  "${PROJECT_SOURCE_DIR}/include/faset/*.h"
  "${PROJECT_SOURCE_DIR}/src/editor/*.cpp"
  "${PROJECT_SOURCE_DIR}/src/authoring/*.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FASET_SDK_INPUTS} "${PROJECT_SOURCE_DIR}/dependencies.lock.json")
set(FASET_SDK_SIGNATURE "${PROJECT_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_SIZEOF_VOID_P};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_COMPILER_FRONTEND_VARIANT};${CMAKE_MSVC_RUNTIME_LIBRARY};${CMAKE_BUILD_TYPE};${CMAKE_CXX_FLAGS};${FASET_SANITIZERS}")
foreach(source IN LISTS FASET_SDK_INPUTS)
  file(SHA256 "${source}" source_hash)
  string(APPEND FASET_SDK_SIGNATURE ";${source_hash}")
endforeach()
file(SHA256 "${PROJECT_SOURCE_DIR}/dependencies.lock.json" dependency_hash)
string(SHA256 FASET_EDITOR_SDK_FINGERPRINT "${FASET_SDK_SIGNATURE};${dependency_hash}")
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/faset/editor")
file(WRITE "${PROJECT_BINARY_DIR}/generated/faset/editor/sdk_build.h"
  "#pragma once\n#define FASET_EDITOR_SDK_FINGERPRINT \"${FASET_EDITOR_SDK_FINGERPRINT}\"\n")
add_library(faset_editor_sdk INTERFACE)
target_include_directories(faset_editor_sdk INTERFACE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
add_library(faset_editor_plugins STATIC src/editor/plugins.cpp)
target_link_libraries(faset_editor_plugins PUBLIC faset_editor_commands faset_editor_sdk PRIVATE ${CMAKE_DL_LIBS})
add_library(faset_example_plugin MODULE examples/extensions/beacon/Editor.cpp)
target_link_libraries(faset_example_plugin PRIVATE faset_editor_sdk nlohmann_json::nlohmann_json)
set_target_properties(faset_example_plugin PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/example-plugin" RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/example-plugin")
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/example-plugin/beacon.faset-plugin.json" CONTENT
  "{\n  \"format\":\"faset.editor_plugin\",\n  \"version\":1,\n  \"id\":\"example.beacon\",\n  \"module_version\":\"1.0.0\",\n  \"kind\":\"editor\",\n  \"api_version\":1,\n  \"build_fingerprint\":\"${FASET_EDITOR_SDK_FINGERPRINT}\",\n  \"library\":\"$<TARGET_FILE_NAME:faset_example_plugin>\",\n  \"dependencies\":[]\n}\n")
if(BUILD_TESTING)
  add_executable(faset_plugin_tests tests/plugin_tests.cpp)
  target_link_libraries(faset_plugin_tests PRIVATE faset_editor_plugins faset_runtime)
  target_compile_definitions(faset_plugin_tests PRIVATE FASET_TEST_PLUGIN_DIRECTORY="${PROJECT_BINARY_DIR}/example-plugin")
  add_dependencies(faset_plugin_tests faset_example_plugin)
  add_test(NAME editor_plugins COMMAND faset_plugin_tests)
endif()
