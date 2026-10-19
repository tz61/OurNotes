add_library(ournotes_pdf STATIC document.cpp)
target_link_libraries(ournotes_pdf PUBLIC ournotes_core PRIVATE MuPDF::MuPDF)
