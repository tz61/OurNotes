# Locate MuPDF and expose the imported target MuPDF::MuPDF.
#
# Resolution order:
#   1. -DMUPDF_ROOT=<prefix>   prefix containing include/mupdf and lib/libmupdf(.a|.lib)
#                              (+ libmupdf-third / libthirdparty when built from source)
#   2. a system installation (Homebrew, distro packages)
#   3. OURNOTES_BUNDLE_MUPDF=ON (default): download and build MuPDF, with make on Unix
#      and with MSBuild (MuPDF's Visual Studio projects) on Windows.

set(MUPDF_ROOT "" CACHE PATH "Prefix of a MuPDF installation")
set(OURNOTES_MUPDF_VERSION "1.28.5")
set(OURNOTES_MUPDF_SHA256 "98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934")
if(UNIX OR MSVC)
    option(OURNOTES_BUNDLE_MUPDF "Download and build MuPDF if it is not found" ON)
else()
    option(OURNOTES_BUNDLE_MUPDF "Download and build MuPDF if it is not found" OFF)
endif()

set(_mupdf_hints)
if(MUPDF_ROOT)
    set(_mupdf_hints HINTS "${MUPDF_ROOT}" NO_DEFAULT_PATH)
endif()
find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h ${_mupdf_hints} PATH_SUFFIXES include)
find_library(MUPDF_LIBRARY NAMES mupdf libmupdf ${_mupdf_hints} PATH_SUFFIXES lib lib64)
find_library(MUPDF_THIRD_LIBRARY NAMES mupdf-third libmupdf-third libthirdparty ${_mupdf_hints} PATH_SUFFIXES lib lib64)

add_library(MuPDF::MuPDF INTERFACE IMPORTED GLOBAL)

# vcpkg's port (Windows) exports a CMake package with all dependencies.
if(NOT MUPDF_ROOT)
    find_package(unofficial-mupdf CONFIG QUIET)
endif()

if(TARGET unofficial::mupdf::mupdf)
    message(STATUS "OurNotes: using MuPDF from vcpkg")
    target_link_libraries(MuPDF::MuPDF INTERFACE unofficial::mupdf::mupdf)
elseif(MUPDF_INCLUDE_DIR AND MUPDF_LIBRARY)
    message(STATUS "OurNotes: using MuPDF from ${MUPDF_LIBRARY}")
    target_include_directories(MuPDF::MuPDF INTERFACE "${MUPDF_INCLUDE_DIR}")
    target_link_libraries(MuPDF::MuPDF INTERFACE "${MUPDF_LIBRARY}")
    if(MUPDF_THIRD_LIBRARY)
        target_link_libraries(MuPDF::MuPDF INTERFACE "${MUPDF_THIRD_LIBRARY}")
    else()
        # Distribution builds link the third-party libraries dynamically.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MUPDF_DEPS QUIET IMPORTED_TARGET freetype2 harfbuzz libopenjp2 jbig2dec libjpeg zlib gumbo)
            if(MUPDF_DEPS_FOUND)
                target_link_libraries(MuPDF::MuPDF INTERFACE PkgConfig::MUPDF_DEPS)
            endif()
        endif()
    endif()
