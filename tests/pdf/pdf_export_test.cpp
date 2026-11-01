// T38: PDF. Türkçe geri okuma ve yerleşim, gerçek pdftotext çıktısından doğrulanır.
#include "core/i18n.hpp"
#include "pdf/pdf_export.hpp"
#include "support/run_process.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <vector>

using namespace noteapp;
using noteapp::test::TempDir;
namespace fs = std::filesystem;

namespace {

constexpr char kTurkish[] = "çğıİöşü ÇĞIÖŞÜ";
constexpr double kPageWidth = 595.28;
constexpr double kPageHeight = 841.89;
constexpr double kMargin = 42.52;  // 15 mm
constexpr double kTolerance = 0.6;
constexpr std::size_t kPathologicalQuoteDepth = 1'000'000;
constexpr int kLazyLineCount = 20000;
constexpr std::chrono::seconds kPathologicalQuoteBound{8};

std::string slurp_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string export_bytes(std::string_view text, pdf::NoteFormat format) {
    const TempDir dir;
    const fs::path out = dir.path() / "çıktı.pdf";
    const auto result = pdf::export_pdf(text, format, out);
    if (!result)
        FAIL(core::format_error(result.error()));
    return slurp_bytes(out);
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

void append_big_endian(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out += static_cast<char>((value >> shift) & 0xFFU);
}

std::uint32_t crc32_of(std::string_view bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char byte : bytes) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
    }
    return ~crc;
}

void append_png_chunk(std::string& png, std::string_view type, std::string_view data) {
    append_big_endian(png, static_cast<std::uint32_t>(data.size()));
    const std::string body = std::string(type).append(data);
    png += body;
    append_big_endian(png, crc32_of(body));
}

// Geçerli, sıkıştırılmamış, gürültülü PNG: dışa aktarım gömseydi dosya en az bu kadar büyürdü. Elle kurulur;
// testler görüntü kütüphanesine bağlı değil.
std::string noise_png() {
    constexpr std::uint32_t kSide = 300;
    constexpr std::size_t kMaxStoredBlock = 65535;
    std::string pixels;
    std::uint32_t noise = 12345;
    for (std::uint32_t y = 0; y < kSide; ++y) {
        pixels += '\0';  // satır filtresi: yok
        for (std::uint32_t x = 0; x < kSide * 3; ++x) {
            noise = noise * 1664525U + 1013904223U;
            pixels += static_cast<char>(noise >> 24);
        }
    }
    std::string deflated = "\x78\x01";  // zlib başlığı
    for (std::size_t at = 0; at < pixels.size(); at += kMaxStoredBlock) {
        const auto length = static_cast<std::uint32_t>(std::min(kMaxStoredBlock, pixels.size() - at));
        deflated += at + length == pixels.size() ? '\1' : '\0';  // saklı blok; son mu
        for (const std::uint32_t half : {length, ~length & 0xFFFFU}) {
            deflated += static_cast<char>(half & 0xFFU);
            deflated += static_cast<char>(half >> 8);
        }
        deflated.append(pixels, at, length);
    }
    std::uint32_t adler_low = 1;
    std::uint32_t adler_high = 0;
    for (const char byte : pixels) {
        adler_low = (adler_low + static_cast<unsigned char>(byte)) % 65521U;
        adler_high = (adler_high + adler_low) % 65521U;
    }
    append_big_endian(deflated, (adler_high << 16) | adler_low);

    std::string header;
    append_big_endian(header, kSide);
    append_big_endian(header, kSide);
    header.append({'\x08', '\x02', '\0', '\0', '\0'});  // 8 bit RGB
    std::string png = "\x89PNG\r\n\x1A\n";
    append_png_chunk(png, "IHDR", header);
    append_png_chunk(png, "IDAT", deflated);
    append_png_chunk(png, "IEND", {});
    return png;
}

// Argümansız `pdftotext` satırlara böler; sayfa/konum için -bbox HTML'i kullanılır.
struct Extracted {
    std::string text;
    std::string bbox;
};

