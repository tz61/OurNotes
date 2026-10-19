file(GLOB _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
on_add_test(test_core ${_sources} LIBS ournotes_core)
