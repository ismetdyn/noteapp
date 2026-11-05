// A7: CSV/JSON önizlemesinin ortak ayrıştırması (TUI REVIEW ve GUI önizleme aynı kuralı kullanır). Görüntü
// testleri arayüzlerde: tests/tui/markdown_view_test.cpp, tests/gui/preview_test.cpp.
#include "app/data_preview.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

// NX-F 3: liste iminden sonraki ve satır başındaki boşluk sekme durağıyla (4) sütundur (CommonMark, md4c
// md_line_indentation). Sekmeli satır öğe içeriğinden az girintili sayılırsa çit erken kapanır, ardındaki "```"
// açılış sanılır ve md4c'nin kodu kapattığı yerden sonra kodun içinden kesilir.
TEST_CASE("markdown chunks measure list item fences in tab stop columns") {
    for (const std::string_view item : {"- ```\n\tx\n```\n\n", "-\t```\n\tx\n```\n\n", "1.\t```\n\tx\n```\n\n"}) {
        CAPTURE(item);
        std::string text(item);
        while (text.size() < 2 * noteapp::app::kMarkdownChunkBytes)
            text += "## Bölüm\n\nparagraf\n\n";
        text += "```\nx\n\ndef f():\n```\n\nson\n";
        const auto chunks = noteapp::app::markdown_chunks(text, 1);
        CHECK(chunks.size() > 2);
        for (const noteapp::app::MarkdownChunk& chunk : chunks)
            CHECK_FALSE(text.substr(chunk.begin).starts_with("def f():"));
    }
}

