add_library(freepen_core STATIC
    config.cpp
    hash.cpp
    schema.cpp
    sqlite.cpp
    util.cpp
    sync/protocol.cpp
)
fp_embed_file(freepen_core "${PROJECT_SOURCE_DIR}/resources/schema.json" fp_schema_json)

target_include_directories(freepen_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(freepen_core PUBLIC fp_json fp_sqlite3 Threads::Threads)
