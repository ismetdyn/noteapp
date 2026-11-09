// PDF1: MuPDF motoru (pdfview::PdfEngine), süreç içinde. Süre/iptal ve işçi süreci: pdf_document_test.cpp.
#include "core/i18n.hpp"
#include "core/limits.hpp"
#include "core/text.hpp"
#include "pdfview/engine/pdf_engine.hpp"
#include "pdfview/fixtures.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace noteapp;
using namespace noteapp::pdftest;
using pdfview::PdfEngine;
using pdfview::RenderedPage;

namespace {

constexpr int kOpaque = 255;

PdfEngine open_ok(std::string_view bytes, const core::PdfLimits& limits = unlimited()) {
    auto opened = PdfEngine::open(bytes, limits);
    if (!opened)
        FAIL(core::format_error(opened.error()));
    return std::move(*opened);
}

void check_out_of_memory(const core::Error& error) {
    CHECK(error.code == core::Error::Code::LimitExceeded);
    CHECK(error.message_key == "error.pdf_out_of_memory");
}

// Bozuk girdi sözleşmesi: açılır ya da error.pdf_invalid (InvalidDocument, iletisiz); açılırsa her sayfa ya okunur
// ya da error.pdf_page_invalid döner. Başka kod (bellek sınırı, iptal) bozuk girdinin sonucu olamaz.
struct UntrustedOutcome {
    int opened = 0;
    int rejected = 0;
};

void exercise_untrusted(std::string_view bytes, UntrustedOutcome& outcome) {
    auto opened = PdfEngine::open(bytes, unlimited());
    if (!opened) {
        ++outcome.rejected;
        CHECK(opened.error().code == core::Error::Code::InvalidDocument);
        CHECK(opened.error().message_key == "error.pdf_invalid");
        CHECK(opened.error().args.empty());
        return;
    }
    ++outcome.opened;
    CHECK(opened->page_count() >= 0);
    const auto check_page_error = [](const core::Error& error) {
        CHECK(error.code == core::Error::Code::InvalidDocument);
        CHECK(error.message_key == "error.pdf_page_invalid");
    };
    for (int page = 0; page < opened->page_count(); ++page) {
        CAPTURE(page);
        if (const auto text = opened->page_text(page); text)
            CHECK(core::is_valid_utf8(*text));
        else
            check_page_error(text.error());
        if (const auto image = opened->render_page(page, 0.5F); image)
            CHECK(image->rgba.size() == static_cast<std::size_t>(image->width) * image->height * 4U);
        else
            check_page_error(image.error());
    }
}

}  // namespace

TEST_CASE("opens a PDF from memory and reads the page count and each page's text in page order") {
    const std::string bytes = ascii_pdf(3);
    PdfEngine document = open_ok(bytes);
    REQUIRE(document.page_count() == 3);
    for (int page = 0; page < 3; ++page) {
        CAPTURE(page);
        const auto text = document.page_text(page);
        REQUIRE(text);
        CHECK(contains(*text, "Page " + std::to_string(page + 1)));
    }
}

TEST_CASE("keeps its own copy of the bytes: the caller's buffer may go away after open") {
    auto bytes = std::make_unique<std::string>(ascii_pdf(1));
    PdfEngine document = open_ok(*bytes);
    bytes.reset();
    const auto text = document.page_text(0);
    REQUIRE(text);
    CHECK(contains(*text, "Page 1"));
}

TEST_CASE("extracts Turkish text as UTF-8") {
    PdfEngine document = open_ok(exported_pdf("Merhaba çğıİöşü ÇĞIÖŞÜ"));
    REQUIRE(document.page_count() == 1);
    const auto text = document.page_text(0);
    REQUIRE(text);
    CAPTURE(*text);
    CHECK(contains(*text, "Merhaba çğıİöşü ÇĞIÖŞÜ"));
}

TEST_CASE("page text is valid UTF-8 without control characters even when ToUnicode maps to a surrogate, ESC and NUL") {
    PdfEngine document = open_ok(hostile_to_unicode_pdf());
    const auto text = document.page_text(0);
    REQUIRE(text);
    CAPTURE(*text);
    CHECK(core::is_valid_utf8(*text));
    CHECK_FALSE(contains(*text, "\x1B"));
    CHECK_FALSE(contains(*text, "\x7F"));      // DEL
    CHECK_FALSE(contains(*text, "\xC2\x9B"));  // U+009B (C1 CSI): bazı terminallerde ESC [ gibi işlenir
    CHECK(text->find('\0') == std::string::npos);
    CHECK(std::ranges::none_of(*text, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 && byte != '\n' && byte != '\t';
    }));
    CHECK(contains(*text, "A"));
    CHECK(contains(*text, "B"));
    CHECK(contains(*text, "\xEF\xBF\xBD"));  // eşlenen denetim karakterleri görünür U+FFFD oldu
}

