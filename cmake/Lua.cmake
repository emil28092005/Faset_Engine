# Official Lua sources are checksum-pinned in dependencies.lock.json. Build only
# the VM and libraries, never the standalone lua/luac executables or a system ABI.
faset_dependency(lua)
set(lua_src "${FASET_lua_SOURCE_DIR}/src")
add_library(faset_lua_vendor STATIC
    ${lua_src}/lapi.c ${lua_src}/lcode.c ${lua_src}/lctype.c
    ${lua_src}/ldebug.c ${lua_src}/ldo.c ${lua_src}/ldump.c
    ${lua_src}/lfunc.c ${lua_src}/lgc.c ${lua_src}/llex.c
    ${lua_src}/lmem.c ${lua_src}/lobject.c ${lua_src}/lopcodes.c
    ${lua_src}/lparser.c ${lua_src}/lstate.c ${lua_src}/lstring.c
    ${lua_src}/ltable.c ${lua_src}/ltm.c ${lua_src}/lundump.c
    ${lua_src}/lvm.c ${lua_src}/lzio.c ${lua_src}/lauxlib.c
    ${lua_src}/lbaselib.c ${lua_src}/lmathlib.c ${lua_src}/lstrlib.c
    ${lua_src}/ltablib.c ${lua_src}/lutf8lib.c)
target_include_directories(faset_lua_vendor SYSTEM PUBLIC "${lua_src}")
if(NOT MSVC)
    # Upstream intentionally uses compiler-supported computed gotos in the VM.
    target_compile_options(faset_lua_vendor PRIVATE -Wno-pedantic)
endif()
if(UNIX)
    target_link_libraries(faset_lua_vendor PUBLIC m)
endif()
add_library(faset_lua STATIC ${PROJECT_SOURCE_DIR}/src/scripting/LuaModule.cpp)
add_library(Faset::Lua ALIAS faset_lua)
target_include_directories(faset_lua PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(faset_lua PUBLIC faset_runtime faset_scripting_project PRIVATE faset_lua_vendor)

if(BUILD_TESTING)
    add_executable(faset_lua_tests ${PROJECT_SOURCE_DIR}/tests/lua_tests.cpp)
    target_link_libraries(faset_lua_tests PRIVATE faset_lua)
    target_compile_definitions(faset_lua_tests PRIVATE FASET_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME lua_contracts COMMAND faset_lua_tests)
    set_tests_properties(lua_contracts PROPERTIES TIMEOUT 30)
    add_executable(faset_lua_safety_tests ${PROJECT_SOURCE_DIR}/tests/lua_safety_tests.cpp)
    target_link_libraries(faset_lua_safety_tests PRIVATE faset_lua)
    add_test(NAME lua_safety_contracts COMMAND faset_lua_safety_tests)
    set_tests_properties(lua_safety_contracts PROPERTIES TIMEOUT 30)
endif()
