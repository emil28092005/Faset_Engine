# Include after the Faset targets have been declared. A source manifest is merged
# with CMake's generated application manifest by its MSVC/Ninja linker wrapper.
# External build tools have their own manifests and still need a compatible host.
if(WIN32)
    get_property(_faset_targets DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_faset_target IN LISTS _faset_targets)
        get_target_property(_faset_type "${_faset_target}" TYPE)
        if(_faset_type STREQUAL "EXECUTABLE" AND _faset_target MATCHES "^faset_")
            target_sources("${_faset_target}" PRIVATE "${PROJECT_SOURCE_DIR}/cmake/windows.manifest")
        endif()
    endforeach()
endif()
