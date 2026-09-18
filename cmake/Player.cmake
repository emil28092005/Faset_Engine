if(TARGET faset_gameplay)
    add_executable(faset_schema_exporter ${PROJECT_SOURCE_DIR}/apps/schema_exporter_main.cpp)
    target_link_libraries(faset_schema_exporter PRIVATE faset_core faset_gameplay)
endif()

if(TARGET faset_runtime AND TARGET faset_render AND TARGET faset_assets)
    add_library(faset_scene_view STATIC
        ${PROJECT_SOURCE_DIR}/src/player/SceneView.cpp
        ${PROJECT_SOURCE_DIR}/src/player/scene_io.cpp)
    target_include_directories(faset_scene_view PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(faset_scene_view PUBLIC faset_render faset_core PRIVATE faset_asset_data)
    if(TARGET faset_stb)
        target_link_libraries(faset_scene_view PRIVATE faset_stb)
        target_compile_definitions(faset_scene_view PRIVATE FASET_HAS_STB=1)
    endif()
    add_executable(faset_player ${PROJECT_SOURCE_DIR}/apps/player_main.cpp)
    target_link_libraries(faset_player PRIVATE faset_scene_view faset_runtime faset_gameplay)
    install(TARGETS faset_player RUNTIME DESTINATION .)
    if(BUILD_TESTING)
        add_executable(faset_player_tests ${PROJECT_SOURCE_DIR}/tests/runtime_player_tests.cpp)
        target_link_libraries(faset_player_tests PRIVATE faset_scene_view faset_runtime faset_assets)
        add_test(NAME player_scene_contracts COMMAND faset_player_tests)

        # Exercise the production Player entry with an isolated throwing module.
        # No test-only switches or behaviors are added to the shipping Player.
        add_executable(faset_player_diagnostics
            ${PROJECT_SOURCE_DIR}/apps/player_main.cpp
            ${PROJECT_SOURCE_DIR}/tests/player_diagnostics/Gameplay.cpp)
        target_include_directories(faset_player_diagnostics PRIVATE
            ${PROJECT_SOURCE_DIR}/tests/player_diagnostics)
        target_link_libraries(faset_player_diagnostics PRIVATE faset_scene_view faset_runtime)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME player_shutdown_diagnostics COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/player_diagnostics_test.py
            $<TARGET_FILE:faset_player_diagnostics>)
        set_tests_properties(player_shutdown_diagnostics PROPERTIES LABELS "gpu" TIMEOUT 60)
    endif()
endif()
