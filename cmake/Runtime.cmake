add_library(faset_runtime STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/runtime/Runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/runtime/Physics.cpp)
add_library(Faset::Runtime ALIAS faset_runtime)
target_compile_features(faset_runtime PUBLIC cxx_std_20)
target_include_directories(faset_runtime PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../include)
target_link_libraries(faset_runtime PUBLIC nlohmann_json::nlohmann_json PRIVATE EnTT::EnTT box2d box3d)

add_library(faset_gameplay STATIC ${CMAKE_CURRENT_LIST_DIR}/../examples/gameplay/Gameplay.cpp)
add_library(Faset::Gameplay ALIAS faset_gameplay)
target_include_directories(faset_gameplay PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../examples/gameplay)
target_link_libraries(faset_gameplay PUBLIC faset_runtime)

if(BUILD_TESTING)
    add_executable(faset_runtime_tests ${CMAKE_CURRENT_LIST_DIR}/../tests/runtime_tests.cpp)
    target_link_libraries(faset_runtime_tests PRIVATE faset_runtime faset_gameplay)
    add_test(NAME runtime_contracts COMMAND faset_runtime_tests)
endif()
