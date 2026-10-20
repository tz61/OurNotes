# The GUI tests need their own main() so the QApplication is owned by main.
find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(test_gui test_gui.cpp)
target_link_libraries(test_gui PRIVATE on_doctest ournotes_gui Qt6::Test)
target_compile_definitions(test_gui PRIVATE OURNOTES_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
# A window's pixels as painted (not drawn anew) take Qt's private backing store; without its
# headers the test that compares them only does the clicking.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 QUIET COMPONENTS GuiPrivate)
if(TARGET Qt6::GuiPrivate)
    target_link_libraries(test_gui PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(test_gui PRIVATE OURNOTES_TEST_BACKING_STORE)
endif()
on_copy_runtime_dlls(test_gui)
add_test(NAME test_gui COMMAND test_gui)
set_tests_properties(test_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# what goes wrong only at a fractional scale (rows' edges inside pixels)
add_test(NAME test_gui_scaled COMMAND test_gui -tc=*leaves\ no\ line*)
set_tests_properties(test_gui_scaled PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5")
