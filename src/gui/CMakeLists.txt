find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
set(FREEPEN_QT_VERSION ${Qt6_VERSION} PARENT_SCOPE)

set(FREEPEN_GUI_SOURCES
    app_context.cpp
    collection_tree.cpp
    flow_layout.cpp
    fulltext_indexer.cpp
    icons.cpp
    item_pane.cpp
    items_model.cpp
    library_view.cpp
    main_window.cpp
    note_editor.cpp
    note_html.cpp
    preferences.cpp
    sync_controller.cpp
    tag_selector.cpp
    reader/annotation_sidebar.cpp
    reader/pdf_view.cpp
    reader/reader_tab.cpp
    reader/render_worker.cpp
)
if(TARGET freepen_formats)
    list(APPEND FREEPEN_GUI_SOURCES formats_actions.cpp)
else()
    list(APPEND FREEPEN_GUI_SOURCES formats_actions_stub.cpp)
endif()

# Everything but main() is a library so tests can drive the widgets.
add_library(freepen_gui STATIC ${FREEPEN_GUI_SOURCES})
set_target_properties(freepen_gui PROPERTIES AUTOMOC ON AUTORCC ON)
target_link_libraries(freepen_gui PUBLIC freepen_core freepen_sync freepen_pdf Qt6::Widgets Qt6::Svg)
if(TARGET freepen_formats)
    target_link_libraries(freepen_gui PUBLIC freepen_formats)
endif()

qt_add_executable(freepen WIN32 MACOSX_BUNDLE main.cpp resources.qrc)
set_target_properties(freepen PROPERTIES
    AUTORCC ON
    MACOSX_BUNDLE_BUNDLE_NAME freePen
    MACOSX_BUNDLE_GUI_IDENTIFIER org.freepen.freePen
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
target_link_libraries(freepen PRIVATE freepen_gui)

install(TARGETS freepen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/freepen.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES "${PROJECT_SOURCE_DIR}/resources/icons/freepen.svg"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
