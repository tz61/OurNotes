#pragma once

#include <fstream>
#include <string>

#include "core/util.h"

// A one-page PDF (US letter) with two lines of Helvetica text. Offsets in the
// xref table are not exact; MuPDF repairs them, which is fine for tests.
inline void writeSamplePdf(const on::fs::path& path, int rotate = 0)
{
    std::string content = "BT /F1 24 Tf 72 700 Td (Hello World from OurNotes) Tj ET\n"
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

// A PDF of `pages` US-letter pages, each with its number as text.
inline void writeManyPagesPdf(const on::fs::path& path, int pages)
{
    std::string kids, objects;
    for (int i = 0; i < pages; ++i) {
        int page = 5 + 2 * i, content = page + 1;
        std::string text = "BT /F1 48 Tf 72 600 Td (Page " + std::to_string(i + 1) + ") Tj ET\n";
        kids += std::to_string(page) + " 0 R ";
        objects += std::to_string(page) + " 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents " +
                   std::to_string(content) + " 0 R /Resources << /Font << /F1 4 0 R >> >> >> endobj\n" +
                   std::to_string(content) + " 0 obj << /Length " + std::to_string(text.size()) + " >> stream\n" +
                   text + "endstream endobj\n";
    }
    std::string pdf = "%PDF-1.4\n"
                      "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
                      "2 0 obj << /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " >> endobj\n"
                      "4 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n" + objects +
                      "xref\n0 1\n0000000000 65535 f \n"
                      "trailer << /Size " + std::to_string(5 + 2 * pages) + " /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    std::ofstream(path, std::ios::binary) << pdf;
}

// A one-page PDF with a line of text and a red 2x2 image drawn as a 100x100 pt
// square with its lower-left corner at (72, 500). With `clipped`, only the left half
// of the image is visible.
inline void writeImagePdf(const on::fs::path& path, bool clipped = false)
{
    std::string content = "BT /F1 24 Tf 72 700 Td (Text above a picture) Tj ET\n";
    content += clipped ? "q 72 500 50 100 re W n q 100 0 0 100 72 500 cm /Im1 Do Q Q\n" : "q 100 0 0 100 72 500 cm /Im1 Do Q\n";
    std::string pixels;
    for (int i = 0; i < 4; ++i)
        pixels += std::string("\xff\x00\x00", 3);
    std::string pdf = "%PDF-1.4\n"
                      "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
                      "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
                      "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
                      "/Resources << /Font << /F1 5 0 R >> /XObject << /Im1 6 0 R >> >> >> endobj\n"
                      "4 0 obj << /Length " + std::to_string(content.size()) + " >> stream\n" + content +
                      "endstream endobj\n"
                      "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
                      "6 0 obj << /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB "
                      "/BitsPerComponent 8 /Length 12 >> stream\n" + pixels +
                      "\nendstream endobj\n"
                      "xref\n0 7\n0000000000 65535 f \n"
                      "trailer << /Size 7 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    std::ofstream(path, std::ios::binary) << pdf;
}
