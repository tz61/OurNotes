add_executable(ournotes-cli
    main.cpp
    extra_commands.cpp
)
target_link_libraries(ournotes-cli PRIVATE ournotes_core ournotes_sync ournotes_pdf ournotes_formats)
install(TARGETS ournotes-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
