// A7: CSV/JSON önizlemesinin ortak ayrıştırması (TUI REVIEW ve GUI önizleme aynı kuralı kullanır). Görüntü
// testleri arayüzlerde: tests/tui/markdown_view_test.cpp, tests/gui/preview_test.cpp.
#include "app/data_preview.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
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

// NY-P inceleme: satır dizini kCsvBlockRows satırda bir ofset tutar; her pencere (blok sınırında, boş satırlar ve
// tırnak içi satır sonu araya girse de) doğru satırları verir.
TEST_CASE("csv rows from the block index match their source rows in every window") {
    std::string source = "\xEF\xBB\xBFn,metin\n";
    for (int i = 1; i < 300; ++i) {
        source += std::to_string(i) + (i % 11 == 0 ? ",\"iki\nsatır\"\n" : ",x\n");
        if (i % 7 == 0)
            source += "\n";
    }
    const auto index = noteapp::app::index_csv(source);
    REQUIRE(index);
    CHECK(index->row_count == 300);
    CHECK(index->checkpoints.size() == (300 + noteapp::app::kCsvBlockRows - 1) / noteapp::app::kCsvBlockRows);
    for (const std::size_t first : {std::size_t{0}, std::size_t{1}, std::size_t{63}, std::size_t{64},
                                    std::size_t{65}, std::size_t{127}, std::size_t{290}}) {
        CAPTURE(first);
        const auto rows = noteapp::app::csv_rows(source, *index, first, 20);
        REQUIRE(rows.size() == std::min<std::size_t>(20, 300 - first));
        for (std::size_t k = 0; k < rows.size(); ++k) {
            const std::size_t row = first + k;
            CHECK(rows[k].cells.front() == (row == 0 ? "n" : std::to_string(row)));
            CHECK(source.substr(rows[k].offset).starts_with(rows[k].cells.front()));
            if (row % 11 == 0 && row > 0)
                CHECK(rows[k].cells[1] == "iki\nsatır");
        }
    }
    CHECK(noteapp::app::csv_rows(source, *index, 300, 5).empty());
}

