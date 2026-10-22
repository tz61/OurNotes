add_library(on_test_main STATIC test_main.cpp)
target_link_libraries(on_test_main PUBLIC on_doctest)

# on_add_test(<name> <sources...> LIBS <libs...>)
function(on_add_test name)
    cmake_parse_arguments(T "" "" "LIBS" ${ARGN})
    add_executable(${name} ${T_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE on_test_main ${T_LIBS})
    target_compile_definitions(${name} PRIVATE OURNOTES_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    on_copy_runtime_dlls(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Each suite is added only when its module is part of this build configuration.
foreach(dir core sync formats pdf gui cli)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt" AND TARGET ournotes_${dir})
        add_subdirectory(${dir})
    endif()
endforeach()

# The browser extension's reading of pages, in a headless Chrome when there is one.
find_program(OURNOTES_CHROME NAMES google-chrome google-chrome-stable chromium chromium-browser chrome "Google Chrome"
             PATHS "C:/Program Files/Google/Chrome/Application" "/Applications/Google Chrome.app/Contents/MacOS")
if(OURNOTES_CHROME)
    set(page "${CMAKE_CURRENT_SOURCE_DIR}/connector/extract_test.html")
    if(WIN32)
        set(page "/${page}")  # file:///C:/...
    endif()
    add_test(NAME test_connector_extract
             COMMAND "${OURNOTES_CHROME}" --headless=new --disable-gpu --no-sandbox --dump-dom "file://${page}")
    # the result, not the test's own source (which spells both)
    set_tests_properties(test_connector_extract PROPERTIES
                         PASS_REGULAR_EXPRESSION "<pre id=\"out\">ALL PASSED</pre>" TIMEOUT 120)
endif()
