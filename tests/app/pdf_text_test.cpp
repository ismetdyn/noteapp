// app::pdf_page_blocks ve app::PdfTextLayer: yapılı PDF metninden biçimli bloklar (TUI) ve metin katmanı (GUI).
#include "app/pdf_text.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <vector>

namespace {

namespace pv = noteapp::pdfview;
using noteapp::app::pdf_page_blocks;
using noteapp::app::PdfTextLayer;
using noteapp::pdf::Block;

constexpr float kBody = 10.0F;
constexpr float kAdvance = 5.0F;  // her kod noktası 5 pt
constexpr float kLineHeight = 12.0F;

pv::TextSpan span(std::string text, float size = kBody, std::uint8_t style = 0) {
    return {.text = std::move(text), .style = style, .size = size, .edges = {}};
}

// Satır: parçaların kenarları `left`ten başlayarak sırayla; `top` satırın üstü.
pv::TextLine line(std::vector<pv::TextSpan> spans, float top, float left = 0.0F, bool joined = false) {
    float x = left;
    for (pv::TextSpan& part : spans) {
        for (const char byte : part.text) {
            if ((static_cast<unsigned char>(byte) & 0xC0U) == 0x80U)
                continue;
            part.edges.push_back(x);
            part.edges.push_back(x + kAdvance);
            x += kAdvance;
        }
    }
    return {.top = top, .bottom = top + kLineHeight, .joined = joined, .spans = std::move(spans)};
}

pv::PageText page(std::vector<pv::TextBlock> blocks) {
    return {.width = 600.0F, .height = 800.0F, .blocks = std::move(blocks)};
}

std::string text_of(const Block& block) {
    std::string out;
    for (const auto& part : block.spans)
        out += part.text;
    return out;
}

}  // namespace

TEST_CASE("pdf blocks: heading levels come from font size relative to the dominant body size") {
    const auto blocks = pdf_page_blocks(page({
        {.lines = {line({span("Title", 20.0F)}, 0)}},
        {.lines = {line({span("Section", 14.0F)}, 40)}},
        {.lines = {line({span("Minor", 11.5F)}, 70)}},
        {.lines = {line({span("Body text that is the most common size here")}, 100)}},
    }));
    REQUIRE(blocks.size() == 4);
    CHECK(blocks[0].kind == Block::Kind::Heading);
    CHECK(blocks[0].heading_level == 1);
    CHECK(blocks[1].heading_level == 2);
    CHECK(blocks[2].heading_level == 3);
    CHECK(blocks[3].kind == Block::Kind::Paragraph);
    CHECK(blocks[3].heading_level == 0);
}

TEST_CASE("pdf blocks: wrapped lines join into one paragraph, a hyphenated word joins without its hyphen") {
    const auto blocks = pdf_page_blocks(page({{.lines = {
                                                   line({span("The quick brown")}, 0),
                                                   line({span("fox jumps over the ex-")}, 13, 0, true),
                                                   line({span("ample dog.")}, 26),
                                               }}}));
    REQUIRE(blocks.size() == 1);
    CHECK(text_of(blocks[0]) == "The quick brown fox jumps over the example dog.");
}

TEST_CASE("pdf blocks: a wider line gap or a first-line indent starts a new paragraph") {
    const auto blocks = pdf_page_blocks(page({{.lines = {
                                                   line({span("First paragraph")}, 0),
                                                   line({span("goes on.")}, 13),
                                                   line({span("Second after a gap.")}, 40),
                                                   line({span("Third is indented")}, 53, 20.0F),
                                                   line({span("and continues.")}, 66),
                                               }}}));
    REQUIRE(blocks.size() == 3);
    CHECK(text_of(blocks[0]) == "First paragraph goes on.");
    CHECK(text_of(blocks[1]) == "Second after a gap.");
    CHECK(text_of(blocks[2]) == "Third is indented and continues.");
}

TEST_CASE("pdf blocks: list-like lines keep their line breaks") {
    const auto blocks = pdf_page_blocks(page({{.lines = {
                                                   line({span("Items:")}, 0),
                                                   line({span("• apples")}, 13),
                                                   line({span("- pears")}, 26),
                                                   line({span("12. plums")}, 39),
                                                   line({span("a) figs that wrap")}, 52),
                                                   line({span("onto a second line")}, 65),
                                               }}}));
    REQUIRE(blocks.size() == 5);
    CHECK(text_of(blocks[1]) == "• apples");
    CHECK(text_of(blocks[2]) == "- pears");
    CHECK(text_of(blocks[3]) == "12. plums");
    CHECK(text_of(blocks[4]) == "a) figs that wrap onto a second line");
}

