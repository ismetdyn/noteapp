// app::PdfPreview: gerçek `note __pdf-worker` ve sahte işçiyle (tests/support/fake_pdf_worker.cpp) kuyruk, düşürme,
// önbellek sınırı, süre dolan sayfanın hatırlanması, duyuru ve kapanış.
#include "app/pdf_preview.hpp"
#include "app/pdf_text.hpp"

#include "core/i18n.hpp"
#include "support/fake_pdf_worker.hpp"
#include "support/test_support.hpp"
#include "support/text_pdf.hpp"

#include <chrono>
#include <condition_variable>
#include <doctest/doctest.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace {

using noteapp::app::PdfPreview;
using noteapp::app::PdfPreviewOptions;
using noteapp::core::Limit;
using Clock = std::chrono::steady_clock;

constexpr auto kWait = std::chrono::seconds(20);  // sanitizer'lı derlemede işçi açılışı yavaş

// on_change işçi iş parçacığından gelir: sayılır, bekleyen uyanır.
struct Changes {
    std::mutex mutex;
    std::condition_variable changed;
    int count = 0;

    std::function<void()> callback() {
        return [this] {
            {
                const std::scoped_lock lock(mutex);
                ++count;
            }
            changed.notify_all();
        };
    }
    int seen() {
        const std::scoped_lock lock(mutex);
        return count;
    }
    // `done` doğru olana dek (her duyuruda yeniden bakılır) en çok kWait. Sayaç kilit altında okunur: done()
    // bakılırken gelen duyuru kaçmaz.
    bool wait_for(const std::function<bool()>& done) {
        const auto deadline = Clock::now() + kWait;
        std::unique_lock lock(mutex);
        while (true) {
            const int before = count;
            lock.unlock();
            if (done())
                return true;
            lock.lock();
            if (!changed.wait_until(lock, deadline, [&] { return count != before; }))
                return false;
        }
    }
};

// Sayfanın yapılı metninin düz hali (seçim metni kuralı).
std::string plain(const PdfPreview::Text& text) {
    const noteapp::app::PdfTextLayer layer(text.value());
    return layer.text(0, layer.size());
}

PdfPreviewOptions options(Changes& changes, const std::filesystem::path& worker = NOTEAPP_NOTE_EXE) {
    PdfPreviewOptions result;
    result.worker = worker;
    result.on_change = changes.callback();
    return result;
}

}  // namespace

TEST_CASE("pdf preview opens in the background, serves text requests in order and posts each result") {
    noteapp::core::init_i18n("en");
    Changes changes;
    PdfPreview preview(noteapp::test::text_pdf({"Alpha", "Beta", "Gamma"}), options(changes));
    preview.request_text(2);
    preview.request_text(0);
    preview.request_text(0);  // yinelenen istek kuyruğa girmez
    REQUIRE(changes.wait_for([&] { return changes.seen() >= 3; }));  // açılış + iki sonuç
    CHECK(preview.state() == PdfPreview::State::Ready);
    CHECK(preview.page_count() == 3);
    REQUIRE(preview.text(2)->has_value());
    CHECK(plain(*preview.text(2)) == "Gamma");
    CHECK(plain(*preview.text(0)) == "Alpha");
    CHECK_FALSE(preview.text(1).has_value());  // istenmeyen sayfa okunmaz
    CHECK(preview.pending() == 0);
    CHECK(changes.seen() == 3);
}

TEST_CASE("pdf preview drops queued requests and closes promptly while the worker is busy") {
    // Sahte işçi açılışa hiç cevap vermez: istekler kuyrukta kalır.
    const noteapp::test::FakePdfWorker fake(NOTEAPP_FAKE_PDF_WORKER);
    fake.set_mode("sleep");
    Changes changes;
    auto preview = std::make_unique<PdfPreview>(noteapp::test::text_pdf({"x"}), options(changes, fake.path()));
    preview->request_text(0);
    preview->request_render(0, 1.0F);
    preview->request_render(0, 1.0F);
    preview->request_render(0, 2.0F);
    CHECK(preview->pending() == 3);
    preview->drop_pending();
    CHECK(preview->pending() == 0);
    CHECK(preview->state() == PdfPreview::State::Opening);
    const auto started = Clock::now();
    preview.reset();  // yürüyen açılış kesilir, işçi öldürülür
    CHECK(Clock::now() - started < std::chrono::seconds(2));
    CHECK(changes.seen() == 0);  // iptal edilen açılış duyurulmaz
}

