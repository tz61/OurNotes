add_library(ournotes_core STATIC
    config.cpp
    fulltext.cpp
    hash.cpp
    library.cpp
    schema.cpp
    sqlite.cpp
    util.cpp
    zotero_importer.cpp
    sync/engine.cpp
    sync/protocol.cpp
)
on_embed_file(ournotes_core "${PROJECT_SOURCE_DIR}/resources/schema.json" on_schema_json)

target_include_directories(ournotes_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(ournotes_core PUBLIC on_json on_sqlite3 Threads::Threads)
if(WIN32)
    target_link_libraries(ournotes_core PUBLIC bcrypt)
endif()
