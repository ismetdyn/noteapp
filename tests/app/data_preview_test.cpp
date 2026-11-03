// A7: CSV/JSON önizlemesinin ortak ayrıştırması (TUI REVIEW ve GUI önizleme aynı kuralı kullanır). Görüntü
// testleri arayüzlerde: tests/tui/markdown_view_test.cpp, tests/gui/preview_test.cpp.
#include "app/data_preview.hpp"

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
