add_executable(ournotesd main.cpp)
target_link_libraries(ournotesd PRIVATE ournotes_sync)
if(WIN32)
    target_link_libraries(ournotesd PRIVATE shell32)  # CommandLineToArgvW
endif()
on_copy_runtime_dlls(ournotesd)

install(TARGETS ournotesd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
