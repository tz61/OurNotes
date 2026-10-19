add_executable(ournotesd main.cpp)
target_link_libraries(ournotesd PRIVATE ournotes_sync)
if(WIN32)
    target_link_libraries(ournotesd PRIVATE shell32)  # CommandLineToArgvW
endif()

install(TARGETS ournotesd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
