add_library(freepen_formats STATIC
    bibtex.cpp
    citation.cpp
    common.cpp
    csljson.cpp
    formats.cpp
    metadata.cpp
    ris.cpp
)
target_link_libraries(freepen_formats PUBLIC freepen_core PRIVATE fp_httplib)
