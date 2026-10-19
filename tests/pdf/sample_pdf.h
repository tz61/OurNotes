#pragma once

#include <fstream>
#include <string>

#include "core/util.h"

// A one-page PDF (US letter) with two lines of Helvetica text. Offsets in the
// xref table are not exact; MuPDF repairs them, which is fine for tests.
inline void writeSamplePdf(const fp::fs::path& path, int rotate = 0)
{
    std::string content = "BT /F1 24 Tf 72 700 Td (Hello World from freePen) Tj ET\n"
                          "BT /F1 24 Tf 72 660 Td (second line of text) Tj ET\n";
    std::string pdf = "%PDF-1.4\n"
                      "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
                      "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
                      "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate " +
                      std::to_string(rotate) +
                      " /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
                      "4 0 obj << /Length " + std::to_string(content.size()) + " >> stream\n" + content +
                      "endstream endobj\n"
                      "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
                      "xref\n0 6\n0000000000 65535 f \n"
                      "trailer << /Size 6 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    std::ofstream(path, std::ios::binary) << pdf;
}