Extracted extract(std::string_view input, pdf::NoteFormat format) {
    const TempDir dir;
    const fs::path out = dir.path() / "a.pdf";
    const auto result = pdf::export_pdf(input, format, out);
    if (!result)
        FAIL(core::format_error(result.error()));
    const auto text = test::run_process(NOTEAPP_PDFTOTEXT, {test::utf8(out), "-"});
    REQUIRE_MESSAGE(text.exit_code == 0, text.err);
    const auto bbox = test::run_process(NOTEAPP_PDFTOTEXT, {"-bbox", test::utf8(out), "-"});
    REQUIRE_MESSAGE(bbox.exit_code == 0, bbox.err);
    return {text.out, bbox.out};
}

std::string without_whitespace(std::string text) {
    std::erase_if(text, [](char c) { return c == ' ' || c == '\n' || c == '\f' || c == '\r'; });
    return text;
}

double attribute(const std::string& tag, const std::string& name) {
    const auto start = tag.find(name + "=\"");
    REQUIRE(start != std::string::npos);
    const char* first = tag.data() + start + name.size() + 2;
    double value = 0;
    std::from_chars(first, tag.data() + tag.size(), value);
    return value;
}

std::size_t page_count(const std::string& bbox) {
    std::size_t pages = 0;
    for (auto at = bbox.find("<page "); at != std::string::npos; at = bbox.find("<page ", at + 1))
        ++pages;
    return pages;
}

void check_inside_margins(const std::string& bbox) {
    std::size_t words = 0;
    for (auto at = bbox.find("<word "); at != std::string::npos; at = bbox.find("<word ", at + 1)) {
        const std::string tag = bbox.substr(at, bbox.find('>', at) - at);
        ++words;
        CHECK(attribute(tag, "xMin") >= kMargin - kTolerance);
        CHECK(attribute(tag, "xMax") <= kPageWidth - kMargin + kTolerance);
        CHECK(attribute(tag, "yMin") >= kMargin - kTolerance);
        CHECK(attribute(tag, "yMax") <= kPageHeight - kMargin + kTolerance);
    }
    CHECK(words > 0);
}

}  // namespace

TEST_CASE("format_for treats only the md extension as markdown") {
    CHECK(pdf::format_for("a/b.md") == pdf::NoteFormat::Markdown);
    CHECK(pdf::format_for("B.MD") == pdf::NoteFormat::Markdown);
    CHECK(pdf::format_for("a.txt") == pdf::NoteFormat::PlainText);
    CHECK(pdf::format_for("md") == pdf::NoteFormat::PlainText);
}

TEST_CASE("export_pdf writes a PDF for markdown and plain text") {
    CHECK(export_bytes("# Başlık\n\nmetin", pdf::NoteFormat::Markdown).rfind("%PDF-", 0) == 0);
    CHECK(export_bytes("düz metin", pdf::NoteFormat::PlainText).rfind("%PDF-", 0) == 0);
}

TEST_CASE("export_pdf of an empty note still writes a one page document") {
    const std::string pdf_bytes = export_bytes("", pdf::NoteFormat::Markdown);
    CHECK(pdf_bytes.rfind("%PDF-", 0) == 0);
    CHECK(contains(pdf_bytes, "/Type /Page"));
}

TEST_CASE("unwritable output reports an error and leaves no file behind") {
    const TempDir dir;
    const fs::path out = dir.path() / "yok-dizin" / "a.pdf";
    const auto result = pdf::export_pdf("metin", pdf::NoteFormat::PlainText, out);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == core::Error::Code::Io);
    CHECK_FALSE(fs::exists(out));
    CHECK(fs::is_empty(dir.path()));
}

TEST_CASE("a successful export replaces an existing output and leaves no temporary file") {
    const TempDir dir;
    const fs::path out = dir.path() / "a.pdf";
    {
        std::ofstream(out, std::ios::binary) << "eski";
    }
    REQUIRE(pdf::export_pdf("yeni", pdf::NoteFormat::PlainText, out).has_value());
    CHECK(slurp_bytes(out).rfind("%PDF-", 0) == 0);
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir.path()))
        ++entries;
    CHECK(entries == 1);  // geçici dosya kalmaz
}

