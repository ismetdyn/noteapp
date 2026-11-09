// PDF test fixture'ları: depoda ikili dosya yok, belgeler testte üretilir (bkz. fixtures.hpp).
#include "pdfview/fixtures.hpp"

#include "core/i18n.hpp"
#include "pdf/pdf_export.hpp"
#include "support/test_support.hpp"

#include <hpdf.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <format>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace noteapp::pdftest {

constexpr float kPageWidth = 200.0F;  // pt; 1 kat = 72 dpi = 1 piksel/pt
constexpr float kPageHeight = 100.0F;

// Sınırsız: her alan Limit{0, ""}.
core::PdfLimits unlimited() {
    return {};
}

core::Limit limit_of(std::uint64_t value) {
    return core::Limit{value, std::to_string(value)};
}

struct HaruDeleter {
    void operator()(HPDF_Doc doc) const { HPDF_Free(doc); }
};
using HaruDoc = std::unique_ptr<std::remove_pointer_t<HPDF_Doc>, HaruDeleter>;

std::string saved_bytes(HPDF_Doc doc) {
    REQUIRE(HPDF_SaveToStream(doc) == HPDF_OK);
    const HPDF_UINT32 size = HPDF_GetStreamSize(doc);
    REQUIRE(size > 0);
    REQUIRE(HPDF_ResetStream(doc) == HPDF_OK);
    std::string bytes(size, '\0');
    HPDF_UINT32 read = size;
    const HPDF_STATUS status = HPDF_ReadFromStream(doc, reinterpret_cast<HPDF_BYTE*>(bytes.data()), &read);
    REQUIRE((status == HPDF_OK || status == HPDF_STREAM_EOF));
    REQUIRE(read == size);
    return bytes;
}

// `pages` sayfalık PDF; sayfa i'de "Page i" (1'den) yazar. Parola verilirse RC4-128 ile şifrelenir.
std::string ascii_pdf(int pages, Passwords passwords) {
    const HaruDoc doc(HPDF_New(nullptr, nullptr));
    REQUIRE(doc != nullptr);
    const HPDF_Font font = HPDF_GetFont(doc.get(), "Helvetica", nullptr);
    REQUIRE(font != nullptr);
    for (int i = 1; i <= pages; ++i) {
        const HPDF_Page page = HPDF_AddPage(doc.get());
        REQUIRE(page != nullptr);
        REQUIRE(HPDF_Page_SetWidth(page, kPageWidth) == HPDF_OK);
        REQUIRE(HPDF_Page_SetHeight(page, kPageHeight) == HPDF_OK);
        REQUIRE(HPDF_Page_BeginText(page) == HPDF_OK);
        REQUIRE(HPDF_Page_SetFontAndSize(page, font, 24) == HPDF_OK);
        const std::string text = "Page " + std::to_string(i);
        REQUIRE(HPDF_Page_TextOut(page, 20, 40, text.c_str()) == HPDF_OK);
        REQUIRE(HPDF_Page_EndText(page) == HPDF_OK);
    }
    if (passwords.owner != nullptr) {
        REQUIRE(HPDF_SetPassword(doc.get(), passwords.owner, passwords.user) == HPDF_OK);
        REQUIRE(HPDF_SetEncryptionMode(doc.get(), HPDF_ENCRYPT_R3, 16) == HPDF_OK);
    }
    return saved_bytes(doc.get());
}

std::string exported_pdf(std::string_view text) {
    const test::TempDir dir;
    const auto out = dir.path() / "fixture.pdf";
    const auto result = pdf::export_pdf(text, pdf::NoteFormat::PlainText, out);
    if (!result)
        FAIL(core::format_error(result.error()));
    return test::slurp(out);
}

std::string ref(int number) {
    return std::format("{} 0 R", number);
}

std::string stream_object(std::string_view dictionary_entries, std::string_view data) {
    return std::format("<< /Length {} {} >>\nstream\n", data.size(), dictionary_entries) + std::string(data) +
           "\nendstream";
}

std::string flate(std::string_view data) {
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    std::string out(size, '\0');
    REQUIRE(compress2(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(data.data()),
                      static_cast<uLong>(data.size()), Z_BEST_COMPRESSION) == Z_OK);
    out.resize(size);
    return out;
}