TEST_CASE("pdf blocks: bold, italic and mono runs are kept; tabs and newlines become spaces") {
    const auto blocks = pdf_page_blocks(page({{.lines = {line(
                                                   {span("plain\t"), span("bold", kBody, pv::kStyleBold),
                                                    span(" and "), span("code", kBody, pv::kStyleMono | pv::kStyleItalic)},
                                                   0)}}}));
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].spans.size() == 4);
    CHECK(blocks[0].spans[0].text == "plain ");
    CHECK(blocks[0].spans[1].style.bold);
    CHECK_FALSE(blocks[0].spans[2].style.bold);
    CHECK(blocks[0].spans[3].style.mono);
    CHECK(blocks[0].spans[3].style.italic);
}

TEST_CASE("pdf blocks: an empty page and whitespace-only lines give no blocks") {
    CHECK(pdf_page_blocks(page({})).empty());
    CHECK(pdf_page_blocks(page({{.lines = {line({span("   ")}, 0)}}})).empty());
}

// Düşman sayfa: text_bytes'a sığan yüz binlerce yalnız boşluk satırı, biçim değiştirerek (parçalar birleşmez).
// Baştaki boşluk parçaları tek tek silinseydi O(N²) olurdu ve TUI'nin UI iş parçacığı donardı.
TEST_CASE("pdf blocks: many leading whitespace spans are trimmed in linear time") {
    constexpr std::size_t kLines = 200000;
    constexpr auto kBound = std::chrono::seconds(5);  // doğrusal yol Debug'da ~1 s altında
    std::vector<pv::TextLine> lines;
    lines.reserve(kLines + 1);
    for (std::size_t i = 0; i < kLines; ++i)
        lines.push_back(line({span(" ", kBody, i % 2 == 0 ? pv::kStyleBold : 0)}, 0.0F));
    lines.push_back(line({span("end")}, 0.0F));
    const auto started = std::chrono::steady_clock::now();
    const auto blocks = pdf_page_blocks(page({{.lines = std::move(lines)}}));
    CHECK(std::chrono::steady_clock::now() - started < kBound);
    REQUIRE(blocks.size() == 1);
    CHECK(text_of(blocks[0]) == "end");
}

TEST_CASE("pdf text layer: a caret follows the nearest line and the character midpoints") {
    const PdfTextLayer layer(page({{.lines = {line({span("abcd")}, 0), line({span("efgh")}, 13)}}}));
    REQUIRE(layer.size() == 8);
    CHECK(layer.caret_at(0.0F, 5.0F) == 0);
    CHECK(layer.caret_at(3.0F, 5.0F) == 1);    // 'a'nın ortasını geçti
    CHECK(layer.caret_at(100.0F, 5.0F) == 4);  // satır sonu
    CHECK(layer.caret_at(7.0F, 18.0F) == 5);
    CHECK(layer.caret_at(7.0F, 500.0F) == 5);  // en yakın satır: son satır
    CHECK(PdfTextLayer(page({})).caret_at(1.0F, 1.0F) == 0);
}

TEST_CASE("pdf text layer: selected text joins lines with a space, dehyphenates and breaks blocks") {
    const PdfTextLayer layer(page({
        {.lines = {line({span("one ex-")}, 0, 0, true), line({span("ample")}, 13), line({span("two")}, 26)}},
        {.lines = {line({span("çğ three")}, 60)}},
    }));
    CHECK(layer.text(0, layer.size()) == "one example two\nçğ three");
    CHECK(layer.text(4, 9) == "exam");  // "ex-" + "am": tire düşer
    CHECK(layer.text(2, 2).empty());
    CHECK(layer.text(15, 17) == "çğ");  // çok baytlı karakterler bütün kalır
}

TEST_CASE("pdf text layer: a double-click word stops at spaces and punctuation, one box per line") {
    const PdfTextLayer layer(page({{.lines = {line({span("hello, wörld")}, 0), line({span("next")}, 13)}}}));
    CHECK(layer.word_at(2.0F, 5.0F) == std::pair<std::size_t, std::size_t>{0, 5});
    CHECK(layer.word_at(27.0F, 5.0F) == std::pair<std::size_t, std::size_t>{5, 6});  // ','
    CHECK(layer.word_at(42.0F, 5.0F) == std::pair<std::size_t, std::size_t>{7, 12});
    const auto boxes = layer.boxes(3, 14);
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[0].left == 15.0F);
    CHECK(boxes[0].right == 60.0F);
    CHECK(boxes[0].top == 0.0F);
    CHECK(boxes[1].right == 10.0F);
    CHECK(boxes[1].bottom == 13.0F + kLineHeight);
}
