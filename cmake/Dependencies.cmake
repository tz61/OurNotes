# Third-party dependencies.
#
#  * nlohmann/json, cpp-httplib, doctest: vendored single headers in third_party/
#  * SQLite: amalgamation fetched at configure time (override with
#    -DFETCHCONTENT_SOURCE_DIR_SQLITE=/path/to/amalgamation for offline builds)
#  * OpenSSL: optional, enables https:// in cpp-httplib
#  * MuPDF: see cmake/MuPDF.cmake (only when the GUI or CLI is built)

include(FetchContent)
find_package(Threads REQUIRED)

set(FREEPEN_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_library(fp_json INTERFACE)
target_include_directories(fp_json SYSTEM INTERFACE "${FREEPEN_THIRD_PARTY}/nlohmann")

add_library(fp_doctest INTERFACE)
target_include_directories(fp_doctest SYSTEM INTERFACE "${FREEPEN_THIRD_PARTY}/doctest")

# --- SQLite -------------------------------------------------------------------
FetchContent_Declare(sqlite
    URL https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite)
add_library(fp_sqlite3 STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(fp_sqlite3 SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(fp_sqlite3 PUBLIC
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_DEFAULT_FOREIGN_KEYS=0
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION)
if(NOT MSVC)
    target_compile_options(fp_sqlite3 PRIVATE -w)
endif()
target_link_libraries(fp_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# --- cpp-httplib (+ OpenSSL) -----------------------------------------------------
add_library(fp_httplib INTERFACE)
target_include_directories(fp_httplib SYSTEM INTERFACE "${FREEPEN_THIRD_PARTY}/httplib")
target_link_libraries(fp_httplib INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(fp_httplib INTERFACE ws2_32 crypt32)
endif()
set(FREEPEN_HAVE_OPENSSL OFF)
if(FREEPEN_WITH_OPENSSL)
    find_package(OpenSSL 3.0)
    if(OpenSSL_FOUND)
        set(FREEPEN_HAVE_OPENSSL ON)
        target_compile_definitions(fp_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(fp_httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
        if(APPLE)
            target_compile_definitions(fp_httplib INTERFACE CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
            target_link_libraries(fp_httplib INTERFACE "-framework CoreFoundation" "-framework Security")
        endif()
    else()
        message(WARNING "OpenSSL >= 3.0 not found: https:// URLs (TLS sync, metadata lookup) are disabled")
    endif()
endif()
message(STATUS "freePen: HTTPS support: ${FREEPEN_HAVE_OPENSSL}")

if(FREEPEN_NEED_PDF)
    include(MuPDF)
endif()
