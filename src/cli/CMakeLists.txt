# The commands apart from main(), for the tests too
add_library(ournotes_cli STATIC
    extra_commands.cpp
    agent_commands.cpp
)
target_link_libraries(ournotes_cli PUBLIC ournotes_core ournotes_sync ournotes_pdf ournotes_formats)
# docs/agents.md, built in for `ournotes-cli guide` (under 16 KB: MSVC's longest string literal)
set(_guide "${PROJECT_SOURCE_DIR}/docs/agents.md")
file(READ "${_guide}" OURNOTES_AGENT_GUIDE)
string(LENGTH "${OURNOTES_AGENT_GUIDE}" _length)
if(_length GREATER 16000)
    message(FATAL_ERROR "docs/agents.md is too long to be built into ournotes-cli")
endif()
configure_file(agent_guide.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/cli/agent_guide.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_guide}")
target_include_directories(ournotes_cli PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_executable(ournotes-cli main.cpp)
target_link_libraries(ournotes-cli PRIVATE ournotes_cli)
on_copy_runtime_dlls(ournotes-cli)
install(TARGETS ournotes-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# For AI agents: what OurNotes can do from the command line, beside the programs (at the
# top of the install folder: ~/.local/opt/ournotes, the tar.gz's and the zip's folder;
# build-dmg.sh puts it in OurNotes.app/Contents/Resources)
install(FILES "${PROJECT_SOURCE_DIR}/docs/agents.md" DESTINATION . RENAME AGENTS.md)
