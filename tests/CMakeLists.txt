add_library(on_test_main STATIC test_main.cpp)
target_link_libraries(on_test_main PUBLIC on_doctest)

# on_add_test(<name> <sources...> LIBS <libs...>)
function(on_add_test name)
    cmake_parse_arguments(T "" "" "LIBS" ${ARGN})
    add_executable(${name} ${T_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE on_test_main ${T_LIBS})
    target_compile_definitions(${name} PRIVATE OURNOTES_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Each suite is added only when its module is part of this build configuration.
foreach(dir core sync formats pdf gui)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt" AND TARGET ournotes_${dir})
        add_subdirectory(${dir})
    endif()
endforeach()
