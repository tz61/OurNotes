# The GUI tests need their own main() so the QApplication is owned by main.
find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(test_gui test_gui.cpp)
target_link_libraries(test_gui PRIVATE fp_doctest freepen_gui Qt6::Test)
target_compile_definitions(test_gui PRIVATE FREEPEN_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME test_gui COMMAND test_gui)
set_tests_properties(test_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
