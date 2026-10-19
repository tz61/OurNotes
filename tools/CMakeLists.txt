# Developer tools (not built by default): cmake -DFREEPEN_BUILD_TOOLS=ON
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Svg)
add_executable(render_icons render_icons.cpp)
target_link_libraries(render_icons PRIVATE Qt6::Gui Qt6::Svg)
