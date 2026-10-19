add_library(ournotes_formats STATIC
    bibtex.cpp
    citation.cpp
    common.cpp
    csljson.cpp
    formats.cpp
    metadata.cpp
    pdf_metadata.cpp
    ris.cpp
)
target_link_libraries(ournotes_formats PUBLIC ournotes_core ournotes_pdf PRIVATE on_httplib)
