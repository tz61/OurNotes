find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
set(FREEPEN_QT_VERSION ${Qt6_VERSION} PARENT_SCOPE)

set(FREEPEN_GUI_SOURCES
    app_context.cpp
    collection_tree.cpp
    flow_layout.cpp
    formats_actions.cpp
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
    ui_scale.cpp
    reader/annotation_sidebar.cpp
    reader/pdf_view.cpp
    reader/reader_tab.cpp
    reader/render_worker.cpp
)

# Everything but main() is a library so tests can drive the widgets.
add_library(freepen_gui STATIC ${FREEPEN_GUI_SOURCES})
set_target_properties(freepen_gui PROPERTIES AUTOMOC ON AUTORCC ON)
target_link_libraries(freepen_gui PUBLIC freepen_core freepen_sync freepen_pdf freepen_formats Qt6::Widgets Qt6::Svg)

set(FREEPEN_ICON_DIR "${PROJECT_SOURCE_DIR}/resources/icons")
set(FREEPEN_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND FREEPEN_PLATFORM_SOURCES freepen.rc)
elseif(APPLE)
    set(FREEPEN_PLATFORM_SOURCES "${FREEPEN_ICON_DIR}/freepen.icns")
    set_source_files_properties("${FREEPEN_ICON_DIR}/freepen.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

qt_add_executable(freepen WIN32 MACOSX_BUNDLE main.cpp resources.qrc ${FREEPEN_PLATFORM_SOURCES})
set_target_properties(freepen PROPERTIES
    AUTORCC ON
    MACOSX_BUNDLE_BUNDLE_NAME freePen
    MACOSX_BUNDLE_GUI_IDENTIFIER org.freepen.freePen
    MACOSX_BUNDLE_ICON_FILE freepen.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
target_link_libraries(freepen PRIVATE freepen_gui)

install(TARGETS freepen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/freepen.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES "${FREEPEN_ICON_DIR}/freepen.svg" DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(DIRECTORY "${FREEPEN_ICON_DIR}/hicolor/" DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
endif()

# Self-contained installs (the per-user install script, app bundles): copy the Qt
# libraries and plugins next to the application.
option(FREEPEN_DEPLOY_QT "Copy the Qt runtime into the install prefix" OFF)
if(FREEPEN_DEPLOY_QT)
    qt_generate_deploy_app_script(TARGET freepen OUTPUT_SCRIPT freepen_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR NO_TRANSLATIONS)
    install(SCRIPT "${freepen_deploy_script}")
endif()
