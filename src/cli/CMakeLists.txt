add_executable(freepen-cli
    main.cpp
    extra_commands.cpp
)
target_link_libraries(freepen-cli PRIVATE freepen_core)
install(TARGETS freepen-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
