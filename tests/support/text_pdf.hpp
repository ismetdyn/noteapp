#pragma once

// Önizleme testlerinin küçük PDF'leri, libharu ve MuPDF bağlamadan (pdfview/fixtures.hpp'nin PdfBuilder'ı; app,
// cli, tui ve gui testleri motoru bağlamaz). Sayfalar 200x100 pt.
#include "pdfview/fixtures.hpp"

#include <format>
#include <string>
#include <vector>

namespace noteapp::test {

namespace text_pdf_detail {

inline int add_stream(pdftest::PdfBuilder& builder, const std::string& dictionary, const std::string& data) {
    return builder.add(std::format("<< /Length {} {} >>\nstream\n{}\nendstream", data.size(), dictionary, data));
}

struct Page {
    std::string resources;
    std::string content;
};

inline std::string finish(pdftest::PdfBuilder& builder, const std::vector<Page>& pages) {
    const int catalog = builder.reserve();
    const int tree = builder.reserve();
    std::string kids;
    for (const Page& spec : pages) {
        const int stream = add_stream(builder, "", spec.content);
        const int page = builder.add(
            std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 200 100] /Resources {} /Contents {} 0 R >>",
                        tree, spec.resources, stream));
        kids += std::format("{} 0 R ", page);
    }
    builder.set(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", tree));
    builder.set(tree, std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kids, pages.size()));
    return builder.build(catalog);
}

inline Page text_page(const std::string& text) {
    return {std::string(pdftest::kHelvetica), std::format("BT /F1 24 Tf 20 40 Td ({}) Tj ET", text)};
}

}  // namespace text_pdf_detail

// Sayfa başına bir ASCII satırı (Helvetica).
inline std::string text_pdf(const std::vector<std::string>& texts) {
    pdftest::PdfBuilder builder;
    std::vector<text_pdf_detail::Page> pages;
    for (const std::string& text : texts)
        pages.push_back(text_pdf_detail::text_page(text));
    return text_pdf_detail::finish(builder, pages);
}

// Sayfa 0 bitmez: 9 düzey, her düzey alttakini 10 kez çizen Form XObject (10^9 yaprak; fixtures fan_out_pdf gibi);
// süre sınırıyla kesilir. Sayfa 1 `second` yazar.
inline std::string slow_first_page_pdf(const std::string& second) {
    pdftest::PdfBuilder builder;
    int next = text_pdf_detail::add_stream(builder, "/Type /XObject /Subtype /Form /BBox [0 0 100 100]",
                                           "0 0 1 1 re f");
    for (int level = 0; level < 9; ++level) {
        std::string content;
        for (int i = 0; i < 10; ++i)
            content += "/N Do\n";
        next = text_pdf_detail::add_stream(
            builder,
            std::format("/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << /XObject << /N {} 0 R >> >>",
                        next),
            content);
    }
    return text_pdf_detail::finish(
        builder, {{std::format("<< /XObject << /F {} 0 R >> >>", next), "/F Do"}, text_pdf_detail::text_page(second)});
}

}  // namespace noteapp::test