std::string finish_pdf(PdfBuilder& builder, const std::vector<PageSpec>& pages) {
    const int catalog = builder.reserve();
    const int tree = builder.reserve();
    std::string kids;
    for (const PageSpec& page : pages) {
        const std::string stream = page.flate_content
                                       ? stream_object("/Filter /FlateDecode", flate(page.content))
                                       : stream_object("", page.content);
        const int content = builder.add(stream);
        const int number = builder.add(std::format("<< /Type /Page /Parent {} /MediaBox {} /Resources {} /Contents {} >>",
                                                   ref(tree), page.media_box, page.resources, ref(content)));
        kids += ref(number) + " ";
    }
    builder.set(catalog, std::format("<< /Type /Catalog /Pages {} >>", ref(tree)));
    builder.set(tree, std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kids, pages.size()));
    return builder.build(catalog);
}

// Her düzey bir alttakini 10 kez çizen 9 düzeylik Form XObject zinciri: 10^9 yaprak. Küçük dosya, kesmesiz çizim ya
// da metin çıkarma saatler sürer (security review Sev-2).
std::string fan_out_pdf(bool plain_second_page) {
    constexpr int kLevels = 9;
    constexpr int kFanOut = 10;
    PdfBuilder builder;
    int next = builder.add(stream_object("/Type /XObject /Subtype /Form /BBox [0 0 100 100]", "0 0 1 1 re f"));
    for (int level = 0; level < kLevels; ++level) {
        std::string content;
        for (int i = 0; i < kFanOut; ++i)
            content += "/N Do\n";
        next = builder.add(stream_object(
            std::format("/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << /XObject << /N {} >> >>",
                        ref(next)),
            content));
    }
    std::vector<PageSpec> pages{{.resources = std::format("<< /XObject << /F {} >> >>", ref(next)), .content = "/F Do"}};
    if (plain_second_page)
        pages.push_back({.resources = std::string(kHelvetica), .content = "BT /F1 24 Tf 10 40 Td (calm) Tj ET"});
    return finish_pdf(builder, pages);
}

// ToUnicode'u 01 -> U+D800 (tek vekil), 02 -> ESC, 03 -> NUL, 04 -> U+009B (C1 CSI), 05 -> DEL eşleyen font;
// "A\001\002\003\004\005B" yazar.
std::string hostile_to_unicode_pdf() {
    constexpr std::string_view kCMap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CMapName /Hostile def\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n5 beginbfchar\n<01> <D800>\n<02> <001B>\n"
        "<03> <0000>\n<04> <009B>\n<05> <007F>\nendbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\n"
        "end\nend\n";
    PdfBuilder builder;
    const int cmap = builder.add(stream_object("", kCMap));
    const int font = builder.add(
        std::format("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode {} >>", ref(cmap)));
    return finish_pdf(builder, {{.resources = std::format("<< /Font << /F1 {} >> >>", ref(font)),
                                 .content = "BT /F1 24 Tf 10 40 Td (A\\001\\002\\003\\004\\005B) Tj ET"}});
}

// Deflate (RFC 1951) bit yazıcısı: alanlar düşük bitten, Huffman kodları yüksek bitten yazılır.
class BitWriter {
public:
    void put_bits(std::uint64_t value, int count) {
        accumulator_ |= value << used_;
        used_ += count;
        while (used_ >= 8) {
            out_ += static_cast<char>(accumulator_ & 0xFFU);
            accumulator_ >>= 8U;
            used_ -= 8;
        }
    }
    void put_huffman(std::uint32_t code, int length) {
        std::uint32_t reversed = 0;
        for (int i = 0; i < length; ++i)
            reversed |= ((code >> i) & 1U) << (length - 1 - i);
        put_bits(reversed, length);
    }
    std::string finish() {
        if (used_ > 0)
            out_ += static_cast<char>(accumulator_ & 0xFFU);
        return std::move(out_);
    }
    void reserve(std::size_t bytes) { out_.reserve(bytes); }

private:
    std::uint64_t accumulator_ = 0;
    int used_ = 0;
    std::string out_;
};

