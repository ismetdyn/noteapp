// A7: CSV/JSON önizlemesinin ortak ayrıştırması (TUI REVIEW ve GUI önizleme aynı kuralı kullanır). Görüntü
// testleri arayüzlerde: tests/tui/markdown_view_test.cpp, tests/gui/preview_test.cpp.
#include "app/data_preview.hpp"

#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

using noteapp::app::PreviewKind;

TEST_CASE("preview kind comes from the extension, case-insensitive") {
    CHECK(noteapp::app::preview_kind_for("md") == PreviewKind::Markdown);
    CHECK(noteapp::app::preview_kind_for("MARKDOWN") == PreviewKind::Markdown);
    CHECK(noteapp::app::preview_kind_for("Csv") == PreviewKind::Csv);
    CHECK(noteapp::app::preview_kind_for("json") == PreviewKind::Json);
    CHECK(noteapp::app::preview_kind_for("txt") == PreviewKind::None);
    CHECK(noteapp::app::preview_kind_for("") == PreviewKind::None);
}

TEST_CASE("csv cells keep their bytes, quotes unfold and rows know their source offset") {
    const std::string_view bom = "\xEF\xBB\xBF";
    const std::string source =
        std::string(bom) + "ad;şehir\r\nÇağrı;\"İstanbul; \"\"merkez\"\"\"\r\n\r\nAli;Van\n";
    const auto table = noteapp::app::parse_csv(source);
    REQUIRE(table);
    const std::vector<std::vector<std::string>> expected = {
        {"ad", "şehir"}, {"Çağrı", "İstanbul; \"merkez\""}, {"Ali", "Van"}};
    CHECK(table->rows == expected);  // boş satır atlanır, \r düşer
    CHECK(table->column_count == 2);
    REQUIRE(table->row_offsets.size() == 3);
    CHECK(table->row_offsets[0] == bom.size());
    CHECK(source.substr(table->row_offsets[1]).starts_with("Çağrı"));
    CHECK(source.substr(table->row_offsets[2]).starts_with("Ali"));
    CHECK_FALSE(noteapp::app::parse_csv("tek sütun\nbaşka satır\n"));
    CHECK_FALSE(noteapp::app::parse_csv(""));
}

TEST_CASE("csv limits keep rows and columns but count what they drop") {
    std::string source = "a,b,c\n";
    for (int row = 0; row < 5; ++row)
        source += "1,2,3\n";
    const auto table = noteapp::app::parse_csv(source, {.rows = 3, .columns = 2});
    REQUIRE(table);
    CHECK(table->rows.size() == 3);
    CHECK(table->rows.front() == std::vector<std::string>{"a", "b"});
    CHECK(table->column_count == 3);  // kaynaktaki genişlik
    CHECK(table->omitted_rows == 3);
}

TEST_CASE("json pretty lines carry their depth and the offset of their first token") {
    const std::string source = "{\"ad\":\"Çağrı\",\n\"liste\":[1,{}],\"n\":null}";
    const auto lines = noteapp::app::pretty_json(source);
    REQUIRE(lines);
    std::vector<std::pair<std::size_t, std::string>> shown;
    for (const auto& line : *lines)
        shown.emplace_back(line.depth, line.text);
    const std::vector<std::pair<std::size_t, std::string>> expected = {{0, "{"},
                                                                       {1, "\"ad\": \"Çağrı\","},
                                                                       {1, "\"liste\": ["},
                                                                       {2, "1,"},
                                                                       {2, "{}"},
                                                                       {1, "],"},
                                                                       {1, "\"n\": null"},
                                                                       {0, "}"}};
    CHECK(shown == expected);
    CHECK(source.substr((*lines)[2].offset).starts_with("\"liste\""));
    const auto with_bom = noteapp::app::pretty_json("\xEF\xBB\xBF[1]");
    REQUIRE(with_bom);
    CHECK((*with_bom)[0].offset == 3);
    CHECK_FALSE(noteapp::app::pretty_json("[1,]"));
    CHECK_FALSE(noteapp::app::pretty_json(std::string(100000, '[')));
}

