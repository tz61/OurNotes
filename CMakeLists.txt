cmake_minimum_required(VERSION 3.21)

project(freePen
    VERSION 0.1.0
    DESCRIPTION "Open-source reference manager with PDF annotation, notes and sync"
    HOMEPAGE_URL "https://github.com/freepen/freepen"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(FREEPEN_BUILD_GUI "Build the Qt desktop application" ON)
option(FREEPEN_BUILD_CLI "Build freepen-cli" ON)
option(FREEPEN_BUILD_SERVER "Build the freepend sync daemon" ON)
option(FREEPEN_BUILD_TESTS "Build unit tests" ON)
option(FREEPEN_WITH_OPENSSL "Enable HTTPS (TLS sync, metadata lookup)" ON)
option(FREEPEN_BUILD_TOOLS "Build developer tools (icon rendering)" OFF)

# The sync daemon alone needs neither Qt nor MuPDF.
set(FREEPEN_NEED_PDF OFF)
if(FREEPEN_BUILD_GUI OR FREEPEN_BUILD_CLI)
    set(FREEPEN_NEED_PDF ON)
endif()

include(GNUInstallDirs)
include(Dependencies)
include(EmbedFile)

if(MSVC)
    add_compile_options(/W3 /utf-8 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/freepen/version.h" @ONLY)

# Skip components whose sources are not present (lets partial checkouts build).
macro(fp_add_component dir)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif()
endmacro()

fp_add_component(src/core)
fp_add_component(src/sync)
if(FREEPEN_NEED_PDF)
    fp_add_component(src/pdf)
    fp_add_component(src/formats)
endif()
if(FREEPEN_BUILD_SERVER)
    fp_add_component(src/server)
endif()
if(FREEPEN_BUILD_CLI)
    fp_add_component(src/cli)
endif()
if(FREEPEN_BUILD_GUI)
    fp_add_component(src/gui)
endif()

if(FREEPEN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(FREEPEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