// NY-P güvenlik (Sev-3): kontrol noktaları bayt mesafesiyle de konur; boş satır dolgusu ya da belirteçler arası dev
// boşluk iki nokta arasında kCsvCheckpointBytes / kJsonCheckpointBytes'tan fazla taranacak yer bırakmaz.
TEST_CASE("csv and json checkpoints are byte-aware across padding") {
    constexpr std::size_t kPadding = std::size_t{1} << 20;
    std::string csv = "n,x\n1,x\n" + std::string(kPadding, '\n') + "2,x\n";
    const auto rows = noteapp::app::index_csv(csv);
    REQUIRE(rows);
    CHECK(rows->row_count == 3);
    for (std::size_t i = 1; i < rows->checkpoints.size(); ++i)
        CHECK(rows->checkpoints[i].offset - rows->checkpoints[i - 1].offset <= noteapp::app::kCsvCheckpointBytes + 4);
    CHECK(noteapp::app::csv_rows(csv, *rows, 2, 1).at(0).cells.front() == "2");

    const std::string json = "[1," + std::string(kPadding, ' ') + "2]";
    const auto lines = noteapp::app::index_json(json);
    REQUIRE(lines);
    const auto& last = lines->checkpoints.back();
    CHECK(last.pos == json.find('2'));  // nokta dolgudan sonraki belirteçte
    const auto window = noteapp::app::json_lines(json, *lines, 2, 2);
    REQUIRE(window.size() == 2);
    CHECK(window[0].text == "2");
    CHECK(window[1].text == "]");
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

// NY-P: büyük JSON dizinle parça parça üretilir; her pencere bütün yazdırmanın aynı satırlarıdır (kontrol noktası
// sınırları, boş kaplar, kaçışlı dizgedeki ayraçlar, BOM dahil).
TEST_CASE("json lines from the index match the whole pretty print in every window") {
    std::string source = "\xEF\xBB\xBF{\"liste\": [\n";
    for (int i = 0; i < 1500; ++i)
        source += "  {\"ad\": \"x\\\"]}[{" + std::to_string(i) + "\", \"boş\": {}, \"dizi\": [ ], \"n\": -1.5e3, \"d\": [[" +
                  std::to_string(i) + "], {\"t\": true}]},\n";
    source += "  null\n], \"son\": false}";
    const auto whole = noteapp::app::pretty_json(source);
    REQUIRE(whole);
    const auto index = noteapp::app::index_json(source);
    REQUIRE(index);
    CHECK(index->line_count == whole->size());
    CHECK(index->checkpoints.size() > whole->size() / noteapp::app::kJsonCheckpointLines);
    const auto same = [](const noteapp::app::JsonLine& a, const noteapp::app::JsonLine& b) {
        return a.depth == b.depth && a.text == b.text && a.offset == b.offset;
    };
    for (const std::size_t first : {std::size_t{0}, std::size_t{1}, std::size_t{255}, std::size_t{256},
                                    std::size_t{257}, std::size_t{4000}, whole->size() - 3}) {
        CAPTURE(first);
        const auto window = noteapp::app::json_lines(source, *index, first, 40);
        const std::size_t expected = std::min<std::size_t>(40, whole->size() - first);
        REQUIRE(window.size() == expected);
        for (std::size_t i = 0; i < window.size(); ++i) {
            CAPTURE(i);
            CHECK(same(window[i], (*whole)[first + i]));
        }
    }
    CHECK(noteapp::app::json_lines(source, *index, whole->size(), 10).empty());
    CHECK_FALSE(noteapp::app::index_json("[1,]"));
    CHECK_FALSE(noteapp::app::index_json(std::string(100000, '[')));
    const auto scalar = noteapp::app::index_json(" 7 ");
    REQUIRE(scalar);
    CHECK(scalar->line_count == 1);
    CHECK(noteapp::app::json_lines(" 7 ", *scalar, 0, 5).front().text == "7");
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

namespace {

// Parçalar notu boşluksuz örter ve hiçbiri sınırdan büyük değildir.
void check_bounded_cover(std::string_view text, const std::vector<noteapp::app::MarkdownChunk>& chunks) {
    REQUIRE_FALSE(chunks.empty());
    CHECK(chunks.front().begin == 0);
    CHECK(chunks.back().end == text.size());
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        CAPTURE(i);
        CHECK(chunks[i].end - chunks[i].begin <= noteapp::app::kMarkdownMaxChunkBytes);
        if (i > 0)
            CHECK(chunks[i].begin == chunks[i - 1].end);
    }
}

}  // namespace

// NY-P: kesim yeri olmayan dev blok (tek satır, uzun paragraf, kod, tablo, sıkı liste) da sınırlı parçalara bölünür;
// kod ve tablo parçası önüne taşınan çit ya da başlıkla yine kod ve tablo olarak biçimlenir.
TEST_CASE("markdown chunks split one huge block at line starts, or inside a huge line, carrying fences and headers") {
    using noteapp::app::kMarkdownMaxChunkBytes;
    const std::string line(5 * kMarkdownMaxChunkBytes, 'x');
    const auto single = noteapp::app::markdown_chunks(line);
    check_bounded_cover(line, single);
    CHECK(single.size() >= 5);
    CHECK(single.back().first_line == 0);

    std::string words;  // tek satır, sözcükler: kesim boşluktan sonra
    while (words.size() < 3 * kMarkdownMaxChunkBytes)
        words += "kelime ğüşıöç ";
    const auto spaced = noteapp::app::markdown_chunks(words);
    check_bounded_cover(words, spaced);
    for (std::size_t i = 1; i < spaced.size(); ++i)
        CHECK(words[spaced[i].begin - 1] == ' ');

    std::string multibyte;  // boşluksuz çok baytlı: kesim kod noktası sınırında
    while (multibyte.size() < 3 * kMarkdownMaxChunkBytes)
        multibyte += "ğ";
    const auto glyphs = noteapp::app::markdown_chunks(multibyte);
    check_bounded_cover(multibyte, glyphs);
    for (const auto& chunk : glyphs)
        CHECK((static_cast<unsigned char>(multibyte[chunk.begin]) & 0xC0) != 0x80);

    std::string code = "# Başlık\n\n```cpp\n";
    while (code.size() < 3 * kMarkdownMaxChunkBytes)
        code += "int satir = 1;\n";
    code += "```\n";
    const auto fenced = noteapp::app::markdown_chunks(code);
    check_bounded_cover(code, fenced);
    REQUIRE(fenced.size() >= 3);
    for (std::size_t i = 1; i < fenced.size(); ++i) {
        CAPTURE(i);
        CHECK(code[fenced[i].begin - 1] == '\n');  // satır başında
        CHECK(code.substr(fenced[i].carry_begin, fenced[i].carry_end - fenced[i].carry_begin) == "```cpp");
        CHECK(fenced[i].carry_lines == 1);
        CHECK(fenced[i - 1].open_fence);
        CHECK(noteapp::app::markdown_chunk_source(code, fenced[i], {}).starts_with("```cpp\nint satir"));
    }

    std::string table = "| ad | değer |\n|---|--:|\n";
    while (table.size() < 3 * kMarkdownMaxChunkBytes)
        table += "| a | 1 |\n";
    const auto rows = noteapp::app::markdown_chunks(table);
    check_bounded_cover(table, rows);
    REQUIRE(rows.size() >= 3);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        CHECK(table.substr(rows[i].carry_begin, rows[i].carry_end - rows[i].carry_begin) ==
              "| ad | değer |\n|---|--:|");
        CHECK(rows[i].carry_lines == 2);
        CHECK_FALSE(rows[i - 1].open_fence);
    }

    std::string list;
    while (list.size() < 3 * kMarkdownMaxChunkBytes)
        list += "- madde\n";
    const auto items = noteapp::app::markdown_chunks(list);
    check_bounded_cover(list, items);
    for (std::size_t i = 1; i < items.size(); ++i) {
        CHECK(list.substr(items[i].begin).starts_with("- madde"));
        CHECK(items[i].carry_end == items[i].carry_begin);
    }
}

