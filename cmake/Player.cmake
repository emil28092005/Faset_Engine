if(TARGET faset_gameplay)
    add_executable(faset_schema_exporter ${PROJECT_SOURCE_DIR}/apps/schema_exporter_main.cpp)
    target_link_libraries(faset_schema_exporter PRIVATE faset_core faset_gameplay faset_scripting_project)
    if(TARGET faset_lua)
        target_link_libraries(faset_schema_exporter PRIVATE faset_lua)
        target_compile_definitions(faset_schema_exporter PRIVATE FASET_HAS_LUA=1)
    endif()
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
    target_link_libraries(faset_player PRIVATE faset_scene_view faset_runtime faset_gameplay faset_scripting_project)
    if(TARGET faset_lua)
        target_link_libraries(faset_player PRIVATE faset_lua)
        target_compile_definitions(faset_player PRIVATE FASET_HAS_LUA=1)
    endif()
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
        target_link_libraries(faset_player_diagnostics PRIVATE faset_scene_view faset_runtime faset_scripting_project)
        if(TARGET faset_lua)
            target_link_libraries(faset_player_diagnostics PRIVATE faset_lua)
            target_compile_definitions(faset_player_diagnostics PRIVATE FASET_HAS_LUA=1)
        endif()
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME player_shutdown_diagnostics COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/player_diagnostics_test.py
            $<TARGET_FILE:faset_player_diagnostics>)
        set_tests_properties(player_shutdown_diagnostics PROPERTIES LABELS "gpu" TIMEOUT 60)
    endif()
endif()

if(BUILD_TESTING AND TARGET faset_schema_exporter)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(FASET_LUA_CLI_TEST_ARGS --exporter $<TARGET_FILE:faset_schema_exporter>)
    if(TARGET faset_player)
        list(APPEND FASET_LUA_CLI_TEST_ARGS --player $<TARGET_FILE:faset_player>)
    endif()
    if(NOT TARGET faset_lua)
        list(APPEND FASET_LUA_CLI_TEST_ARGS --disabled)
    endif()
    add_test(NAME lua_cli_contracts COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/lua_cli_test.py ${FASET_LUA_CLI_TEST_ARGS})
    set_tests_properties(lua_cli_contracts PROPERTIES TIMEOUT 90)
    if(TARGET faset_lua AND TARGET faset_player)
        add_test(NAME lua_player_reload COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/lua_player_reload_test.py $<TARGET_FILE:faset_player>)
        set_tests_properties(lua_player_reload PROPERTIES LABELS "gpu" TIMEOUT 90)
    endif()
endif()
