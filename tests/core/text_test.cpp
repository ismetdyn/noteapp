#include "core/text.hpp"

#include <chrono>
#include <ctime>
#include <doctest/doctest.h>

using namespace noteapp::core;

namespace {

// Yerel saatte sabit bir an; slugify zaman damgasını yerel saatten üretir.
std::chrono::system_clock::time_point local_time(int y, int mo, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

const auto kNow = local_time(2026, 10, 28, 14, 32, 5);
constexpr const char* kStamp = "note-20261028-143205";

}  // namespace

TEST_CASE("utf8 round-trips ASCII, 2-, 3- and 4-byte sequences") {
    const std::string text = "aç€😀";
    auto decoded = decode_utf8(text);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == std::u32string{U'a', U'ç', U'€', U'\U0001F600'});
    CHECK(encode_utf8(*decoded) == text);
    CHECK(is_valid_utf8(text));
    CHECK(is_valid_utf8(""));
}

TEST_CASE("utf8 rejects malformed input") {
    CHECK_FALSE(is_valid_utf8("\xC3"));              // kesik dizi
    CHECK_FALSE(is_valid_utf8("\xFF"));              // geçersiz başlangıç baytı
    CHECK_FALSE(is_valid_utf8("\x80"));              // yalnız devam baytı
    CHECK_FALSE(is_valid_utf8("\xC0\xAF"));          // fazla uzun kodlama
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));      // vekil çift U+D800
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // > U+10FFFF
    CHECK_FALSE(decode_utf8("a\xC3").has_value());
}

TEST_CASE("transliterate_turkish maps Turkish letters") {
    CHECK(transliterate_turkish("çğıöşü") == "cgiosu");
    CHECK(transliterate_turkish("ÇĞİÖŞÜ") == "CGiOSU");
    CHECK(transliterate_turkish("abc é 1") == "abc é 1");
}

TEST_CASE("slugify basics") {
    CHECK(slugify("# Toplantı Notları: Q4!", kNow) == "toplanti-notlari-q4");
    CHECK(slugify("İSTANBUL ŞUBESİ", kNow) == "istanbul-subesi");
    CHECK(slugify("- * > ## \t Merhaba   Dünya", kNow) == "merhaba-dunya");
    CHECK(slugify("--a--b--", kNow) == "a-b");
}

TEST_CASE("slugify falls back to a timestamp name") {
    CHECK(slugify("", kNow) == kStamp);
    CHECK(slugify("   ", kNow) == kStamp);
    CHECK(slugify("# !!! ???", kNow) == kStamp);
    CHECK(slugify("😀", kNow) == kStamp);
}

TEST_CASE("slugify respects max length at a dash boundary") {
    // 7 kelime x 10 harf: 6. kelime 60. karakteri aşar, son tire 54. konumda.
    const std::string line = "aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee ffffffffff gggggggggg";
    CHECK(slugify(line, kNow) == "aaaaaaaaaa-bbbbbbbbbb-cccccccccc-dddddddddd-eeeeeeeeee");
    CHECK(slugify(std::string(100, 'x'), kNow) == std::string(60, 'x'));
    CHECK(slugify("abc defgh", kNow, 5) == "abc");
    CHECK(slugify("abc-defgh", kNow, 4) == "abc");  // 4. karakter tam tire: temiz kesim
}

TEST_CASE("slugify does not crash on invalid UTF-8") {
    CHECK(slugify("\xFF\xFE", kNow) == kStamp);
    CHECK(slugify("ab\xC3 cd", kNow) == "ab-cd");
}

// Not adının kaynağı: CLI, TUI ve GUI aynı kuralı kullanır (önceden üç kopyaydı).
TEST_CASE("first_non_empty_line skips blank and whitespace-only lines") {
    CHECK(first_non_empty_line("") == "");
    CHECK(first_non_empty_line("\n \t\r\n") == "");
    CHECK(first_non_empty_line("\n\n  # Başlık\nikinci") == "  # Başlık");
    CHECK(first_non_empty_line("satır\r\nsonraki") == "satır\r");
    CHECK(first_non_empty_line("tek") == "tek");
}

