# MicroTeX (MIT): LaTeX math rendering for notes, with its Qt painter backend.
# Built here from its sources (its own CMakeLists builds GTK demos), together with
# tinyxml2 (zlib licence). The font resources it loads at runtime are staged to
# ${OURNOTES_MICROTEX_RES} in the build tree and installed to share/ournotes/microtex.

FetchContent_Declare(microtex
    URL https://github.com/NanoMichael/MicroTeX/archive/0e3707f6dafebb121d98b53c64364d16fefe481d.tar.gz
    URL_HASH SHA256=47476269d29c41df322bce6bdd2daa7017cc50b9eda841b2bec1767dba28daa6
    SOURCE_SUBDIR do-not-build
    ${OURNOTES_EXTRACT_TIMESTAMP})
FetchContent_Declare(tinyxml2
    URL https://github.com/leethomason/tinyxml2/archive/refs/tags/10.0.0.tar.gz
    URL_HASH SHA256=3bdf15128ba16686e69bce256cc468e76c7b94ff2c7f391cc5ec09e40bff3839
    SOURCE_SUBDIR do-not-build
    ${OURNOTES_EXTRACT_TIMESTAMP})
FetchContent_MakeAvailable(microtex tinyxml2)
include(PatchMicroTeX)

file(GLOB_RECURSE _microtex_sources CONFIGURE_DEPENDS "${microtex_SOURCE_DIR}/src/*.cpp")
list(FILTER _microtex_sources EXCLUDE REGEX "/src/(samples|platform)/")
list(APPEND _microtex_sources
    "${microtex_SOURCE_DIR}/src/platform/qt/graphic_qt.cpp"
    "${tinyxml2_SOURCE_DIR}/tinyxml2.cpp")

add_library(on_microtex STATIC ${_microtex_sources})
target_include_directories(on_microtex SYSTEM PUBLIC "${microtex_SOURCE_DIR}/src" "${tinyxml2_SOURCE_DIR}")
target_compile_definitions(on_microtex PUBLIC BUILD_QT)
target_link_libraries(on_microtex PUBLIC Qt6::Gui)
if(MSVC)
    target_compile_options(on_microtex PRIVATE /w /utf-8)
else()
    target_compile_options(on_microtex PRIVATE -w)
endif()

set(OURNOTES_MICROTEX_RES "${CMAKE_BINARY_DIR}/share/ournotes/microtex")
file(COPY "${microtex_SOURCE_DIR}/res/" DESTINATION "${OURNOTES_MICROTEX_RES}")
install(DIRECTORY "${microtex_SOURCE_DIR}/res/" DESTINATION "${CMAKE_INSTALL_DATADIR}/ournotes/microtex")
