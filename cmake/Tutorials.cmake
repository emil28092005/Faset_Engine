if(BUILD_TESTING AND TARGET faset_runtime)
    foreach(tutorial moving following spawning physics)
        set(tutorial_dir "${PROJECT_SOURCE_DIR}/examples/tutorials/${tutorial}")
        add_library(faset_tutorial_${tutorial} STATIC "${tutorial_dir}/Gameplay.cpp")
        target_include_directories(faset_tutorial_${tutorial} PUBLIC "${tutorial_dir}")
        target_link_libraries(faset_tutorial_${tutorial} PUBLIC faset_runtime)
        add_executable(faset_tutorial_${tutorial}_tests "${PROJECT_SOURCE_DIR}/tests/runtime_tutorials.cpp")
        target_link_libraries(faset_tutorial_${tutorial}_tests PRIVATE faset_tutorial_${tutorial})
        target_compile_definitions(faset_tutorial_${tutorial}_tests PRIVATE
            FASET_TUTORIAL_NAME="${tutorial}"
            FASET_TUTORIAL_SCENE="${tutorial_dir}/scene.json")
        add_test(NAME tutorial_${tutorial} COMMAND faset_tutorial_${tutorial}_tests)
    endforeach()
endif()

if(BUILD_TESTING AND TARGET faset_runtime)
    foreach(dimension 2 3)
        set(project_dir "${PROJECT_SOURCE_DIR}/examples/projects/collect-${dimension}d")
        add_library(faset_example_${dimension}d STATIC "${project_dir}/Scripts/Gameplay.cpp")
        target_include_directories(faset_example_${dimension}d PUBLIC "${project_dir}/Scripts")
        target_link_libraries(faset_example_${dimension}d PUBLIC faset_runtime)
        add_executable(faset_example_${dimension}d_tests "${PROJECT_SOURCE_DIR}/tests/runtime_tutorials_projects.cpp")
        target_link_libraries(faset_example_${dimension}d_tests PRIVATE faset_example_${dimension}d)
        target_compile_definitions(faset_example_${dimension}d_tests PRIVATE
            FASET_EXAMPLE_DIMENSION=${dimension}
            FASET_EXAMPLE_PROJECT="${project_dir}")
        add_test(NAME playable_${dimension}d COMMAND faset_example_${dimension}d_tests)
    endforeach()
endif()
