add_library(freepen_pdf STATIC document.cpp)
target_link_libraries(freepen_pdf PUBLIC freepen_core PRIVATE MuPDF::MuPDF)
