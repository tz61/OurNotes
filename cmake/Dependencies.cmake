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

# Windows looks for DLLs next to the program, and OpenSSL's are in its own bin directory
# (vcpkg, Miniconda, the Shining Light installer): on_copy_runtime_dlls() copies them
# beside a program after it is linked, and they are installed with the programs.
set(OURNOTES_RUNTIME_DLLS)
if(WIN32 AND OURNOTES_HAVE_OPENSSL AND NOT OPENSSL_USE_STATIC_LIBS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_ssl_arch "")
    elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64")
        set(_ssl_arch "-arm64")
    else()
        set(_ssl_arch "-x64")
    endif()
    get_filename_component(_ssl_prefix "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    set(_ssl_hints "${_ssl_prefix}/bin" "${_ssl_prefix}")
    if(OPENSSL_ROOT_DIR)
        list(APPEND _ssl_hints "${OPENSSL_ROOT_DIR}/bin" "${OPENSSL_ROOT_DIR}")
    endif()
    find_file(OURNOTES_LIBSSL_DLL libssl-3${_ssl_arch}.dll HINTS ${_ssl_hints} NO_DEFAULT_PATH)
    find_file(OURNOTES_LIBCRYPTO_DLL libcrypto-3${_ssl_arch}.dll HINTS ${_ssl_hints} NO_DEFAULT_PATH)
    if(OURNOTES_LIBSSL_DLL AND OURNOTES_LIBCRYPTO_DLL)
        set(OURNOTES_RUNTIME_DLLS "${OURNOTES_LIBSSL_DLL}" "${OURNOTES_LIBCRYPTO_DLL}")
        install(FILES ${OURNOTES_RUNTIME_DLLS} DESTINATION ${CMAKE_INSTALL_BINDIR})
        message(STATUS "OurNotes: OpenSSL DLLs: ${OURNOTES_RUNTIME_DLLS}")
    else()
        message(WARNING "libssl-3${_ssl_arch}.dll and libcrypto-3${_ssl_arch}.dll not found in ${_ssl_hints}: "
                        "the programs will only start with them on PATH")
    endif()
endif()

function(on_copy_runtime_dlls target)
    if(OURNOTES_RUNTIME_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${OURNOTES_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

if(OURNOTES_NEED_PDF)
    include(MuPDF)
endif()