// create() ve GUI editörü aynı kuralı kullanır: yalnız CRLF'nin CR'si düşer, tek başına CR kalır.
TEST_CASE("normalize_line_endings turns CRLF into LF and keeps a lone CR") {
    CHECK(normalize_line_endings("") == "");
    CHECK(normalize_line_endings("a\r\nb\r\n") == "a\nb\n");
    CHECK(normalize_line_endings("a\rb\n\r") == "a\rb\n\r");
    CHECK(normalize_line_endings("\r\r\n") == "\r\n");
    CHECK(normalize_line_endings("çğ\r\nış") == "çğ\nış");
    const std::string big(1 << 20, 'x');  // doğrusal: büyük girdide de bir geçiş
    CHECK(normalize_line_endings(big + "\r\n") == big + "\n");
}

TEST_CASE("note_name_source: an assignment on the first line names the note by its key, whatever the type") {
    for (const std::string_view type : {"md", "txt", "sh"}) {
        CAPTURE(type);
        CHECK(note_name_source("KEY=value", type) == "KEY");
        CHECK(note_name_source("AWS_SECRET=sk-live-123\n", type) == "AWS_SECRET");
        CHECK(note_name_source("export TOKEN=x", type) == "TOKEN");
        CHECK(note_name_source("API_KEY: abc", type) == "API_KEY");
        CHECK(note_name_source("API_KEY: \"a b c\"", type) == "API_KEY");
        CHECK(note_name_source("TOKEN = abc def", type) == "TOKEN");
        CHECK(note_name_source("server.port=8080\r\n", type) == "server.port");
    }
}

TEST_CASE("note_name_source: ordinary prose and code keep the whole line") {
    for (const std::string_view line :
         {"Toplantı: bütçe", "Not: süt al ve ekmek", "# Toplantı Notları: Q4!", "int x = 5;", "merhaba dünya",
          "public class Greeter {", "12:30 toplantı", "std::cout << x", "a == b", "https://example.org/yol",
          "KEY=", "KEY:", "=deger", "iki sözcük=deger"}) {
        CAPTURE(line);
        CHECK(note_name_source(line, "md") == line);
    }
    CHECK(slugify(note_name_source("# Toplantı Notları: Q4!", "md"), kNow) == "toplanti-notlari-q4");
    // Kullanıcı adı ve parola taşıyan URL düzyazı sayılmaz: parola ada girmesin.
    CHECK(note_name_source("https://kullanici:parola@example.org", "md") == "https");
}

TEST_CASE("note_name_source: secret-bearing types skip comments and use the first key") {
    for (const std::string_view type : {"env", "properties", "ini", "toml", "yaml"}) {
        CAPTURE(type);
        CHECK(note_name_source("# yorum\n\n; yorum\n! yorum\nAPI_TOKEN=gizli\nB=1\n", type) == "API_TOKEN");
        CHECK(note_name_source("[bolum]\nkullanici = ali\n", type) == "kullanici");
        CHECK(note_name_source("---\n- liste\nanahtar: deger\n", type) == "anahtar");
        CHECK(note_name_source("export TOKEN=x\n", type) == "TOKEN");
    }
}

TEST_CASE("note_name_source: a secret-bearing type with no key gets a timestamp name") {
    for (const std::string_view content : {"", "# yalniz yorum\n", "bir sir degeri\n", "- liste\n"}) {
        CAPTURE(content);
        CHECK(note_name_source(content, "env").empty());
        CHECK(slugify(note_name_source(content, "env"), kNow) == kStamp);
    }
}

TEST_CASE("split_typed_name: the last dot of a name with no blanks after it") {
    const struct {
        std::string_view name, stem, extension;
    } cases[] = {
        {"deneme.cpp", "deneme", "cpp"},
        {"a.tar.gz", "a.tar", "gz"},
        {"notlar.v2", "notlar", "v2"},
        {"ad", "ad", ""},
        {".gizli", ".gizli", ""},
        {"ad.", "ad.", ""},
        {"v1.2 son", "v1.2 son", ""},
        {"Rapor.PDF", "Rapor", "PDF"},
        {"", "", ""},
    };
    for (const auto& item : cases) {
        CAPTURE(item.name);
        const TypedName typed = split_typed_name(item.name);
        CHECK(typed.stem == item.stem);
        CHECK(typed.extension == item.extension);
    }
}

TEST_CASE("slugify_stem keeps dots between the slugged parts") {
    const auto now = std::chrono::system_clock::now();
    CHECK(slugify_stem("a.tar", now) == "a.tar");
    CHECK(slugify_stem("Sürüm 1.2 Son", now) == "surum-1.2-son");
    CHECK(slugify_stem("Toplantı Notu", now) == slugify("Toplantı Notu", now));
    CHECK(slugify_stem("a..b", now) == "a.b");
    CHECK(slugify_stem("...", now) == slugify("", now));
}