TEST_CASE("renders a page as opaque RGBA at the requested scale") {
    PdfEngine document = open_ok(ascii_pdf(1));
    const auto rendered = document.render_page(0, 2.0F);
    REQUIRE(rendered);
    CHECK(rendered->width == 400);
    CHECK(rendered->height == 200);
    REQUIRE(rendered->rgba.size() == 400U * 200U * 4U);
    bool all_opaque = true;
    bool has_ink = false;
    for (std::size_t i = 0; i < rendered->rgba.size(); i += 4) {
        all_opaque = all_opaque && rendered->rgba[i + 3] == kOpaque;
        has_ink = has_ink || rendered->rgba[i] < 128;
    }
    CHECK(all_opaque);
    CHECK(has_ink);  // "Page 1" çizildi
    CHECK(rendered->rgba[0] == 255);  // sol üst köşe beyaz zemin
    CHECK(rendered->rgba[1] == 255);
    CHECK(rendered->rgba[2] == 255);

    const auto half = document.render_page(0, 0.5F);
    REQUIRE(half);
    CHECK(half->width == 100);
    CHECK(half->height == 50);
}

TEST_CASE("renders a JPEG 2000 image") {
    PdfEngine document = open_ok(jpx_pdf());
    const auto rendered = document.render_page(0, 4.0F);
    REQUIRE(rendered);
    REQUIRE(rendered->width == 64);
    REQUIRE(rendered->height == 64);
    // Sol üst 4x4 piksel görselin (0, 0) noktası: r = 0, g = 0, b = 128.
    const std::size_t at = (2U * 64U + 2U) * 4U;
    CAPTURE(static_cast<int>(rendered->rgba[at]));
    CAPTURE(static_cast<int>(rendered->rgba[at + 1]));
    CAPTURE(static_cast<int>(rendered->rgba[at + 2]));
    CHECK(rendered->rgba[at] < 40);
    CHECK(rendered->rgba[at + 1] < 40);
    CHECK(rendered->rgba[at + 2] > 100);
    CHECK(rendered->rgba[at + 2] < 156);
}

TEST_CASE("rejects bytes that are not a PDF with an InvalidDocument error that carries no MuPDF text") {
    for (const std::string_view bytes : {std::string_view{}, std::string_view{"hello, not a pdf"},
                                         std::string_view{"%PDF-1.7\n"}}) {
        CAPTURE(bytes);
        const auto opened = PdfEngine::open(bytes, unlimited());
        REQUIRE_FALSE(opened);
        CHECK(opened.error().code == core::Error::Code::InvalidDocument);
        CHECK(opened.error().message_key == "error.pdf_invalid");
        CHECK(opened.error().args.empty());  // MuPDF iletisi (belge baytı içerebilir) kullanıcıya gitmez
    }
}

TEST_CASE("truncated and corrupted PDFs either open or fail with InvalidDocument, never anything else") {
    const std::string valid = ascii_pdf(2);
    SUBCASE("truncated at many lengths") {
        UntrustedOutcome outcome;
        for (std::size_t length = 0; length < valid.size(); length += std::max<std::size_t>(1, valid.size() / 40))
            exercise_untrusted(std::string_view(valid).substr(0, length), outcome);
        CHECK(outcome.rejected > 0);  // kısa kesikler açılmaz
        CHECK(outcome.opened > 0);    // nesneleri tamam, xref'i eksik kesikler onarılarak açılır
    }
    SUBCASE("bytes flipped across the file") {
        UntrustedOutcome outcome;
        for (std::size_t stride = 7; stride < 200; stride += 31) {
            std::string corrupted = valid;
            for (std::size_t i = stride; i < corrupted.size(); i += stride)
                corrupted[i] = static_cast<char>(corrupted[i] ^ 0x5A);
            exercise_untrusted(corrupted, outcome);
        }
        CHECK(outcome.opened + outcome.rejected == 7);
    }
    SUBCASE("the header alone is not a document") {
        const auto opened = PdfEngine::open(std::string_view(valid).substr(0, 16), unlimited());
        REQUIRE_FALSE(opened);
        CHECK(opened.error().code == core::Error::Code::InvalidDocument);
    }
    SUBCASE("a file cut before its xref table is repaired and reads normally") {
        const std::size_t xref = valid.rfind("xref");
        REQUIRE(xref != std::string::npos);
        PdfEngine document = open_ok(std::string_view(valid).substr(0, xref));
        REQUIRE(document.page_count() == 2);
        const auto text = document.page_text(1);
        REQUIRE(text);
        CHECK(contains(*text, "Page 2"));
    }
    SUBCASE("an empty MediaBox is drawn as US Letter, as MuPDF substitutes it") {
        PdfBuilder builder;
        PdfEngine document = open_ok(finish_pdf(builder, {{.media_box = "[0 0 0 0]",
                                                             .resources = std::string(kHelvetica),
                                                             .content = "BT /F1 24 Tf 10 40 Td (zero) Tj ET"}}));
        REQUIRE(document.page_count() == 1);
        const auto rendered = document.render_page(0, 1.0F);
        REQUIRE(rendered);
        CHECK(rendered->width == 612);
        CHECK(rendered->height == 792);
        const auto text = document.page_text(0);
        REQUIRE(text);
        CHECK(contains(*text, "zero"));
    }
}