namespace {

// Parçaların ilk baytları: kesimler yalnız blok sınırında.
std::vector<std::string> chunk_starts(std::string_view text, std::size_t target) {
    constexpr std::size_t kShown = 6;
    std::vector<std::string> starts;
    for (const noteapp::app::MarkdownChunk& chunk : noteapp::app::markdown_chunks(text, target))
        starts.emplace_back(text.substr(chunk.begin, std::min(chunk.end - chunk.begin, kShown)));
    return starts;
}

// Toplanan ilk tanım satırları, belge sırasıyla.
std::vector<std::string> definition_lines(std::string_view text) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (const noteapp::app::MarkdownDefinition& definition : noteapp::app::markdown_link_definitions(text).first)
        spans.emplace_back(definition.begin, definition.end);
    std::ranges::sort(spans);
    std::vector<std::string> lines;
    for (const auto& [begin, end] : spans)
        lines.emplace_back(text.substr(begin, end - begin));
    return lines;
}

}  // namespace

TEST_CASE("markdown chunks cover the note, know their first line and cut only between top-level blocks") {
    const std::string text = "# Bir\n\nmetin\n\n- liste\n\n  devam\n\n> alıntı\n\n1. sıra\n\n"
                             "```\nkod\n\n# kodda\n```\n\n    girintili\n\nSon\n";
    const auto chunks = noteapp::app::markdown_chunks(text, 1);
    REQUIRE_FALSE(chunks.empty());
    CHECK(chunks.front().begin == 0);
    CHECK(chunks.back().end == text.size());
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        CHECK(chunks[i].begin == chunks[i - 1].end);
        const auto lines_before = std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(chunks[i].begin), '\n');
        CHECK(chunks[i].first_line == static_cast<std::size_t>(lines_before));
    }
    // Kuşatılmış koddaki "# kodda", liste devamı, alıntı, sıralı liste ve girintili kod kesim yeri değil.
    CHECK(chunk_starts(text, 1) == std::vector<std::string>{"# Bir\n", "metin\n", "```\nko", "Son\n"});
}

TEST_CASE("markdown chunks keep a small note whole and reach the target size before cutting") {
    const std::string text = "# a\n\nb\n\n# c\n\nd\n";
    CHECK(noteapp::app::markdown_chunks(text).size() == 1);
    CHECK(noteapp::app::markdown_chunks("").size() == 1);
    CHECK(chunk_starts(text, 6) == std::vector<std::string>{"# a\n\nb", "# c\n\nd"});
    // Bilgi dizgesinde ters tırnak olan satır çit değildir: ardındaki başlık kesim yeridir.
    CHECK(chunk_starts("``` a`b\n\n# x\n", 1) == std::vector<std::string>{"``` a`", "# x\n"});
    // Kısa kapanış (```` açılışında ```) kodu kapatmaz.
    CHECK(chunk_starts("````\n```\n\n# x\n````\n\n# y\n", 1) == std::vector<std::string>{"````\n`", "# y\n"});
}

// Liste öğesindeki girintili çit, girintisiz satırla (öğe biter) kapanır; izleyici bunu çitin içeriği sanırsa
// sonraki gerçek çiti kapanış sayar ve kodun içinden keserdi. Emin olunmayınca kesilmez.
TEST_CASE("markdown chunks never cut inside code after a fence in a list item ends with its item") {
    std::string text = "- a\n  ```\n  x\n\ntext\n\n";
    while (text.size() < 2 * noteapp::app::kMarkdownChunkBytes)
        text += "## Bölüm\n\nparagraf\n\n";
    text += "```\nx\n\ndef f():\n```\n\nson\n";
    for (const noteapp::app::MarkdownChunk& chunk : noteapp::app::markdown_chunks(text, 1))
        CHECK_FALSE(text.substr(chunk.begin).starts_with("def f():"));
}

// NX-E F2: liste imiyle aynı satırdaki çit (`- ```bash`) de çittir; kaçırılırsa kapanışı açılış sayılır, sonraki
// gerçek çit kapanış sayılır ve kodun içinden kesilir.
TEST_CASE("markdown chunks see a fence opened on a list marker line") {
    for (const std::string_view item : {"- ```bash\n  echo\n  ```\n\n", "12) ~~~\n    x\n    ~~~\n\n"}) {
        CAPTURE(item);
        std::string text(item);
        while (text.size() < 2 * noteapp::app::kMarkdownChunkBytes)
            text += "## Bölüm\n\nparagraf\n\n";
        text += "```\nx\n\ndef f():\n```\n\nson\n";
        const auto chunks = noteapp::app::markdown_chunks(text, 1);
        CHECK(chunks.size() > 2);  // kesimler sürer
        for (const noteapp::app::MarkdownChunk& chunk : chunks)
            CHECK_FALSE(text.substr(chunk.begin).starts_with("def f():"));
    }
}

