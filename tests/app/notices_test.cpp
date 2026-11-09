// NY-G: işlem bildirimleri (toast) iki arayüzün ortak modeli. Süre `tuning.notice_ms`; hata kalıcıdır.
#include "app/notices.hpp"

#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>

using noteapp::app::NoticeKind;
using noteapp::app::NoticeQueue;
using Clock = NoticeQueue::Clock;
using std::chrono::milliseconds;

namespace {

constexpr milliseconds kLife{4000};

}  // namespace

TEST_CASE("notices: info and success expire after notice_ms, an error stays until dismissed") {
    NoticeQueue queue(kLife);
    const Clock::time_point start{};
    queue.post(NoticeKind::Success, "PDF yazıldı", start);
    queue.post(NoticeKind::Error, "senkron başarısız", start + milliseconds(10));
    REQUIRE(queue.items().size() == 2);
    CHECK(queue.next_deadline() == start + kLife);

    CHECK_FALSE(queue.expire(start + kLife - milliseconds(1)));
    CHECK(queue.expire(start + kLife));
    REQUIRE(queue.items().size() == 1);
    CHECK(queue.items().front().kind == NoticeKind::Error);
    CHECK_FALSE(queue.next_deadline().has_value());  // kalan hata için uyanma yok: boşta kare üretilmez

    queue.dismiss(queue.items().front().id);
    CHECK(queue.items().empty());

    queue.post(NoticeKind::Error, "a", start);
    queue.post(NoticeKind::Info, "b", start);
    queue.dismiss_all(NoticeKind::Error);  // TUI: sonraki tuş hataları kapatır
    REQUIRE(queue.items().size() == 1);
    CHECK(queue.items().front().kind == NoticeKind::Info);
}

TEST_CASE("notices: the same text again refreshes the notice and counts it instead of stacking") {
    NoticeQueue queue(kLife);
    const Clock::time_point start{};
    queue.post(NoticeKind::Info, "Senkron tamam", start);
    queue.post(NoticeKind::Info, "Senkron tamam", start + milliseconds(1000));
    REQUIRE(queue.items().size() == 1);
    CHECK(queue.items().front().repeats == 2);
    CHECK(queue.next_deadline() == start + milliseconds(1000) + kLife);
    queue.post(NoticeKind::Error, "Senkron tamam", start + milliseconds(1500));  // tür farklı: ayrı bildirim
    CHECK(queue.items().size() == 2);
}

TEST_CASE("notices: an export carries the folder to open and notice_ms 0 keeps every notice") {
    NoticeQueue queue(milliseconds(0));
    const Clock::time_point start{};
    queue.post(NoticeKind::Success, "PDF yazıldı: /tmp/a.pdf", start, "/tmp");
    REQUIRE(queue.items().size() == 1);
    CHECK(queue.items().front().folder == "/tmp");
    CHECK_FALSE(queue.next_deadline().has_value());
    CHECK_FALSE(queue.expire(start + std::chrono::hours(24)));
    CHECK(queue.items().size() == 1);
}

TEST_CASE("notices: dismissing an unknown id changes nothing; ids are never reused") {
    NoticeQueue queue(kLife);
    const Clock::time_point start{};
    queue.post(NoticeKind::Info, "a", start);
    const auto first = queue.items().front().id;
    queue.dismiss(first + 100);
    CHECK(queue.items().size() == 1);
    queue.dismiss(first);
    queue.post(NoticeKind::Info, "a", start);
    CHECK(queue.items().front().id != first);
}

// İnceleme bulgusu: çok büyük notice_ms (config'te tavan yok) süre toplamını taşırmaz; temsil edilemeyen süre
// "kendiliğinden kapanmaz" demektir.
TEST_CASE("notices: a lifetime beyond the clock's range never expires and never overflows") {
    CHECK(NoticeQueue::lifetime_of(0) == milliseconds(0));
    CHECK(NoticeQueue::lifetime_of(4000) == milliseconds(4000));
    CHECK(NoticeQueue::lifetime_of(UINT64_MAX) == milliseconds::max());
    NoticeQueue queue(NoticeQueue::lifetime_of(UINT64_MAX));
    const Clock::time_point start{};
    queue.post(NoticeKind::Info, "a", start);
    CHECK_FALSE(queue.next_deadline().has_value());
    CHECK_FALSE(queue.expire(Clock::time_point::max()));
    CHECK(queue.items().size() == 1);

    NoticeQueue late(std::chrono::hours(2));  // makul süre, saatin sonuna yakın anda
    late.post(NoticeKind::Info, "b", Clock::time_point::max() - std::chrono::hours(1));
    CHECK_FALSE(late.next_deadline().has_value());
    CHECK_FALSE(late.expire(Clock::time_point::max()));
}

TEST_CASE("notices: an update notice carries the version to install; other notices carry none") {
    NoticeQueue queue(kLife);
    const Clock::time_point start{};
    queue.post(NoticeKind::Info, "senkron bitti", start);
    queue.post_update("noteapp 1.3.0 hazır", "1.3.0", start);
    REQUIRE(queue.items().size() == 2);
    CHECK_FALSE(queue.items().front().update_version.has_value());
    const auto& update = queue.items().back();
    CHECK(update.kind == NoticeKind::Info);
    CHECK(update.update_version == "1.3.0");
    CHECK_FALSE(update.folder.has_value());

    queue.post_update("noteapp 1.3.0 hazır", "1.3.0", start + milliseconds(5));  // aynı metin: yığılmaz
    REQUIRE(queue.items().size() == 2);
    CHECK(queue.items().back().repeats == 2);
    CHECK(queue.items().back().update_version == "1.3.0");
}

TEST_CASE("notices: an update notice stays until dismissed; other info notices still expire") {
    NoticeQueue queue(kLife);
    const Clock::time_point start{};
    queue.post_update("noteapp 1.3.0 hazır", "1.3.0", start);
    queue.post(NoticeKind::Info, "senkron bitti", start);
    CHECK(queue.next_deadline() == start + kLife);  // yalnız sıradan bilgi düşer

    CHECK(queue.expire(start + kLife));
    REQUIRE(queue.items().size() == 1);
    CHECK(queue.items().front().update_version == "1.3.0");
    CHECK_FALSE(queue.next_deadline().has_value());
    CHECK_FALSE(queue.expire(start + std::chrono::hours(48)));
    CHECK(queue.items().size() == 1);
}

TEST_CASE("notices: acting on the update removes only the update notice") {
    NoticeQueue queue(kLife);
    const Clock::time_point start{};
    queue.post(NoticeKind::Error, "senkron başarısız", start);
    queue.post_update("noteapp 1.3.0 hazır", "1.3.0", start);
    queue.post(NoticeKind::Info, "senkron bitti", start);
    queue.dismiss_update();
    REQUIRE(queue.items().size() == 2);
    for (const auto& notice : queue.items())
        CHECK_FALSE(notice.update_version.has_value());
}