TEST_CASE("rejects a password-protected PDF with an Encrypted error") {
    const auto opened = PdfEngine::open(ascii_pdf(1, {.owner = "owner-secret", .user = "user-secret"}), unlimited());
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == core::Error::Code::Encrypted);
    CHECK(opened.error().message_key == "error.pdf_encrypted");
}

TEST_CASE("opens an encrypted PDF whose user password is empty (owner password only restricts permissions)") {
    PdfEngine document = open_ok(ascii_pdf(1, {.owner = "owner-secret", .user = ""}));
    const auto text = document.page_text(0);
    REQUIRE(text);
    CHECK(contains(*text, "Page 1"));
}

TEST_CASE("limits.pdf.pages rejects a document with more pages, inclusive at the limit") {
    const std::string bytes = ascii_pdf(3);
    core::PdfLimits limits = unlimited();
    limits.pages = limit_of(2);
    const auto rejected = PdfEngine::open(bytes, limits);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == core::Error::Code::LimitExceeded);
    CHECK(rejected.error().message_key == "error.pdf_too_many_pages");
    CHECK(has_arg(rejected.error(), "count", "3"));

    limits.pages = limit_of(3);
    CHECK(PdfEngine::open(bytes, limits).has_value());
}

TEST_CASE("limits.pdf.render_pixels refuses to draw a larger page, inclusive at the limit") {
    core::PdfLimits limits = unlimited();
    limits.render_pixels = limit_of(200U * 100U - 1U);
    PdfEngine small_limit = open_ok(ascii_pdf(1), limits);
    const auto refused = small_limit.render_page(0, 1.0F);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == core::Error::Code::LimitExceeded);
    CHECK(refused.error().message_key == "error.pdf_render_too_large");
    CHECK(small_limit.render_page(0, 0.5F).has_value());  // daha küçük yakınlaştırma sığar

    limits.render_pixels = limit_of(200U * 100U);
    PdfEngine exact = open_ok(ascii_pdf(1), limits);
    CHECK(exact.render_page(0, 1.0F).has_value());
}

TEST_CASE("limits.pdf.text_bytes rejects a page whose text is larger, inclusive at the limit") {
    const std::string bytes = ascii_pdf(1);
    const auto full = open_ok(bytes).page_text(0);
    REQUIRE(full);
    REQUIRE(full->size() > 1);

    core::PdfLimits limits = unlimited();
    limits.text_bytes = limit_of(full->size() - 1);
    PdfEngine too_small = open_ok(bytes, limits);
    const auto refused = too_small.page_text(0);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == core::Error::Code::LimitExceeded);
    CHECK(refused.error().message_key == "error.pdf_text_too_large");

    limits.text_bytes = limit_of(full->size());
    PdfEngine exact = open_ok(bytes, limits);
    const auto text = exact.page_text(0);
    REQUIRE(text);
    CHECK(*text == *full);
}

TEST_CASE("limits.pdf.context_memory refuses to open when the context alone does not fit") {
    const std::string bytes = ascii_pdf(1);
    core::PdfLimits limits = unlimited();
    limits.context_memory = core::Limit{64U * 1024U, "64KiB"};
    const auto refused = PdfEngine::open(bytes, limits);
    REQUIRE_FALSE(refused);
    check_out_of_memory(refused.error());
    CHECK(has_arg(refused.error(), "limit", "64KiB"));

    limits.context_memory = core::Limit{512U * 1024U * 1024U, "512MiB"};  // defaults.toml değeri
    PdfEngine enough = open_ok(bytes, limits);
    CHECK(enough.render_page(0, 1.0F).has_value());
}

