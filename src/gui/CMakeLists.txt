find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
include(MicroTeX)

set(OURNOTES_GUI_SOURCES
    app_context.cpp
    collection_tree.cpp
    fancy_progress.cpp
    flow_layout.cpp
    formats_actions.cpp
    fulltext_indexer.cpp
    icons.cpp
    item_pane.cpp
    items_model.cpp
    library_view.cpp
    main_window.cpp
    math_dialog.cpp
    math_render.cpp
    memory.cpp
    metadata_progress.cpp
    note_editor.cpp
    note_html.cpp
    preferences.cpp
    shift_wheel.cpp
    sync_controller.cpp
    tab_widget.cpp
    tag_selector.cpp
    theme.cpp
    ui_scale.cpp
    wrap_toolbar.cpp
    reader/annotation_sidebar.cpp
    reader/pdf_view.cpp
    reader/reader_tab.cpp
    reader/render_worker.cpp
    rich_text_images.cpp
)

# Everything but main() is a library so tests can drive the widgets.
add_library(ournotes_gui STATIC ${OURNOTES_GUI_SOURCES})
set_target_properties(ournotes_gui PROPERTIES AUTOMOC ON AUTORCC ON)
target_link_libraries(ournotes_gui PUBLIC ournotes_core ournotes_sync ournotes_pdf ournotes_formats Qt6::Widgets Qt6::Svg
    PRIVATE on_microtex)
target_compile_definitions(ournotes_gui PRIVATE OURNOTES_MICROTEX_BUILD_RES="${OURNOTES_MICROTEX_RES}")

set(OURNOTES_ICON_DIR "${PROJECT_SOURCE_DIR}/resources/icons")
set(OURNOTES_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND OURNOTES_PLATFORM_SOURCES ournotes.rc)
elseif(APPLE)
    set(OURNOTES_PLATFORM_SOURCES "${OURNOTES_ICON_DIR}/ournotes.icns")
    set_source_files_properties("${OURNOTES_ICON_DIR}/ournotes.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

qt_add_executable(ournotes WIN32 MACOSX_BUNDLE main.cpp resources.qrc ${OURNOTES_PLATFORM_SOURCES})
set_target_properties(ournotes PROPERTIES
    AUTORCC ON
    MACOSX_BUNDLE_BUNDLE_NAME OurNotes
    MACOSX_BUNDLE_GUI_IDENTIFIER org.ournotes.OurNotes
    MACOSX_BUNDLE_ICON_FILE ournotes.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
target_link_libraries(ournotes PRIVATE ournotes_gui)
on_copy_runtime_dlls(ournotes)

install(TARGETS ournotes RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/ournotes.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES "${OURNOTES_ICON_DIR}/ournotes.svg" DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(DIRECTORY "${OURNOTES_ICON_DIR}/hicolor/" DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
endif()

# Self-contained installs (the per-user install script, app bundles): copy the Qt
# libraries and plugins next to the application.
option(OURNOTES_DEPLOY_QT "Copy the Qt runtime into the install prefix" OFF)
if(OURNOTES_DEPLOY_QT)
    qt_generate_deploy_app_script(TARGET ournotes OUTPUT_SCRIPT ournotes_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR NO_TRANSLATIONS)
    install(SCRIPT "${ournotes_deploy_script}")
    if(WIN32)
        # the Visual C++ runtime, for computers without its redistributable
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
        include(InstallRequiredSystemLibraries)
    endif()
endif()