TEST_CASE("image references are never embedded and their targets never reach the PDF") {
    const TempDir dir;
    const fs::path png = dir.path() / "secret.png";
    const std::string png_bytes = noise_png();
    {
        std::ofstream(png, std::ios::binary) << png_bytes;
    }
    const std::string pixels_sample = png_bytes.substr(png_bytes.size() / 2, 1000);
    const std::string local = "![alt](file://" + test::utf8(png) + ")\n";
    const std::string remote = "![alt](file://attacker.example/s/a.png)\n";
    const std::string raw_html = "<img src=\"file://" + test::utf8(png) + "\">\n";

    const std::string from_local = export_bytes(local, pdf::NoteFormat::Markdown);
    const std::string from_remote = export_bytes(remote, pdf::NoteFormat::Markdown);
    const std::string from_raw_html = export_bytes(raw_html, pdf::NoteFormat::Markdown);

    // Hedef çıktıyı hiç etkilemez: gömme ya da yol sızıntısı olsa ikisi farklı olurdu.
    CHECK(from_local.size() == from_remote.size());
    CHECK(from_local == from_remote);
    CHECK_FALSE(contains(from_remote, "/Subtype /Image"));
    CHECK_FALSE(contains(from_remote, "attacker.example"));
    CHECK_FALSE(contains(from_local, pixels_sample));
    CHECK_FALSE(contains(from_raw_html, "/Subtype /Image"));
    CHECK_FALSE(contains(from_raw_html, pixels_sample));
}

TEST_CASE("links produce no annotation that a viewer could follow") {
    const std::string pdf_bytes =
        export_bytes("[metin](https://attacker.example/x)", pdf::NoteFormat::Markdown);
    CHECK_FALSE(contains(pdf_bytes, "attacker.example"));
    CHECK_FALSE(contains(pdf_bytes, "/Annot"));
}

TEST_CASE("broken and unsupported text never fails the export") {
    for (const std::string& text : {std::string("geçersiz \xC3\x28 \xFF utf8"),
                                    std::string("emoji \xF0\x9F\x98\x80 son"), std::string("中文 العربية"),
                                    std::string("kontrol \x01\x07 son"), std::string("satır\r\nsonu\ttab")}) {
        CHECK(export_bytes(text, pdf::NoteFormat::PlainText).rfind("%PDF-", 0) == 0);
        CHECK(export_bytes(text, pdf::NoteFormat::Markdown).rfind("%PDF-", 0) == 0);
    }
}