// Açılınca ~8,6 GB "1 0 1 0 ..." veren zlib akışı, elle: sabit Huffman bloğunda "1 0 " ve ardından 4 bayt geriye
// 258'lik 33 milyon kopya (kopya başına 13 bit, ~54 MB). zlib'le 8,6 GB'ı sıkıştırmak testte saniyeler sürerdi.
std::string endless_pairs_zlib() {
    constexpr std::uint32_t kMatches = 33'000'000;
    constexpr std::uint32_t kLiteralBase = 0x30;  // 0-143 değişmezleri: 8 bit, 0x30 + değer
    BitWriter writer;
    writer.reserve(kMatches / 8U * 13U + 64U);
    writer.put_bits(0x78, 8);  // zlib başlığı: deflate, 32 KiB pencere
    writer.put_bits(0x01, 8);  // (0x7801 % 31 == 0)
    writer.put_bits(1, 1);     // BFINAL
    writer.put_bits(1, 2);     // BTYPE = sabit Huffman
    for (const char literal : std::string_view("1 0 "))
        writer.put_huffman(kLiteralBase + static_cast<unsigned char>(literal), 8);
    BitWriter one_match;
    one_match.put_huffman(0xC5, 8);  // uzunluk 258 (sembol 285)
    one_match.put_huffman(3, 5);     // uzaklık 4 (kod 3)
    std::uint64_t match_bits = 0;
    const std::string match_bytes = one_match.finish();
    for (std::size_t i = 0; i < match_bytes.size(); ++i)
        match_bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(match_bytes[i])) << (8 * i);
    for (std::uint32_t i = 0; i < kMatches; ++i)
        writer.put_bits(match_bits, 13);
    writer.put_huffman(0, 7);  // blok sonu (256)
    std::string stream = writer.finish();
    stream.append(4, '\0');  // adler32 (açılan sona hiç varılmaz)
    return stream;
}

// xref'siz dosyada tek nesne akışı: /N = INT_MAX, iki kat Flate. Açılış onarıma düşer; MuPDF onarımda /N kez
// sözcük okur (bellek ayırmadan): yamasız, ~20 KB'lık dosya dakikalarca döner (security review round 3, Sev-2).
std::string repair_spin_pdf() {
    const std::string inner = endless_pairs_zlib();
    uLongf size = compressBound(static_cast<uLong>(inner.size()));
    std::string outer(size, '\0');
    REQUIRE(compress2(reinterpret_cast<Bytef*>(outer.data()), &size, reinterpret_cast<const Bytef*>(inner.data()),
                      static_cast<uLong>(inner.size()), Z_BEST_SPEED) == Z_OK);
    outer.resize(size);
    return "%PDF-1.7\n1 0 obj\n<< /Type /ObjStm /N 2147483647 /First 0 /Filter [/FlateDecode /FlateDecode] "
           "/Length " +
           std::to_string(outer.size()) + " >>\nstream\n" + outer + "\nendstream\nendobj\n";
}

// Bağlantı hedefinin yavaş sayfa araması (pdf-link.c'den pdf_lookup_page_number_slow, pdf-page.c): sayfa ağacı
// haritası kurulamaz (kökte /Count 1 ile iki sayfa: "too many kids"), her bağlantının /Dest'i sayfanın D düzeylik ata
// zincirini yürür ve her düzeyde paylaşılan K + D öğeli Kids dizisini tarar. Sayfa yüklenirken (fz_load_page) L
// bağlantı x D x (K + D/2) adım: ~90 KB dosya dakikalarca döner, MuPDF bu döngüde hiçbir kesmeye bakmaz.
std::string link_lookup_pdf() {
    constexpr int kLinks = 2000;
    constexpr int kDepth = 1000;
    constexpr int kKids = 2000;
    PdfBuilder builder;
    const int catalog = builder.reserve();
    const int root = builder.reserve();
    const int page = builder.reserve();
    const int spare = builder.reserve();
    const int shared_kids = builder.reserve();
    std::vector<int> ancestors(kDepth);
    for (int& ancestor : ancestors)
        ancestor = builder.reserve();
    const int link =
        builder.add(std::format("<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] /Dest [{} /Fit] >>", ref(page)));
    const int content = builder.add(stream_object("", "BT /F1 12 Tf 10 50 Td (links) Tj ET"));
    std::string annots;
    for (int i = 0; i < kLinks; ++i)
        annots += ref(link) + " ";
    builder.set(page, std::format("<< /Type /Page /Parent {} /MediaBox [0 0 100 100] /Resources {} /Contents {} "
                                  "/Annots [{}] >>",
                                  ref(ancestors[0]), kHelvetica, ref(content), annots));
    builder.set(spare, std::format("<< /Type /Page /Parent {} /MediaBox [0 0 100 100] >>", ref(root)));
    builder.set(root, std::format("<< /Type /Pages /Kids [{} {}] /Count 1 >>", ref(page), ref(spare)));
    std::string kids;
    for (int i = 0; i < kKids; ++i)
        kids += "<<>> ";
    kids += ref(page);
    for (int i = 0; i + 1 < kDepth; ++i)
        kids += " " + ref(ancestors[static_cast<std::size_t>(i)]);
    builder.set(shared_kids, "[" + kids + "]");
    for (std::size_t i = 0; i < ancestors.size(); ++i) {
        const std::string parent = i + 1 < ancestors.size() ? "/Parent " + ref(ancestors[i + 1]) : "";
        builder.set(ancestors[i], std::format("<< /Type /Pages /Kids {} /Count 0 {} >>", ref(shared_kids), parent));
    }
    builder.set(catalog, std::format("<< /Type /Catalog /Pages {} >>", ref(root)));
    return builder.build(catalog);
}