// İmden sonra 5 ya da daha çok sütun boşluk: içerik imden bir sütun sonradır, satırın kalanı girintili koddur, çit
// değil. Sonraki "```" çit açar, ardındaki başlıklar koddur.
TEST_CASE("markdown chunks see no fence after five columns of space behind a list marker") {
    for (const std::string_view item : {"-\t\t```\n```\n\n", "-     ```\n```\n\n"}) {
        CAPTURE(item);
        std::string text(item);
        while (text.size() < 2 * noteapp::app::kMarkdownChunkBytes)
            text += "## Bölüm\n\nparagraf\n\n";
        text += "```\n\nson\n";
        for (const noteapp::app::MarkdownChunk& chunk : noteapp::app::markdown_chunks(text, 1))
            CHECK_FALSE(text.substr(chunk.begin).starts_with("## Bölüm"));
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

namespace {

std::string repeated(std::string_view unit, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i)
        out += unit;
    return out;
}

// `head` ilk parçada, `tail` son parçada: aradaki dolgu notu iki parçadan fazlasına böler.
std::string two_chunk_note(std::string_view head, std::string_view tail) {
    std::string text(head);
    while (text.size() < 2 * noteapp::app::kMarkdownChunkBytes)
        text += "## Bölüm\n\nparagraf\n\n";
    text += tail;
    return text;
}

std::string first_chunk_source(std::string_view text) {
    const auto scan = noteapp::app::scan_markdown(text);
    REQUIRE(scan.chunks.size() >= 2);
    return noteapp::app::markdown_chunk_source(text, scan.chunks.front(), scan.definitions);
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

}  // namespace

// NX-F 1: md4c etiketi paragraf içinde satır sonuyla sürebilir, `\]` kaçışlı ayraç içerebilir ve 999 karaktere
// (bayt değil) dek uzayabilir. Başka parçadaki tanımı md4c gibi bulunmazsa parça bağlantıyı düz metin gösterir.
TEST_CASE("a chunk finds references whose label spans a line or escapes a bracket") {
    const std::string long_label = repeated("ç", 999);  // 1998 bayt, 999 karakter
    const std::string text =
        two_chunk_note("[çok satırlı][my\nlink] ve [kaçış][a\\]b], [uzun][" + long_label +
                           "]\r\n\n> [alıntı][q\n> label]\n\n",
                       "[my link]: https://ml.example\n\n[a\\]b]: https://esc.example\n\n[" + long_label +
                           "]: https://long.example\n\n[q label]: https://q.example\n");
    const std::string source = first_chunk_source(text);
    CHECK(contains(source, "https://ml.example"));
    CHECK(contains(source, "https://esc.example"));
    CHECK(contains(source, "https://long.example"));
    CHECK(contains(source, "https://q.example"));
}

// NX-F 2: md4c etiketi Unicode büyük/küçük harf katlamasıyla karşılaştırır: ß = ss, Ä = ä, Ş = ş, İ = i + U+0307;
// ı (U+0131) katlanmaz, I ise i olur (ılık ≠ ILIK).
TEST_CASE("link labels fold Unicode case like md4c across chunks") {
    const std::string text = two_chunk_note("[a][Straße] [b][ÄRGER] [c][ŞÖĞÜÇ] [d][İz] [e][ılık]\n\n",
                                            "[STRASSE]: https://ss.example\n\n[ärger]: https://ae.example\n\n"
                                            "[şöğüç]: https://tr.example\n\n[i\xCC\x87z]: https://idot.example\n\n"
                                            "[ILIK]: https://ilik.example\n");
    const std::string source = first_chunk_source(text);
    CHECK(contains(source, "https://ss.example"));
    CHECK(contains(source, "https://ae.example"));
    CHECK(contains(source, "https://tr.example"));
    CHECK(contains(source, "https://idot.example"));
    CHECK_FALSE(contains(source, "https://ilik.example"));
}

TEST_CASE("the first definition of a label wins over its Unicode case variants") {
    const std::string text = "[ÇAĞ]: https://1.example\n\n[çağ]: https://2.example\n\n[ILIK]: https://3.example\n\n"
                             "[ılık]: https://4.example\n\n[İ]: https://5.example\n\n[i]: https://6.example\n\n"
                             "[MASSE]: https://7.example\n\n[Maße]: https://8.example\n";
    CHECK(definition_lines(text) == std::vector<std::string>{"[ÇAĞ]: https://1.example", "[ILIK]: https://3.example",
                                                             "[ılık]: https://4.example", "[İ]: https://5.example",
                                                             "[i]: https://6.example", "[MASSE]: https://7.example"});
    CHECK(noteapp::app::markdown_link_definitions(text).repeats.size() == 2);
}

// NX-F 4: özeti çakışan k farklı etiket. Her tanımı öncekilerle metinden karşılaştırmak O(k²) idi: çakışmalı
// üretilmiş notta (64 bit FNV-1a kriptografik değildir) ilk kare donardı. Özet testte elle eşitlenir; aramada da
// grup etikete göre ikili aranır. Süre sınırı yeni yolun ~100 katı (Debug, yük, Wine payı); eski yol bu boyda
// saniyelerce sürer.
TEST_CASE("colliding label digests keep first definitions without quadratic work") {
    constexpr std::size_t kLabels = 5000;
    constexpr double kBudgetSeconds = 1.0;
    // Gerçek özet: "[x]" başvurusu çakışan grupta aranır.
    const std::uint64_t digest = noteapp::app::markdown_link_definitions("[x]: u\n").first.at(0).hash;
    std::string text = "[a][x]\n\n";
    const noteapp::app::MarkdownChunk reference{0, text.size(), 0, false};
    noteapp::app::MarkdownDefinitions definitions;
    const auto add = [&](const std::string& line) {
        definitions.first.push_back({digest, text.size(), text.size() + line.size()});
        text += line + "\n\n";
    };
    for (std::size_t i = 0; i < kLabels; ++i)
        add("[l" + std::to_string(i) + "]: https://x.example/" + std::to_string(i));
    add("[X]: https://target.example");
    add("[L7]: https://repeat.example");  // l7'nin tekrarı, büyük harfle
    add("[x]: https://repeat-x.example");
    const auto start = std::chrono::steady_clock::now();
    noteapp::app::keep_first_definitions(text, definitions);
    const std::string source = noteapp::app::markdown_chunk_source(text, reference, definitions);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("colliding digests, " << kLabels << " labels: " << seconds * 1000 << " ms");
    CHECK(definitions.first.size() == kLabels + 1);
    CHECK(definitions.repeats.size() == 2);
    CHECK(source == "[a][x]\n\n\n\n[X]: https://target.example\n");
    CHECK(seconds < kBudgetSeconds);
}

// Kaçışlı `\[` etiket açmaz (md4c): her biri 999 birimlik tarama başlatsaydı `\[` dolu 256 KiB'lık satır ~131 bin ×
// 999 adım sürerdi (Debug ~1 s). Kaçışsız `[` taramaları birbirini aşmaz: parça başına doğrusal. Süre sınırı yeni
// yolun ~50 katı.
TEST_CASE("escaped brackets start no label scan in a chunk") {
    constexpr double kBudgetSeconds = 0.25;
    const std::string text = two_chunk_note("[" + repeated("\\[", 2 * noteapp::app::kMarkdownChunkBytes) + "\n\n",
                                            "[a]: https://a.example\n");
    const auto scan = noteapp::app::scan_markdown(text);
    REQUIRE(scan.chunks.size() >= 2);
    const auto start = std::chrono::steady_clock::now();
    const std::string source = noteapp::app::markdown_chunk_source(text, scan.chunks.front(), scan.definitions);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("escaped brackets chunk: " << seconds * 1000 << " ms");
    CHECK_FALSE(contains(source, "https://a.example"));
    CHECK(seconds < kBudgetSeconds);
    // Çift ters bölü kaçışı kapatır: ardındaki `[` etiket açar.
    CHECK(contains(first_chunk_source(two_chunk_note("\\\\[a]\n\n", "[a]: https://a.example\n")), "https://a.example"));
}

// Etiket boş satırı aşamaz (paragraf biter), 999 karakteri aşamaz: md4c bu satırı tanım saymaz, eklenirse parçada
// düz metin olarak görünürdü.
TEST_CASE("link labels stop at a blank line and at 999 characters like md4c") {
    const std::string too_long = repeated("ç", 1000);
    CHECK(definition_lines("[" + too_long + "]: https://x.example\n").empty());
    CHECK(definition_lines("[" + repeated("ç", 999) + "]: https://x.example\n").size() == 1);
    CHECK(definition_lines("[\xC2\xA0]: https://x.example\n").empty());  // yalnız NBSP: boş etiket
    const std::string text = two_chunk_note("[a\n\nb]\n\n", "[a b]: https://ab.example\n");
    CHECK_FALSE(contains(first_chunk_source(text), "https://ab.example"));
}
