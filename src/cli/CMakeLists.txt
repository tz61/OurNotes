add_executable(freepen-cli
    main.cpp
    extra_commands.cpp
)
target_link_libraries(freepen-cli PRIVATE freepen_core freepen_sync freepen_pdf freepen_formats)
install(TARGETS freepen-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