// Girinti tavana takılınca bar çizilmez; döngü yine de derinlik kadar dönerse satır başına milyon adım
// demektir.
TEST_CASE("a pathologically deep quote with a long lazy paragraph exports within the time bound") {
    std::string text(kPathologicalQuoteDepth, '>');
    text += " ilk satır\n";
    for (int i = 0; i < kLazyLineCount; ++i)
        text += "tembel satır\n";
    const auto started = std::chrono::steady_clock::now();
    const std::string pdf_bytes = export_bytes(text, pdf::NoteFormat::Markdown);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    MESSAGE(
        "deep quote export ms: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    CHECK(pdf_bytes.rfind("%PDF-", 0) == 0);
    CHECK(elapsed < kPathologicalQuoteBound);
}

TEST_CASE("Turkish markdown reads back through pdftotext") {
    const auto got = extract(std::string("# Toplantı notları\n\n- ") + kTurkish +
                                 "\n\n**kalın çğış** *eğik ĞÜ* `kod ıİ`\n",
                             pdf::NoteFormat::Markdown);
    CHECK(contains(got.text, kTurkish));
    CHECK(contains(got.text, "Toplantı notları"));
    CHECK(contains(got.text, "kalın çğış"));
    CHECK(contains(got.text, "eğik ĞÜ"));
    CHECK(contains(got.text, "kod ıİ"));
}

TEST_CASE("a leading BOM does not hide a markdown heading or reach the text") {
    const auto got = extract("\xEF\xBB\xBF# Başlık\n", pdf::NoteFormat::Markdown);
    CHECK(contains(got.text, "Başlık"));
    CHECK_FALSE(contains(got.text, "#"));
    CHECK_FALSE(contains(got.text, "\xEF\xBB\xBF"));
}

TEST_CASE("Turkish plain text reads back through pdftotext") {
    const auto got = extract(std::string("bütçe onayı\n") + kTurkish, pdf::NoteFormat::PlainText);
    CHECK(contains(got.text, "bütçe onayı"));
    CHECK(contains(got.text, kTurkish));
}

TEST_CASE("the page is A4 and text stays inside 15 mm margins") {
    const auto got = extract("# Başlık\n\nkısa metin", pdf::NoteFormat::Markdown);
    const std::string page_tag = got.bbox.substr(got.bbox.find("<page "), 60);
    CHECK(attribute(page_tag, "width") == doctest::Approx(kPageWidth).epsilon(0.002));
    CHECK(attribute(page_tag, "height") == doctest::Approx(kPageHeight).epsilon(0.002));
    check_inside_margins(got.bbox);
}

TEST_CASE("a long line wraps inside the margins and loses no text") {
    std::string words;
    for (int i = 0; i < 400; ++i)
        words += "kelime" + std::to_string(i) + " ";
    for (const auto format : {pdf::NoteFormat::Markdown, pdf::NoteFormat::PlainText}) {
        const auto got = extract(words, format);
        check_inside_margins(got.bbox);
        CHECK(contains(got.text, "kelime0"));
        CHECK(contains(got.text, "kelime399"));
    }
}

TEST_CASE("a single word wider than the page is broken and stays whole") {
    std::string word;
    for (int i = 0; i < 3000; ++i)
        word += i % 2 == 0 ? "ş" : "x";
    for (const auto format : {pdf::NoteFormat::Markdown, pdf::NoteFormat::PlainText}) {
        const auto got = extract(word, format);
        check_inside_margins(got.bbox);
        CHECK(without_whitespace(got.text) == word);
    }
}

TEST_CASE("overflowing text continues on new pages without losing lines") {
    std::string lines;
    for (int i = 0; i < 300; ++i)
        lines += "satır-" + std::to_string(i) + "\n\n";
    for (const auto format : {pdf::NoteFormat::Markdown, pdf::NoteFormat::PlainText}) {
        const auto got = extract(lines, format);
        CHECK(page_count(got.bbox) >= 2);
        check_inside_margins(got.bbox);
        for (int i = 0; i < 300; ++i)
            CHECK(contains(got.text, "satır-" + std::to_string(i)));
    }
}

TEST_CASE("markdown coverage: heading, list, code, quote, link text and GFM table") {
    const auto got = extract(
        "# Başlık\n\n## Alt başlık\n\n- madde bir\n  - iç madde\n\n1. sıralı\n2. ikinci\n\n"
        "```\nkod bloğu  iki boşluk\n```\n\n> alıntı satırı\n\n[bağlantı metni](https://x.example/y)\n\n"
        "| Ad | Değer |\n|---|---|\n| şehir | İzmir |\n\n---\n\nson\n",
        pdf::NoteFormat::Markdown);
    for (const char* expected :
         {"Başlık", "Alt başlık", "madde bir", "iç madde", "sıralı", "ikinci", "kod bloğu", "alıntı satırı",
          "bağlantı metni", "Ad", "Değer", "şehir", "İzmir", "son"})
        CHECK_MESSAGE(contains(got.text, expected), expected);
    CHECK_FALSE(contains(got.text, "x.example"));
    CHECK_FALSE(contains(got.text, "|"));
    CHECK_FALSE(contains(got.text, "```"));
    CHECK(contains(got.text, "•"));
    CHECK(contains(got.text, "2."));
    check_inside_margins(got.bbox);
}

TEST_CASE("image alt text replaces every image, with a placeholder when empty") {
    core::init_i18n("en");
    const auto got =
        extract("önce ![gizli alt](a.png) ve ![](file:///x/id.jpg) sonra", pdf::NoteFormat::Markdown);
    CHECK(contains(got.text, "önce"));
    CHECK(contains(got.text, "gizli alt"));
    CHECK(contains(got.text, "[image]"));
    CHECK(contains(got.text, "sonra"));
    CHECK_FALSE(contains(got.text, "a.png"));
    CHECK_FALSE(contains(got.text, "id.jpg"));
}

TEST_CASE("raw HTML stays literal text") {
    const auto got = extract("<img src=\"file://attacker.example/b.png\">\n\nmetin <b>kalın</b>",
                             pdf::NoteFormat::Markdown);
    CHECK(contains(got.text, "<img"));
    CHECK(contains(got.text, "<b>"));
}

TEST_CASE("non-markdown files are not parsed as markdown") {
    const auto got = extract("# değil başlık\n**yıldız**\n", pdf::NoteFormat::PlainText);
    CHECK(contains(got.text, "# değil başlık"));
    CHECK(contains(got.text, "**yıldız**"));
}

// DejaVu Mono işarete genişlik verir, satırlar kırılır; DejaVu Sans'ta sıfır genişlikli işaretler tek parçada
// birikir. İkisi de libharu'dan hatasız geçmeli.
TEST_CASE("a long run of combining marks exports and plain text keeps every mark") {
    constexpr std::size_t kMarks = 30000;
    std::string marks;
    for (std::size_t i = 0; i < kMarks; ++i)
        marks += "\xCC\x81";  // U+0301
    const auto plain = extract(marks, pdf::NoteFormat::PlainText);
    std::size_t found = 0;
    for (auto at = plain.text.find("\xCC\x81"); at != std::string::npos;
         at = plain.text.find("\xCC\x81", at + 2))
        ++found;
    CHECK(found == kMarks);
    check_inside_margins(plain.bbox);
    // pdftotext üst üste binen aynı glifleri teke indirir; burada yalnız hatasız dışa aktarım ve taban harf
    // denetlenir.
    const auto markdown = extract("taban" + marks, pdf::NoteFormat::Markdown);
    CHECK(contains(markdown.text, "taban"));
}

TEST_CASE("hostile structure stays inside the margins and keeps its text") {
    SUBCASE("deep quote nesting") {
        const auto got = extract(std::string(300, '>') + " derin metin burada\n", pdf::NoteFormat::Markdown);
        check_inside_margins(got.bbox);
        CHECK(contains(without_whitespace(got.text), "derinmetinburada"));
    }
    SUBCASE("deep list nesting") {
        std::string list;
        for (int i = 0; i < 100; ++i)
            list += std::string(static_cast<std::size_t>(i) * 2, ' ') + "- iç" + std::to_string(i) + "\n";
        const auto got = extract(list, pdf::NoteFormat::Markdown);
        check_inside_margins(got.bbox);
        CHECK(contains(without_whitespace(got.text), "iç99"));
    }
    SUBCASE("table with very many columns") {
        std::string header = "|";
        std::string rule = "|";
        std::string row = "|";
        for (int i = 0; i < 60; ++i) {
            header += "b" + std::to_string(i) + "|";
            rule += "-|";
            row += "h" + std::to_string(i) + "|";
        }
        const auto got = extract(header + "\n" + rule + "\n" + row + "\n", pdf::NoteFormat::Markdown);
        check_inside_margins(got.bbox);
        CHECK(contains(got.text, "h59"));
    }
    SUBCASE("table cell wider than its column") {
        const auto got =
            extract("|a|b|\n|-|-|\n|" + std::string(400, 'x') + "|y|\n", pdf::NoteFormat::Markdown);
        check_inside_margins(got.bbox);
        CHECK(std::count(got.text.begin(), got.text.end(), 'x') == 400);
    }
}
