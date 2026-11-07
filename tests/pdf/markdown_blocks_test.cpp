// T82: md4c yürüyüşünün blok çıktısı. PDF hedefleri hiç taşımaz; GUI önizlemesi `links` ile bağlantı
// hedeflerini ve [[wiki]] bağlantılarını ister.
#include "pdf/markdown_blocks.hpp"
#include "pdf/md4c_parse.hpp"

#include <doctest/doctest.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using noteapp::pdf::Block;
using noteapp::pdf::parse_markdown;
using noteapp::pdf::Span;

namespace {

constexpr char kSample[] = "# Başlık\n\n"
                           "- madde [[hedef|takma ad]] ve [site](https://x.example/y)\n"
                           "- *vurgu* ve **kalın**\n\n"
                           "```\nkod satırı\n```\n";

const Span* find_span(const std::vector<Block>& blocks, const std::string& text) {
    for (const Block& block : blocks) {
        for (const Span& span : block.spans) {
            if (span.text == text)
                return &span;
        }
    }
    return nullptr;
}

std::string text_of(const Block& block) {
    std::string text;
    for (const Span& span : block.spans)
        text += span.text;
    return text;
}

}  // namespace

TEST_CASE("the walk gives heading, list items, emphasis and code blocks") {
    const auto blocks = parse_markdown(kSample, {.links = true});
    REQUIRE(blocks.size() == 4);
    CHECK(blocks[0].kind == Block::Kind::Heading);
    CHECK(blocks[0].heading_level == 1);
    CHECK(text_of(blocks[0]) == "Başlık");
    CHECK(blocks[1].kind == Block::Kind::Paragraph);
    CHECK(blocks[1].list_depth == 1);
    CHECK(blocks[1].marker == "• ");
    CHECK(text_of(blocks[2]) == "vurgu ve kalın");
    REQUIRE(find_span(blocks, "vurgu") != nullptr);
    CHECK(find_span(blocks, "vurgu")->style.italic);
    REQUIRE(find_span(blocks, "kalın") != nullptr);
    CHECK(find_span(blocks, "kalın")->style.bold);
    CHECK(blocks[3].kind == Block::Kind::Code);
    CHECK(text_of(blocks[3]) == "kod satırı\n");
    CHECK(blocks[3].spans.front().style.mono);
}

TEST_CASE("link targets and wiki links are carried only when asked") {
    const auto linked = parse_markdown(kSample, {.links = true});
    const Span* wiki = find_span(linked, "takma ad");
    REQUIRE(wiki != nullptr);
    CHECK(wiki->link == "hedef");
    CHECK(wiki->wiki);
    const Span* site = find_span(linked, "site");
    REQUIRE(site != nullptr);
    CHECK(site->link == "https://x.example/y");
    CHECK_FALSE(site->wiki);
    REQUIRE(find_span(linked, " ve ") != nullptr);
    CHECK(find_span(linked, " ve ")->link.empty());

    // PDF: hedef yok, [[...]] düz metin kalır (dışa aktarma çıktısı değişmez).
    const auto plain = parse_markdown(kSample);
    CHECK(text_of(plain[1]) == "madde [[hedef|takma ad]] ve site");
    for (const Block& block : plain) {
        for (const Span& span : block.spans) {
            CHECK(span.link.empty());
            CHECK_FALSE(span.wiki);
        }
    }
}

// SD5a: blok kap yolunun tamamını taşısaydı "- - - …" (iç içe liste) bellekte derinliğin karesi olurdu (64 KiB →
// 512 MB). Bloklardaki kap verisi girdiyle doğrusal kalır; ölçü bayttır, süre değil.
TEST_CASE("deep list nesting keeps the per-block container data linear in the input") {
    constexpr std::size_t kDepth = 32000;
    std::string text;
    for (std::size_t i = 0; i < kDepth; ++i)
        text += "- ";
    text += "x\n";
    const auto blocks = parse_markdown(text);
    std::size_t container_bytes = 0;
    for (const Block& block : blocks)
        container_bytes += block.nesting_opened.size();
    CHECK(blocks.size() >= kDepth);  // her öğe kendi bloğunu verdi: boş çıktı yanlış yeşil olmasın
    CHECK(container_bytes <= text.size());
}

namespace {

// İlk metinde fırlatan işleyici (bellek yetmezliği gibi): md4c -fexceptions'sız C'dir, exception onun
// çerçevelerinden geçmemeli.
struct ThrowingHandler {
    int texts = 0;
    void enter_block(MD_BLOCKTYPE /*type*/, const void* /*detail*/) {}
    void leave_block(MD_BLOCKTYPE /*type*/) {}
    void enter_span(MD_SPANTYPE /*type*/, const void* /*detail*/) {}
    void leave_span(MD_SPANTYPE /*type*/) {}
    void text(MD_TEXTTYPE /*type*/, std::string_view /*value*/) {
        ++texts;
        throw std::runtime_error("handler failed");
    }
};

}  // namespace

TEST_CASE("md4c: a handler exception stops the parse and reaches the caller after md4c returns") {
    ThrowingHandler handler;
    CHECK_THROWS_WITH_AS(
        noteapp::pdf::parse_md4c("first *one*\n\nsecond\n", noteapp::pdf::kMarkdownFlags, handler),
        "handler failed", std::runtime_error);
    CHECK(handler.texts == 1);  // ayrıştırma ilk hatada durdu
}
