# Third-party dependencies.
#
#  * nlohmann/json, cpp-httplib, doctest: vendored single headers in third_party/
#  * SQLite: amalgamation fetched at configure time (override with
#    -DFETCHCONTENT_SOURCE_DIR_SQLITE=/path/to/amalgamation for offline builds)
#  * OpenSSL: optional, enables https:// in cpp-httplib
#  * MuPDF: see cmake/MuPDF.cmake (only when the GUI or CLI is built)

include(FetchContent)
find_package(Threads REQUIRED)

# DOWNLOAD_EXTRACT_TIMESTAMP only exists since CMake 3.24.
set(OURNOTES_EXTRACT_TIMESTAMP)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(OURNOTES_EXTRACT_TIMESTAMP DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

set(OURNOTES_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_library(on_json INTERFACE)
target_include_directories(on_json SYSTEM INTERFACE "${OURNOTES_THIRD_PARTY}/nlohmann")

add_library(on_doctest INTERFACE)
target_include_directories(on_doctest SYSTEM INTERFACE "${OURNOTES_THIRD_PARTY}/doctest")

# --- SQLite -------------------------------------------------------------------
FetchContent_Declare(sqlite
    URL https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    ${OURNOTES_EXTRACT_TIMESTAMP})
FetchContent_MakeAvailable(sqlite)
add_library(on_sqlite3 STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(on_sqlite3 SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(on_sqlite3 PUBLIC
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_DEFAULT_FOREIGN_KEYS=0
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION)
if(NOT MSVC)
    target_compile_options(on_sqlite3 PRIVATE -w)
endif()
target_link_libraries(on_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# --- cpp-httplib (+ OpenSSL) -----------------------------------------------------
add_library(on_httplib INTERFACE)
target_include_directories(on_httplib SYSTEM INTERFACE "${OURNOTES_THIRD_PARTY}/httplib")
target_link_libraries(on_httplib INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(on_httplib INTERFACE ws2_32 crypt32)
endif()
set(OURNOTES_HAVE_OPENSSL OFF)
if(OURNOTES_WITH_OPENSSL)
    find_package(OpenSSL 3.0)
    if(OpenSSL_FOUND)
        set(OURNOTES_HAVE_OPENSSL ON)
        target_compile_definitions(on_httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(on_httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
        if(APPLE)
            target_compile_definitions(on_httplib INTERFACE CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
            target_link_libraries(on_httplib INTERFACE "-framework CoreFoundation" "-framework Security")
        endif()
    else()
        message(WARNING "OpenSSL >= 3.0 not found: https:// URLs (TLS sync, metadata lookup) are disabled")
    endif()
endif()
message(STATUS "OurNotes: HTTPS support: ${OURNOTES_HAVE_OPENSSL}")

if(OURNOTES_NEED_PDF)
    include(MuPDF)
endif()
