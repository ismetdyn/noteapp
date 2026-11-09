// PDF1b: istemci (pdfview::PdfDocument) gerçek `note __pdf-worker` ve bozuk davranan sahte işçiyle. Süre ve iptal
// işçiyi öldürerek sınırlanır; MuPDF'in kesilemeyen döngüleri (onarım, bağlantı hedefi araması, Form XObject
// çoğalması) burada sınanır.
#include "core/child_process.hpp"
#include "core/i18n.hpp"
#include "core/limits.hpp"
#include "core/paths.hpp"
#include "pdfview/engine/pdf_engine.hpp"
#include "pdfview/fixtures.hpp"
#include "pdfview/pdf_document.hpp"
#include "pdfview/protocol.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace noteapp;
using namespace noteapp::pdftest;
using pdfview::PdfDocument;
using pdfview::PdfEngine;
namespace fs = std::filesystem;

namespace {

const fs::path kNote = NOTEAPP_NOTE_EXE;
const fs::path kFakeWorker = NOTEAPP_FAKE_PDF_WORKER;
// Öldürmeyle kesilen işin dönüş sınırı (süre + 1 sn; Wine ve sanitizer altında da).
constexpr auto kKillSlack = std::chrono::seconds(1);
// Süre her çağrıda işçinin (yeniden) başlatılmasını ve Open'ı da kapsar: yüklü makinede de bunlara
// yetecek kadar uzun, fixture'ların doğal süresinden (dakikalar; RED'de 120 sn zaman aşımı) çok kısa.
constexpr auto kShortDuration = std::chrono::seconds(1);
constexpr std::string_view kShortDurationText = "1s";
// Durdurma isteğinden dönüşe: 50 ms yoklama + öldürme; öldürme sürecin bellek alanı sökülene dek bekler.
constexpr auto kStopBound = std::chrono::milliseconds(500);

core::PdfLimits with_duration(core::PdfLimits limits = unlimited()) {
    const auto milliseconds = std::chrono::milliseconds(kShortDuration).count();
    limits.duration = core::Limit{static_cast<std::uint64_t>(milliseconds), std::string(kShortDurationText)};
    return limits;
}

PdfDocument open_ok(std::string_view bytes, const core::PdfLimits& limits = unlimited(),
                    const fs::path& worker = kNote) {
    auto opened = PdfDocument::open(bytes, limits, worker);
    if (!opened)
        FAIL(core::format_error(opened.error()));
    return std::move(*opened);
}

// Başarı beklenmeyen sonucun hatası (başarıda test durur: .error() tanımsız olurdu).
template <class T>
core::Error error_of(const std::expected<T, core::Error>& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

void check_same_error(const core::Error& from_worker, const core::Error& in_process) {
    CHECK(from_worker.code == in_process.code);
    CHECK(from_worker.message_key == in_process.message_key);
    CHECK(from_worker.args == in_process.args);
}

void check_too_slow(const core::Error& error) {
    CHECK(error.code == core::Error::Code::LimitExceeded);
    CHECK(error.message_key == "error.pdf_too_slow");
    CHECK(has_arg(error, "limit", kShortDurationText));
}

using Clock = std::chrono::steady_clock;

#ifndef _WIN32
// Bu sürecin toplanmamış (zombi) çocukları.
int zombie_children() {
    int zombies = 0;
    const std::string own = std::to_string(getpid());
    for (const auto& entry : fs::directory_iterator("/proc")) {
        std::ifstream stat(entry.path() / "stat");
        std::string line;
        if (!std::getline(stat, line))
            continue;
        // "pid (comm) state ppid ...": comm boşluk ve parantez içerebilir; son ')' sonrasını oku.
        const std::size_t close = line.rfind(')');
        if (close == std::string::npos || close + 4 > line.size())
            continue;
        const char state = line[close + 2];
        const std::string rest = line.substr(close + 4);
        const std::string ppid = rest.substr(0, rest.find(' '));
        if (ppid == own && state == 'Z')
            ++zombies;
    }
    return zombies;
}
#endif

std::string read_log() {
    std::ifstream in(core::state_dir() / "log.txt", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("the worker returns byte-identical text and RGBA to the in-process engine") {
    for (const std::string& bytes : {ascii_pdf(2), exported_pdf("Merhaba çğıİöşü ÇĞIÖŞÜ"), jpx_pdf()}) {
        PdfDocument document = open_ok(bytes);
        auto engine = PdfEngine::open(bytes, unlimited());
        REQUIRE(engine);
        REQUIRE(document.page_count() == engine->page_count());
        for (int page = 0; page < document.page_count(); ++page) {
            CAPTURE(page);
            const auto text = document.page_text(page);
            const auto expected_text = engine->page_text(page);
            REQUIRE(text);
            REQUIRE(expected_text);
            CHECK(*text == *expected_text);
            const auto structure = document.page_structure(page);
            const auto expected_structure = engine->page_structure(page);
            REQUIRE(structure);
            REQUIRE(expected_structure);
            CHECK(*structure == *expected_structure);
            // RGBA her bayt değerini taşır (0x0A, 0x0D, 0x1A dahil): Windows'ta ikili kip boru gidiş-dönüşü.
            const auto image = document.render_page(page, 1.5F);
            const auto expected_image = engine->render_page(page, 1.5F);
            REQUIRE(image);
            REQUIRE(expected_image);
            CHECK(image->width == expected_image->width);
            CHECK(image->height == expected_image->height);
            CHECK(image->rgba == expected_image->rgba);
        }
    }
}

TEST_CASE("every engine failure reaches the client as the same error key and arguments") {
    const std::string three = ascii_pdf(3);
    SUBCASE("not a PDF") {
        check_same_error(error_of(PdfDocument::open("hello", unlimited(), kNote)),
                         error_of(PdfEngine::open("hello", unlimited())));
    }
    SUBCASE("encrypted") {
        const std::string bytes = ascii_pdf(1, {.owner = "o", .user = "u"});
        check_same_error(error_of(PdfDocument::open(bytes, unlimited(), kNote)),
                         error_of(PdfEngine::open(bytes, unlimited())));
    }
    SUBCASE("too many pages") {
        core::PdfLimits limits = unlimited();
        limits.pages = limit_of(2);
        check_same_error(error_of(PdfDocument::open(three, limits, kNote)), error_of(PdfEngine::open(three, limits)));
    }
    SUBCASE("context memory at open") {
        core::PdfLimits limits = unlimited();
        limits.context_memory = core::Limit{64U * 1024U, "64KiB"};
        check_same_error(error_of(PdfDocument::open(three, limits, kNote)), error_of(PdfEngine::open(three, limits)));
    }
    SUBCASE("render too large, text too large") {
        core::PdfLimits limits = unlimited();
        limits.render_pixels = limit_of(100);
        limits.text_bytes = limit_of(2);
        PdfDocument document = open_ok(three, limits);
        auto engine = PdfEngine::open(three, limits);
        REQUIRE(engine);
        check_same_error(error_of(document.render_page(1, 1.0F)), error_of(engine->render_page(1, 1.0F)));
        check_same_error(error_of(document.page_text(2)), error_of(engine->page_text(2)));
    }
    SUBCASE("out of memory during a render (budget)") {
        core::PdfLimits limits = unlimited();
        limits.context_memory = core::Limit{8U * 1024U * 1024U, "8MiB"};
        const std::string bytes = big_image_pdf();
        PdfDocument document = open_ok(bytes, limits);
        auto engine = PdfEngine::open(bytes, limits);
        REQUIRE(engine);
        const core::Error error = error_of(document.render_page(0, 1.0F));
        CHECK(error.message_key == "error.pdf_out_of_memory");
        check_same_error(error, error_of(engine->render_page(0, 1.0F)));
        CHECK(document.page_text(1).has_value());  // iş hatasında işçi canlı kalır
    }
    SUBCASE("a broken page") {
        // /Count 2 ama tek sayfa: ikinci sayfa yüklenemez (MuPDF FZ_ERROR_FORMAT).
        std::string bytes = ascii_pdf(1);
        const std::size_t count = bytes.find("/Count 1");
        REQUIRE(count != std::string::npos);
        bytes.replace(count, 8, "/Count 2");
        PdfDocument document = open_ok(bytes);
        auto engine = PdfEngine::open(bytes, unlimited());
        REQUIRE(engine);
        REQUIRE(document.page_count() == 2);
        check_same_error(error_of(document.page_text(1)), error_of(engine->page_text(1)));
        CHECK(document.page_text(0).has_value());
    }
}

TEST_CASE("a page that never finishes is killed at the duration and the next call respawns the worker") {
    PdfDocument document = open_ok(fan_out_pdf(true), with_duration());
    const auto started = Clock::now();
    const auto slow = document.render_page(0, 1.0F);
    CHECK(Clock::now() - started < kShortDuration + kKillSlack);
    REQUIRE_FALSE(slow);
    check_too_slow(slow.error());
#ifndef _WIN32
    CHECK(zombie_children() == 0);  // öldürülen işçi toplandı
#endif
    const auto text = document.page_text(1);  // yeni işçi, aynı baytlar
    REQUIRE(text);
    CHECK(contains(*text, "calm"));
    const auto slow_text = document.page_text(0);
    REQUIRE_FALSE(slow_text);
    check_too_slow(slow_text.error());
}

TEST_CASE("opening a file whose repair spins is bounded by the duration and by a stop request") {
    const std::string bytes = repair_spin_pdf();
    SUBCASE("deadline") {
        const auto started = Clock::now();
        const auto opened = PdfDocument::open(bytes, with_duration(), kNote);
        CHECK(Clock::now() - started < kShortDuration + kKillSlack);
        REQUIRE_FALSE(opened);
        check_too_slow(opened.error());
    }
    SUBCASE("stop") {
        std::stop_source source;
        std::jthread stopper([&source] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            source.request_stop();
        });
        const auto started = Clock::now();
        const auto opened = PdfDocument::open(bytes, unlimited(), kNote, source.get_token());
        CHECK(Clock::now() - started < std::chrono::milliseconds(300) + kKillSlack);
        REQUIRE_FALSE(opened);
        CHECK(opened.error().code == core::Error::Code::Cancelled);
    }
}

TEST_CASE("loading a page whose links trigger the slow page lookup is bounded by the kill") {
    const std::string bytes = link_lookup_pdf();
    for (const bool render : {true, false}) {
        CAPTURE(render);
        // Her çağrı açılmış, canlı bir işçiyle başlar: süre yalnız yavaş sayfa yüklemesine harcanır.
        PdfDocument document = open_ok(bytes, with_duration());
        REQUIRE(document.page_count() == 1);
        const std::size_t log_before = read_log().size();
        const auto started = Clock::now();
        const core::Error error = render ? error_of(document.render_page(0, 1.0F)) : error_of(document.page_text(0));
        const auto elapsed = Clock::now() - started;
        CHECK(elapsed >= kShortDuration);  // hızlı bir hata değil: süre doldu
        CHECK(elapsed < kShortDuration + kKillSlack);
        check_too_slow(error);
        // Süre dolunca işçi öldürüldü (çıkış kodu yok: kendiliğinden bitmedi) ve toplandı.
        const std::string operation = render ? "render_page" : "page_text";
        const std::string event =
            "event=pdf_worker_failed operation=" + operation + " page=1 exit_code=- reason=deadline";
        CHECK(contains(read_log().substr(log_before), event));
#ifndef _WIN32
        CHECK(zombie_children() == 0);
#endif
    }
}

TEST_CASE("a stop request during a render cancels promptly") {
    PdfDocument document = open_ok(fan_out_pdf());
    std::stop_source source;
    Clock::time_point stopped_at{};
    std::jthread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stopped_at = Clock::now();
        source.request_stop();
    });
    const auto rendered = document.render_page(0, 1.0F, source.get_token());
    const auto returned_at = Clock::now();
    stopper.join();
    REQUIRE_FALSE(rendered);
    CHECK(rendered.error().code == core::Error::Code::Cancelled);
    CHECK(rendered.error().message_key == "error.pdf_cancelled");
    CHECK(returned_at - stopped_at <= kStopBound);
}

TEST_CASE("a duration too long to add to the clock counts as unlimited") {
    core::PdfLimits limits = unlimited();
    limits.duration = core::Limit{static_cast<std::uint64_t>(INT64_MAX), "huge"};
    PdfDocument document = open_ok(ascii_pdf(1), limits);
    CHECK(document.page_text(0).has_value());
}

TEST_CASE("a duration that never fires does not delay a normal page") {
    core::PdfLimits limits = unlimited();
    limits.duration = core::Limit{10000, "10s"};
    const auto started = Clock::now();
    PdfDocument document = open_ok(ascii_pdf(1), limits);
    CHECK(document.page_text(0).has_value());
    CHECK(document.render_page(0, 1.0F).has_value());
    CHECK(Clock::now() - started < std::chrono::seconds(2));
}

TEST_CASE("a misbehaving worker gives a clean error, is reaped and never reaches the log raw") {
    test::ScopedEnv mode("NOTEAPP_FAKE_PDF_WORKER_MODE");
    const auto open_fake = [&](std::string_view name, const core::PdfLimits& limits = unlimited()) {
        mode.set(std::string(name));
        return PdfDocument::open(ascii_pdf(1), limits, kFakeWorker);
    };
    for (const std::string_view name : {"exit-mid", "crash", "oversize", "wrong-kind", "escape-detail"}) {
        CAPTURE(name);
        const auto opened = open_fake(name);
        REQUIRE_FALSE(opened);
        CHECK(opened.error().code == core::Error::Code::InvalidDocument);
        CHECK(opened.error().message_key == "error.pdf_invalid");
        CHECK(opened.error().args.empty());
    }
    SUBCASE("escape sequences in the worker's detail are neutralised in the log") {
        (void)open_fake("escape-detail");
        const std::string log = read_log();
        CHECK(contains(log, "event=pdf_worker_error"));
        CHECK_FALSE(contains(log, "\x1B"));
        CHECK_FALSE(contains(log, "\x07"));
        CHECK_FALSE(contains(log, "\nforged=1"));
    }
    SUBCASE("a worker that never answers is killed at the duration") {
        const auto started = Clock::now();
        const auto opened = open_fake("sleep", with_duration());
        CHECK(Clock::now() - started < kShortDuration + kKillSlack);
        REQUIRE_FALSE(opened);
        check_too_slow(opened.error());
    }
    SUBCASE("an RGBA length that is not 8 + w*h*4 is a broken frame") {
        auto opened = open_fake("bad-rgba");
        REQUIRE(opened);
        const auto rendered = opened->render_page(0, 1.0F);
        REQUIRE_FALSE(rendered);
        CHECK(rendered.error().message_key == "error.pdf_page_invalid");
    }
    SUBCASE("a huge RGBA with render_pixels 0 fails as no memory, without reading it") {
        auto opened = open_fake("huge-render");
        REQUIRE(opened);
        const auto rendered = opened->render_page(0, 1.0F);
        REQUIRE_FALSE(rendered);
        CHECK(rendered.error().code == core::Error::Code::LimitExceeded);
        CHECK(rendered.error().message_key == "error.pdf_no_memory");
    }
    SUBCASE("a page count over the pages limit is a broken frame") {
        core::PdfLimits limits = unlimited();
        limits.pages = limit_of(2);
        const auto opened = open_fake("too-many-pages", limits);
        REQUIRE_FALSE(opened);
        CHECK(opened.error().message_key == "error.pdf_invalid");
    }
    SUBCASE("text longer than text_bytes is a broken frame, not a text-too-large error") {
        core::PdfLimits limits = unlimited();
        limits.text_bytes = limit_of(4);
        auto opened = open_fake("text-over-cap", limits);
        REQUIRE(opened);
        const auto text = opened->page_text(0);
        REQUIRE_FALSE(text);
        CHECK(text.error().message_key == "error.pdf_page_invalid");
    }
    SUBCASE("a render over render_pixels is a broken frame") {
        core::PdfLimits limits = unlimited();
        limits.render_pixels = limit_of(100);
        auto opened = open_fake("render-over-cap", limits);
        REQUIRE(opened);
        const auto rendered = opened->render_page(0, 1.0F);
        REQUIRE_FALSE(rendered);
        CHECK(rendered.error().message_key == "error.pdf_page_invalid");
    }
    SUBCASE("a render wider than INT_MAX is a broken frame") {
        core::PdfLimits limits = unlimited();
        limits.render_pixels = limit_of(1000);
        auto opened = open_fake("wide-render", limits);
        REQUIRE(opened);
        const auto rendered = opened->render_page(0, 1.0F);
        REQUIRE_FALSE(rendered);
        CHECK(rendered.error().message_key == "error.pdf_page_invalid");
    }
    SUBCASE("the client cleans the worker's text again") {
        core::PdfLimits limits = unlimited();
        limits.text_bytes = limit_of(1000);
        auto opened = open_fake("dirty-text", limits);
        REQUIRE(opened);
        const auto text = opened->page_text(0);
        REQUIRE(text);
        CHECK_FALSE(contains(*text, "\x1B"));
        CHECK_FALSE(contains(*text, "\xC2\x9B"));
        CHECK_FALSE(contains(*text, "\xFF"));
        CHECK(contains(*text, "\xEF\xBF\xBD"));  // U+FFFD
        CHECK(text->starts_with("a"));
        CHECK(text->ends_with("d"));
    }
    SUBCASE("a hostile structured page is a broken frame and the worker is replaced") {
        for (const std::string_view name :
             {"structured-huge-count", "structured-nan", "structured-off-page", "structured-bad-utf8"}) {
            CAPTURE(name);
            auto opened = open_fake(name);
            REQUIRE(opened);
            const auto text = opened->page_structure(0);
            CHECK_FALSE(text);
            if (text)
                continue;  // her kip ayrı raporlansın
            CHECK(text.error().code == core::Error::Code::InvalidDocument);
            CHECK(text.error().message_key == "error.pdf_page_invalid");
            CHECK(contains(read_log(), "operation=page_structure page=1 exit_code=- reason=broken"));
        }
    }
    SUBCASE("a structured page over text_bytes is a broken frame") {
        core::PdfLimits limits = unlimited();
        limits.text_bytes = limit_of(4);
        auto opened = open_fake("structured-over-cap", limits);
        REQUIRE(opened);
        const auto text = opened->page_structure(0);
        REQUIRE_FALSE(text);
        CHECK(text.error().message_key == "error.pdf_page_invalid");
    }
    SUBCASE("a structured frame longer than its text_bytes-derived cap is refused before any payload arrives") {
        // İşçi yalnız başlığı gönderip bekler: sınır denetimi olmasa istemci süre dolana dek yükü beklerdi.
        core::PdfLimits limits = with_duration();
        limits.text_bytes = limit_of(4);
        auto opened = open_fake("structured-over-frame", limits);
        REQUIRE(opened);
        const auto started = Clock::now();
        const auto text = opened->page_structure(0);
        CHECK(Clock::now() - started < kShortDuration);
        REQUIRE_FALSE(text);
        CHECK(text.error().message_key == "error.pdf_page_invalid");
    }
    SUBCASE("the client cleans control characters in a structured page, keeping one box per character") {
        auto opened = open_fake("structured-dirty");
        REQUIRE(opened);
        const auto page = opened->page_structure(0);
        REQUIRE(page);
        REQUIRE(page->blocks.size() == 1);
        const pdfview::TextSpan& span = page->blocks[0].lines[0].spans[0];
        CHECK(span.text == "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c");
        CHECK(span.edges.size() == 10);
    }
    SUBCASE("a respawned worker reporting another page count makes the document invalid") {
        auto opened = open_fake("die-after-2");
        REQUIRE(opened);
        REQUIRE(opened->page_count() == 2);
        const auto crashed = opened->page_text(0);
        REQUIRE_FALSE(crashed);
        CHECK(crashed.error().message_key == "error.pdf_page_invalid");
        mode.set("die-after-3");
        const auto changed = opened->page_text(0);
        REQUIRE_FALSE(changed);
        CHECK(changed.error().code == core::Error::Code::InvalidDocument);
        CHECK(changed.error().message_key == "error.pdf_invalid");
    }
#ifndef _WIN32
    CHECK(zombie_children() == 0);
#endif
}

TEST_CASE("a missing worker binary is reported as program not found") {
    const auto opened = PdfDocument::open(ascii_pdf(1), unlimited(), kNote.parent_path() / "no-such-note-worker");
    REQUIRE_FALSE(opened);
    CHECK(opened.error().message_key == "error.process_not_found");
}

TEST_CASE("the worker rejects a request whose payload ends right after its header") {
    // Open, sayfa 0 için Text, sonra 4 baytlık yük bildiren Text başlığı ve hemen EOF: eksik yük önceki
    // isteğin artığıyla doldurulup (sayfa 0) cevaplanmamalı.
    namespace proto = pdfview::protocol;
    const std::string pdf = ascii_pdf(1);
    auto worker = core::ChildProcess::start({kNote.string(), "__pdf-worker"}, true);
    REQUIRE(worker);
    const auto deadline = core::ChildProcess::Clock::now() + std::chrono::seconds(30);
    const std::string request = proto::encode_open_head({}, pdf.size()) + pdf + proto::encode_text_request(0) +
                                proto::encode_header(proto::Kind::Text, 4);
    REQUIRE(worker->write_all(request, deadline));
    worker->close_stdin();
    // 3 = kBadRequest; düzeltmeden önce ikinci Text'e de cevap verip stdin EOF'unda 0 ile çıkıyordu.
    CHECK(worker->wait_until(deadline) == 3);
}