TEST_CASE("pdf preview keeps at most cache_pages rendered pages and evicts the least recently used") {
    Changes changes;
    PdfPreviewOptions settings = options(changes);
    settings.limits.cache_pages = Limit{2, "2"};
    PdfPreview preview(noteapp::test::text_pdf({"one", "two", "three"}), settings);
    preview.request_render(0, 1.0F);
    preview.request_render(1, 1.0F);
    REQUIRE(changes.wait_for([&] { return preview.render(1, 1.0F).has_value(); }));
    REQUIRE(preview.render(0, 1.0F).has_value());  // 0 en son kullanılan olur
    const auto page = *preview.render(0, 1.0F);
    REQUIRE(page.has_value());
    CHECK((*page)->width == 200);
    CHECK((*page)->height == 100);
    CHECK((*page)->rgba.size() == 200U * 100U * 4U);
    preview.request_render(2, 1.0F);
    REQUIRE(changes.wait_for([&] { return preview.render(2, 1.0F).has_value(); }));
    CHECK(preview.render(0, 1.0F).has_value());
    CHECK_FALSE(preview.render(1, 1.0F).has_value());  // düşen sayfa
    CHECK_FALSE(preview.render(2, 2.0F).has_value());  // başka ölçek ayrı sayfadır
}

TEST_CASE("pdf preview remembers a page that timed out and does not request it again") {
    Changes changes;
    PdfPreviewOptions settings = options(changes);
    // Süre açılışa, yeniden başlatmaya ve sayfa 1'e de uyar: yavaş makinede (sanitizer) onlara bol pay kalsın. Sayfa
    // 0 saatler sürer, yine kesin dolar.
    settings.limits.duration = Limit{3000, "3s"};
    PdfPreview preview(noteapp::test::slow_first_page_pdf("calm"), settings);
    preview.request_render(0, 1.0F);
    preview.request_text(1);  // yavaş sayfa bitmeden kuyruktadır, ardından yeni işçide okunur
    REQUIRE(changes.wait_for([&] { return preview.text(1).has_value(); }));
    const auto slow = preview.render(0, 1.0F);
    REQUIRE(slow.has_value());
    REQUIRE_FALSE(slow->has_value());
    CHECK(slow->error().message_key == "error.pdf_too_slow");
    CHECK(plain(*preview.text(1)) == "calm");
    const auto version = preview.version();
    preview.request_render(0, 1.0F);  // hatırlanır: kuyruğa girmez
    CHECK(preview.pending() == 0);
    CHECK(preview.version() == version);
}

TEST_CASE("pdf preview reports an invalid document and ignores later requests") {
    Changes changes;
    PdfPreview preview("not a pdf", options(changes));
    REQUIRE(changes.wait_for([&] { return preview.state() != PdfPreview::State::Opening; }));
    CHECK(preview.state() == PdfPreview::State::Failed);
    REQUIRE(preview.open_error().has_value());
    CHECK(preview.open_error()->message_key == "error.pdf_invalid");
    preview.request_text(0);
    CHECK(preview.pending() == 0);
}

TEST_CASE("pdf page separator is localized and one-based") {
    noteapp::core::init_i18n("en");
    CHECK(noteapp::app::pdf_page_separator(2, 12) == "--- page 3/12 ---");
    noteapp::core::init_i18n("tr");
    CHECK(noteapp::app::pdf_page_separator(0, 1) == "--- sayfa 1/1 ---");
    noteapp::core::init_i18n("en");
    CHECK(noteapp::app::is_pdf_type("PDF"));
    CHECK_FALSE(noteapp::app::is_pdf_type("md"));
}
