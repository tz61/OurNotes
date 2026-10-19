add_executable(freepend main.cpp)
target_link_libraries(freepend PRIVATE freepen_sync)
if(WIN32)
    target_link_libraries(freepend PRIVATE shell32)  # CommandLineToArgvW
endif()

install(TARGETS freepend RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