// Sayfa 1: tek büyük Flate görsel (dosya küçük, çözülünce büyük). Sayfa 2: "ok" metni.
std::string big_image_pdf() {
    PdfBuilder builder;
    const std::string pixels(static_cast<std::size_t>(kBigImageSide) * kBigImageSide * 3, '\x40');
    const int image = builder.add(stream_object(
        std::format("/Type /XObject /Subtype /Image /Width {0} /Height {0} /ColorSpace /DeviceRGB "
                    "/BitsPerComponent 8 /Filter /FlateDecode",
                    kBigImageSide),
        flate(pixels)));
    const std::string side = std::to_string(kBigImageSide);
    return finish_pdf(builder,
                      {{.media_box = std::format("[0 0 {0} {0}]", side),
                        .resources = std::format("<< /XObject << /Im {} >> >>", ref(image)),
                        .content = std::format("{0} 0 0 {0} 0 0 cm /Im Do", side)},
                       {.resources = std::string(kHelvetica), .content = "BT /F1 24 Tf 10 40 Td (ok) Tj ET"}});
}

// İçerik akışında 24 MiB'lık tek dizge: MuPDF'in sözcük tamponu (pdf_lexbuf) fz_realloc ile ikiye katlanarak büyür.
std::string huge_string_pdf() {
    constexpr std::size_t kStringBytes = 24U * 1024U * 1024U;
    PdfBuilder builder;
    return finish_pdf(builder,
                      {{.content = "q\n(" + std::string(kStringBytes, 'a') + ")\nQ\n", .flate_content = true}});
}

// 16x16 RGB JPEG 2000 kod akışı (r = x*16, g = y*16, b = 128), kayıpsız. Üretim: Pillow 12.1.1 (OpenJPEG 2.5.4),
// Image.save(format="JPEG2000", irreversible=False, num_resolutions=1).
constexpr std::string_view kTinyJ2kHex =
    "ff4fff51002f000000000010000000100000000000000000000000100000001000000000000000000003070101070101070101ff52000c"
    "00000001000004040001ff5c00044040ff640025000143726561746564206279204f70656e4a5045472076657273696f6e20322e352e"
    "34ff90000a0000000000a10001ff93df8258116b32aee61c64e9728d13627118b0379ee7ec4991f618eb0be6c29d617cd7f7d37c5021"
    "4204715e20adbafe369e974d8adbafbcd874ba6c56dd7de6c3a5d35478400c1ba5b203ff7fff7fdf8208128eb9aa89ff47216c8d4e72"
    "063f09099c9a4c5f82d3637ddf6a52739ce739ce73dec90d14aa00000000009fe5410c631ee600005de8c95000000000271d7fff7f80"
    "ffd9";

std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out += static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16));
    return out;
}

// 16x16 pt sayfayı JPX görselle doldurur: çizim openjpeg'den geçer (süreç geneli ayırıcı durumu, ortak kilitler).
std::string jpx_pdf() {
    PdfBuilder builder;
    const int image = builder.add(stream_object(
        "/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8 "
        "/Filter /JPXDecode",
        from_hex(kTinyJ2kHex)));
    return finish_pdf(builder, {{.media_box = "[0 0 16 16]",
                                 .resources = std::format("<< /XObject << /Im {} >> >>", ref(image)),
                                 .content = "16 0 0 16 0 0 cm /Im Do"}});
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool has_arg(const core::Error& error, std::string_view name, std::string_view value) {
    return std::ranges::any_of(error.args, [&](const auto& arg) { return arg.first == name && arg.second == value; });
}

}  // namespace noteapp::pdftest
