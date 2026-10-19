file(GLOB _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
fp_add_test(test_core ${_sources} LIBS freepen_core)