// NY-P inceleme (Sev-3): taşınan satır sınırlı önektir. Dev bilgi dizgeli çitten yalnız im ve dil sözcüğü, dev
// başlıklı tablodan hiçbir şey taşınmaz (her parçaya kopyalanmaz). Kod içindeki tablo görünümlü satır sonraki dev
// paragrafa sahte tablo başlığı taşıtmaz.
TEST_CASE("markdown chunks carry only a bounded prefix and no header from a fenced table look-alike") {
    using noteapp::app::kMarkdownMaxChunkBytes;
    std::string code = "```cpp " + std::string(3 * kMarkdownMaxChunkBytes, 'i') + "\n";
    while (code.size() < 6 * kMarkdownMaxChunkBytes)
        code += "int satir = 1;\n";
    code += "```\n";
    const auto fenced = noteapp::app::markdown_chunks(code);
    REQUIRE(fenced.size() >= 3);
    for (std::size_t i = 1; i < fenced.size(); ++i) {
        const std::string_view carry =
            std::string_view(code).substr(fenced[i].carry_begin, fenced[i].carry_end - fenced[i].carry_begin);
        CAPTURE(i);
        CHECK((carry.empty() || carry == "```cpp"));
        CHECK(fenced[i].carry_lines <= 1);
    }

    std::string wide = "| " + std::string(3 * kMarkdownMaxChunkBytes, 'b') + " |\n|---|\n";
    while (wide.size() < 6 * kMarkdownMaxChunkBytes)
        wide += "| a |\n";
    for (const auto& chunk : noteapp::app::markdown_chunks(wide))
        CHECK(chunk.carry_end == chunk.carry_begin);

    std::string look_alike = "```\n\n| a |\n|---|\n```\n";  // koddaki boş satırdan sonra tablo görünümü
    while (look_alike.size() < 3 * kMarkdownMaxChunkBytes)
        look_alike += "uzun paragraf satırı devam ediyor\n";
    const auto paragraph = noteapp::app::markdown_chunks(look_alike);
    REQUIRE(paragraph.size() >= 2);
    for (const auto& chunk : paragraph)
        CHECK(chunk.carry_end == chunk.carry_begin);
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
        // Koddaki başlıktan yalnız zorunlu kesim (kMarkdownMaxChunkBytes) kesebilir; o parça açılış çitini taşır.
        for (const noteapp::app::MarkdownChunk& chunk : noteapp::app::markdown_chunks(text, 1))
            if (text.substr(chunk.begin).starts_with("## Bölüm"))
                CHECK(text.substr(chunk.carry_begin, chunk.carry_end - chunk.carry_begin) == "```");
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

// NY-P güvenlik (Sev-2): parçanın andığı başka parçadaki tanımlar bir iş bütçesiyle eklenir. Her parça bütün tanım
// tablosunu ansa da parçaya eklenen bayt bütçeyi aşmaz: toplam iş parça sayısı × tanım baytı değil, not boyuyla
// doğrusaldır. Bütçeyi aşan başvuru çözülmez (metin olarak görünür).
TEST_CASE("a chunk appends referenced definitions within a work budget") {
    std::string references;
    std::string definitions;
    for (int i = 0; i < 4000; ++i) {
        const std::string label = "e" + std::to_string(i);
        references += "[" + label + "] ";
        definitions += "[" + label + "]: https://example.org/" + std::string(60, 'p') + "/" + label + "\n";
    }
    const std::string text = references + "\n\n# tanımlar\n\n" + definitions;
    const auto scan = noteapp::app::scan_markdown(text, 1);
    REQUIRE(scan.chunks.size() >= 2);
    const noteapp::app::MarkdownChunk& first = scan.chunks.front();
    const std::string source = noteapp::app::markdown_chunk_source(text, first, scan.definitions);
    const std::size_t budget = noteapp::app::MarkdownChunkLimits{}.definition_bytes;
    REQUIRE(definitions.size() > budget);
    CHECK(source.size() <= (first.end - first.begin) + budget + 2);
    CHECK(source.find("[e0]: https://example.org/") != std::string::npos);  // bütçe içindekiler çözülür
    // Bütçe config'tendir; 0 (sınırsız, SIZE_MAX) her anılan tanımı ekler.
    const noteapp::app::MarkdownChunkLimits unlimited{.definition_bytes = std::numeric_limits<std::size_t>::max()};
    const std::string all = noteapp::app::markdown_chunk_source(text, first, scan.definitions, unlimited);
    CHECK(all.find("[e3999]: https://example.org/") != std::string::npos);
}

// Korumaların üye varsayılanları defaults.toml ile aynı; değerler limits.tui.preview_*'tan gelir.
TEST_CASE("markdown chunk limits come from limits.tui.preview_* and match defaults.toml") {
    noteapp::core::TuiLimits limits;
    limits.preview_definition_bytes = {128U * 1024U, "128KiB"};
    limits.preview_fence_info_bytes = {64, "64"};
    limits.preview_table_header_bytes = {4U * 1024U, "4KiB"};
    CHECK(noteapp::app::markdown_chunk_limits_of(limits) == noteapp::app::MarkdownChunkLimits{});
    limits.preview_fence_info_bytes = {0, "0"};
    CHECK(noteapp::app::markdown_chunk_limits_of(limits).fence_info_bytes == std::numeric_limits<std::size_t>::max());

    using noteapp::app::kMarkdownMaxChunkBytes;
    std::string code = "```" + std::string(100, 'l') + "\n";  // 64 baytı aşan dil sözcüğü
    while (code.size() < 3 * kMarkdownMaxChunkBytes)
        code += "satir\n";
    const auto carried = [&](const noteapp::app::MarkdownChunkLimits& chunk_limits) {
        const auto chunks = noteapp::app::scan_markdown(code, noteapp::app::kMarkdownChunkBytes, kMarkdownMaxChunkBytes,
                                                        chunk_limits)
                                .chunks;
        REQUIRE(chunks.size() >= 2);
        return chunks[1].carry_end - chunks[1].carry_begin;
    };
    CHECK(carried({}) == 3);  // yalnız çit
    CHECK(carried({.fence_info_bytes = 128}) == 103);
}

// NY-P güvenlik (Sev-3): taşınan çit önekinin TAMAMI (girinti, im, çit koşusu, boşluk, dil sözcüğü) sınırdadır. Dev
// çit koşusu ya da sözcükten önceki dev boşluk her parçaya kopyalanmaz; sınırı aşan çit hiç taşınmaz (kısaltılmış çit
// kodu erken kapatırdı).
TEST_CASE("the carried fence prefix is bounded as a whole, never shortened") {
    using noteapp::app::kMarkdownMaxChunkBytes;
    const std::size_t limit = noteapp::app::MarkdownChunkLimits{}.fence_info_bytes;
    for (const std::string& opener : {std::string(3 * kMarkdownMaxChunkBytes, '~'),
                                      "``` " + std::string(3 * kMarkdownMaxChunkBytes, ' ') + "cpp"}) {
        std::string code = opener + "\n";
        while (code.size() < 2 * opener.size())
            code += "satir\n";
        const auto chunks = noteapp::app::markdown_chunks(code);
        REQUIRE(chunks.size() >= 3);
        for (const auto& chunk : chunks) {
            CHECK(chunk.carry_end - chunk.carry_begin <= limit);
            CHECK(chunk.carry_lines == (chunk.carry_end > chunk.carry_begin ? 1U : 0U));
        }
    }
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

// NY-P güvenlik (Sev-3): özeti çakışan grupta başvuru başına karşılaştırma tanım satırının yalnız etiketini okur
// (satır toplanırken doğrulandı). Satırın tamamını yeniden doğrulamak 100 KB başlıklı iki tanım ve 2.000 başvuruda
// ~400 MB tarama ederdi (Debug saniyeler).
TEST_CASE("a colliding digest compares only the label of long definition lines") {
    constexpr int kReferences = 2000;
    constexpr double kBudgetSeconds = 0.5;
    const std::uint64_t digest = noteapp::app::markdown_link_definitions("[x]: u\n").first.at(0).hash;
    std::string text;
    for (int i = 0; i < kReferences; ++i)
        text += "[x] ";
    text += "\n\n";
    const noteapp::app::MarkdownChunk reference{0, text.size(), 0, false};
    noteapp::app::MarkdownDefinitions definitions;
    const std::string title(100 * 1024, 't');
    for (const char* label : {"w", "x"}) {
        const std::string line = std::string("[") + label + "]: https://x.example \"" + title + "\"";
        definitions.first.push_back({digest, text.size(), text.size() + line.size()});
        text += line + "\n\n";
    }
    noteapp::app::keep_first_definitions(text, definitions);
    const auto start = std::chrono::steady_clock::now();
    const std::string source = noteapp::app::markdown_chunk_source(text, reference, definitions);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("colliding digest, 2 long definitions, " << kReferences << " references: " << seconds * 1000 << " ms");
    CHECK(source.find("[x]: https://x.example") != std::string::npos);
    CHECK(source.find("[w]: https://x.example") == std::string::npos);
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