// Tanımlar her parçanın ardına eklenir: yalnız md4c'nin de tanım sayacağı tek satırlar (yoksa parçanın sonunda
// metin görünürdü), kuşatılmış kodun içindekiler değil.
TEST_CASE("markdown link definitions are collected outside code fences, single-line and strict") {
    const std::string text = "[a]: https://a.example\n"
                             "   [B c]: <https://b.example/x y> \"başlık\"\r\n"
                             "[p]: https://w.example/A_(b) (paren)\n"
                             "```\n[kod]: https://kod.example\n```\n\n"
                             "    [girinti]: https://kod.example\n\n"
                             "> [alıntı]: https://q.example\n\n"
                             "[eksik]:\n\n"
                             "[dengesiz]: https://x.example/(a\n\n"
                             "[başlık]: https://x.example \"kapanmaz\n\n"
                             "[bitişik]: https://x.example\"t\"\n\n"
                             "[[iç]]: https://x.example\n\n"
                             "paragraf\n[devam]: https://x.example\n";  // paragraf devamı tanım değil
    CHECK(definition_lines(text) == std::vector<std::string>{"[a]: https://a.example",
                                                             "   [B c]: <https://b.example/x y> \"başlık\"\r",
                                                             "[p]: https://w.example/A_(b) (paren)",
                                                             "[bitişik]: https://x.example\"t\""});
}

// Parçaya yalnız andığı ve başka parçada tanımlı etiketlerin ilk tanımı eklenir (belge sırasıyla); kendi tanımı
// eklenmez, kapanmamış kuşatılmış kodla biten parçaya hiç eklenmez (tanım kodun içinde metin olurdu).
TEST_CASE("a chunk gets only the definitions it references from other chunks") {
    const std::string text = "[a]: u\n\n[b]: v\n\n# b\n\n[r][a]\n\n```\n[a]\n";
    const auto scan = noteapp::app::scan_markdown(text, 13);  // kesim "# b" ve çitin önünde
    REQUIRE(scan.chunks.size() == 3);
    CHECK(scan.chunks.back().open_fence);
    CHECK(noteapp::app::markdown_chunk_source(text, scan.chunks[0], scan.definitions) == "[a]: u\n\n[b]: v\n\n");
    CHECK(noteapp::app::markdown_chunk_source(text, scan.chunks[1], scan.definitions) ==
          "# b\n\n[r][a]\n\n\n\n[a]: u\n");
    CHECK(noteapp::app::markdown_chunk_source(text, scan.chunks[2], scan.definitions) == "```\n[a]\n");
}

// NX-E F1: belge sırasıyla ilk tanım kazanır. Tekrar (büyük/küçük harf ve boşluk farkıyla da) ilk tanımın yerini
// almaz, parçasında aynı uzunlukta boşluğa çevrilir: satır ve bayt yerleri kaymaz.
TEST_CASE("a repeated link label keeps its first definition and the repeat is blanked in its chunk") {
    const std::string text = "[Ad  X]: https://first.example\n\nmetin\n\n[ad x]: https://second.example\n\n[ad x]\n";
    const auto definitions = noteapp::app::markdown_link_definitions(text);
    CHECK(definition_lines(text) == std::vector<std::string>{"[Ad  X]: https://first.example"});
    REQUIRE(definitions.repeats.size() == 1);
    const std::size_t repeat = text.find("[ad x]: ");
    CHECK(definitions.repeats.front() == std::pair{repeat, text.find('\n', repeat)});
    const noteapp::app::MarkdownChunk whole{0, text.size(), 0, false};
    const std::string source = noteapp::app::markdown_chunk_source(text, whole, definitions);
    CHECK(source.find("second") == std::string::npos);
    CHECK(source.size() == text.size());  // ilk tanım kendi parçasında: eklenen yok
    CHECK(std::ranges::count(source, '\n') == std::ranges::count(text, '\n'));
}