elseif(OURNOTES_BUNDLE_MUPDF AND MSVC)
    # libmupdf.vcxproj builds the third-party libraries too and merges them into
    # libmupdf.lib. Its Debug and Release configurations use the same C runtimes (/MDd,
    # /MD) as CMake's, so the one matching the build configuration is built.
    include(ExternalProject)
    if(CMAKE_VS_MSBUILD_COMMAND)
        set(_msbuild "${CMAKE_VS_MSBUILD_COMMAND}")
    else()
        find_program(_msbuild NAMES msbuild REQUIRED)
    endif()
    if(CMAKE_VS_PLATFORM_NAME)
        set(_platform "${CMAKE_VS_PLATFORM_NAME}")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_platform Win32)
    elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
        set(_platform ARM64)
    else()
        set(_platform x64)
    endif()
    set(_prefix "${CMAKE_BINARY_DIR}/mupdf")
    set(_config "$<IF:$<CONFIG:Debug>,Debug,Release>")
    ExternalProject_Add(mupdf_ext
        URL "https://mupdf.com/downloads/archive/mupdf-${OURNOTES_MUPDF_VERSION}-source.tar.gz"
        URL_HASH SHA256=${OURNOTES_MUPDF_SHA256}
        ${OURNOTES_EXTRACT_TIMESTAMP}
        PREFIX "${_prefix}"
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND ""
        # Built through the solution: it maps the bin2coff tool (which embeds the fonts and
        # only has a Release|Win32 configuration) to Win32 when building for x64; building
        # libmupdf.vcxproj alone would ask bin2coff for Release|x64 and fail (MSB8013).
        # The projects ask for Visual Studio 2019's toolset (use the one CMake found) and
        # link-time code generation (which every program linking libmupdf.lib would redo).
        BUILD_COMMAND "${_msbuild}" platform/win32/mupdf.sln -t:libmupdf -m -nologo -verbosity:minimal
            "-p:Configuration=${_config}" "-p:Platform=${_platform}"
            "-p:PlatformToolset=v${MSVC_TOOLSET_VERSION}" -p:WindowsTargetPlatformVersion=10.0
            -p:WholeProgramOptimization=false
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy_directory include "${_prefix}/include"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_prefix}/lib"
            COMMAND "${CMAKE_COMMAND}" -E copy "platform/win32/${_platform}/${_config}/libmupdf.lib" "${_prefix}/lib/libmupdf.lib"
        BUILD_BYPRODUCTS "${_prefix}/lib/libmupdf.lib")
    file(MAKE_DIRECTORY "${_prefix}/include")
    add_dependencies(MuPDF::MuPDF mupdf_ext)
    target_include_directories(MuPDF::MuPDF INTERFACE "${_prefix}/include")
    target_link_libraries(MuPDF::MuPDF INTERFACE "${_prefix}/lib/libmupdf.lib")
    message(STATUS "OurNotes: MuPDF ${OURNOTES_MUPDF_VERSION} will be built from source with MSBuild")
elseif(OURNOTES_BUNDLE_MUPDF AND UNIX)
    include(ExternalProject)
    find_program(MAKE_EXECUTABLE NAMES gmake make REQUIRED)
    set(_prefix "${CMAKE_BINARY_DIR}/mupdf")
    set(_flags build=release HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no mujs=no "XCFLAGS=-fPIC")
    ExternalProject_Add(mupdf_ext
        URL "https://mupdf.com/downloads/archive/mupdf-${OURNOTES_MUPDF_VERSION}-source.tar.gz"
        URL_HASH SHA256=${OURNOTES_MUPDF_SHA256}
        ${OURNOTES_EXTRACT_TIMESTAMP}
        PREFIX "${_prefix}"
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${MAKE_EXECUTABLE} -j8 ${_flags} libs
        INSTALL_COMMAND ${MAKE_EXECUTABLE} ${_flags} "prefix=${_prefix}" install-libs
        BUILD_BYPRODUCTS "${_prefix}/lib/libmupdf.a" "${_prefix}/lib/libmupdf-third.a")
    file(MAKE_DIRECTORY "${_prefix}/include")
    add_dependencies(MuPDF::MuPDF mupdf_ext)
    target_include_directories(MuPDF::MuPDF INTERFACE "${_prefix}/include")
    target_link_libraries(MuPDF::MuPDF INTERFACE "${_prefix}/lib/libmupdf.a" "${_prefix}/lib/libmupdf-third.a")
    message(STATUS "OurNotes: MuPDF ${OURNOTES_MUPDF_VERSION} will be built from source")
else()
    message(FATAL_ERROR "MuPDF not found. Set -DMUPDF_ROOT=<prefix> (see README).")
endif()

if(NOT WIN32)
    target_link_libraries(MuPDF::MuPDF INTERFACE m Threads::Threads)
endif()

# MuPDF's static libraries contain their own FreeType. On ELF platforms the linker
# would export those FT_* symbols from our executables, and Qt/fontconfig (which load
# the system FreeType) would then call MuPDF's differently configured copy. Keep them
# private. (macOS two-level namespaces and Windows DLL imports are not affected.)
if(UNIX AND NOT APPLE)
    target_link_options(MuPDF::MuPDF INTERFACE
        "LINKER:--exclude-libs,libmupdf.a" "LINKER:--exclude-libs,libmupdf-third.a")
endif()