TEST_CASE("limits.pdf.context_memory stops an operation after a successful open") {
    core::PdfLimits limits = unlimited();
    limits.context_memory = core::Limit{8U * 1024U * 1024U, "8MiB"};

    SUBCASE("a large Flate image needs more than the budget to decode (malloc path)") {
        PdfEngine document = open_ok(big_image_pdf(), limits);
        REQUIRE(document.page_count() == 2);
        const auto rendered = document.render_page(0, 1.0F);
        REQUIRE_FALSE(rendered);
        check_out_of_memory(rendered.error());
        // Reddin etkisi o işlemle sınırlı: sonraki işlemler normal sonuçlanır.
        const auto text = document.page_text(1);
        REQUIRE(text);
        CHECK(contains(*text, "ok"));
        CHECK(document.render_page(1, 1.0F).has_value());
    }
    SUBCASE("a 24 MiB string grows the lexer buffer past the budget (realloc path)") {
        PdfEngine document = open_ok(huge_string_pdf(), limits);
        const auto text = document.page_text(0);
        REQUIRE_FALSE(text);
        check_out_of_memory(text.error());
        const auto rendered = document.render_page(0, 0.25F);
        REQUIRE_FALSE(rendered);
        check_out_of_memory(rendered.error());
    }
    SUBCASE("without the budget both pages read") {
        PdfEngine document = open_ok(big_image_pdf());
        const auto rendered = document.render_page(0, 1.0F);
        REQUIRE(rendered);
        CHECK(rendered->rgba[0] == 0x40);  // görselin rengi
    }
}

TEST_CASE("PDF errors have catalog messages in both languages") {
    const std::vector<core::Error> errors = {
        {core::Error::Code::InvalidDocument, "error.pdf_invalid", {}},
        {core::Error::Code::InvalidDocument, "error.pdf_page_invalid", {{"page", "2"}}},
        {core::Error::Code::Encrypted, "error.pdf_encrypted", {}},
        {core::Error::Code::LimitExceeded, "error.pdf_too_many_pages", {{"count", "3"}, {"limit", "2"}}},
        {core::Error::Code::LimitExceeded,
         "error.pdf_render_too_large",
         {{"page", "1"}, {"width", "4"}, {"height", "5"}, {"limit", "6"}}},
        {core::Error::Code::LimitExceeded, "error.pdf_text_too_large", {{"page", "1"}, {"size", "9"}, {"limit", "8"}}},
        {core::Error::Code::LimitExceeded, "error.pdf_out_of_memory", {{"limit", "64KiB"}}},
        {core::Error::Code::LimitExceeded, "error.pdf_no_memory", {{"page", "1"}, {"width", "4"}, {"height", "5"}}},
        {core::Error::Code::LimitExceeded, "error.pdf_too_slow", {{"limit", "10s"}}},
        {core::Error::Code::Cancelled, "error.pdf_cancelled", {}},
    };
    for (const char* language : {"en", "tr"}) {
        core::init_i18n(language);
        for (const core::Error& error : errors) {
            CAPTURE(language);
            CAPTURE(error.message_key);
            const std::string text = core::format_error(error);
            CHECK_FALSE(contains(text, "error.pdf"));  // anahtar çevrildi, kendisi basılmadı
            CHECK_FALSE(contains(text, "{"));          // her yer tutucu dolduruldu
        }
    }
    core::init_i18n("en");
}

// Ortak kilitlerin nedeni JPX: openjpeg ayırıcısı süreç geneli bir bağlam işaretçisi kullanır. İki iş parçacığı
// ayrı belgelerde aynı anda JPX çözer (TSan/ASan altında yarış burada görünür).
TEST_CASE("documents on different threads render text and JPEG 2000 images concurrently") {
    const std::string text_bytes = ascii_pdf(2);
    const std::string jpx_bytes = jpx_pdf();
    constexpr int kRounds = 10;
    std::array<int, 2> failures{};
    {
        std::vector<std::jthread> workers;
        for (std::size_t worker = 0; worker < failures.size(); ++worker)
            workers.emplace_back([&, worker] {
                for (int round = 0; round < kRounds; ++round) {
                    auto text_doc = PdfEngine::open(text_bytes, unlimited());
                    auto jpx_doc = PdfEngine::open(jpx_bytes, unlimited());
                    if (!text_doc || !jpx_doc || !text_doc->render_page(round % 2, 1.0F) ||
                        !text_doc->page_text(round % 2) || !jpx_doc->render_page(0, 2.0F))
                        ++failures[worker];
                }
            });
    }
    CHECK(failures[0] == 0);
    CHECK(failures[1] == 0);
}
