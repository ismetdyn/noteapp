#include "app/sync_indicator.hpp"
#include "core/i18n.hpp"

#include <chrono>
#include <ctime>
#include <doctest/doctest.h>
#include <string>

using namespace noteapp;
using app::SyncIndicator;

namespace {

// Yerel saatle 09:05; dilimden bağımsız beklenti "09:05".
std::chrono::system_clock::time_point at_0905() {
    std::tm local{};
    local.tm_year = 126;
    local.tm_mon = 3;
    local.tm_mday = 14;
    local.tm_hour = 9;
    local.tm_min = 5;
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

core::Error failure(const char* key = "sync.timeout") {
    return {core::Error::Code::Io, key, {}};
}

}  // namespace

TEST_CASE("a new sync indicator is disabled") {
    const SyncIndicator indicator;
    CHECK(indicator.state() == SyncIndicator::State::Disabled);
    CHECK(indicator.success_time().empty());
    CHECK(indicator.error_text().empty());
}

TEST_CASE("a last success is Ok with the local HH:MM") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({at_0905(), std::nullopt});
    CHECK(indicator.state() == SyncIndicator::State::Ok);
    CHECK(indicator.success_time() == "09:05");
    CHECK(indicator.error_text().empty());
}

TEST_CASE("a running sync is Running, even over an old error") {
    core::init_i18n("en");
    SyncIndicator indicator;
    CHECK_FALSE(indicator.update({at_0905(), failure()}, true).has_value());
    CHECK(indicator.state() == SyncIndicator::State::Running);
    CHECK(indicator.error_text().empty());
}

TEST_CASE("an error is Failed with the full translated text") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({at_0905(), failure()});
    CHECK(indicator.state() == SyncIndicator::State::Failed);
    CHECK(indicator.error_text() == core::format_error(failure()));
    CHECK(indicator.success_time().empty());
}

TEST_CASE("a stopped device log after a success still shows as an error") {
    core::init_i18n("en");
    SyncIndicator indicator;
    sync::SyncStatus status{at_0905(), std::nullopt};
    status.log_issues.push_back(failure("sync.not_initialized"));
    indicator.restore(status);
    CHECK(indicator.state() == SyncIndicator::State::Failed);
    CHECK(indicator.error_text() == core::format_error(failure("sync.not_initialized")));
}

TEST_CASE("neither success nor error yet means the first sync is about to start") {
    SyncIndicator indicator;
    indicator.restore({});
    CHECK(indicator.state() == SyncIndicator::State::Running);
}

TEST_CASE("disable() returns to Disabled from any state") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({at_0905(), std::nullopt});
    indicator.disable();
    CHECK(indicator.state() == SyncIndicator::State::Disabled);
    CHECK(indicator.success_time().empty());
}

TEST_CASE("the opening status is shown without a notice") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({std::nullopt, failure()});
    CHECK(indicator.state() == SyncIndicator::State::Failed);
    // Açılışta gösterilen hata bildirilmemişti: ilk senkron aynı hatayla düşerse bildirilir.
    CHECK_FALSE(indicator.update({}, true).has_value());
    const auto notice = indicator.update({std::nullopt, failure()}, false);
    REQUIRE(notice.has_value());
    CHECK(notice->kind == app::NoticeKind::Error);
    CHECK(notice->text == core::format_error(failure()));
}

TEST_CASE("a run that ends Failed notifies the error once, until it changes or sync recovers") {
    core::init_i18n("en");
    SyncIndicator indicator;
    // AutoSync gibi: her tur Running ile açılır, sonuçla kapanır.
    const auto run = [&](const sync::SyncStatus& result) {
        CHECK_FALSE(indicator.update({}, true).has_value());
        return indicator.update(result, false);
    };
    REQUIRE(run({std::nullopt, failure()}).has_value());
    CHECK_FALSE(run({std::nullopt, failure()}).has_value());
    CHECK_FALSE(indicator.update({std::nullopt, failure()}, false).has_value());  // Failed -> Failed
    const auto changed = run({std::nullopt, failure("sync.not_initialized")});
    REQUIRE(changed.has_value());
    CHECK(changed->kind == app::NoticeKind::Error);
    CHECK(changed->text == core::format_error(failure("sync.not_initialized")));
    REQUIRE(run({at_0905(), std::nullopt}).has_value());  // başarı: "bitti", hata unutulur
    CHECK(run({std::nullopt, failure("sync.not_initialized")}).has_value());
}

TEST_CASE("a result outside a run (no Running before it) notifies nothing") {
    core::init_i18n("en");
    SyncIndicator indicator;
    CHECK_FALSE(indicator.update({std::nullopt, failure()}, false).has_value());
    CHECK(indicator.state() == SyncIndicator::State::Failed);
    CHECK_FALSE(indicator.update({at_0905(), std::nullopt}, false).has_value());
    CHECK(indicator.state() == SyncIndicator::State::Ok);
}

TEST_CASE("a running sync that ends Ok says done; Ok without a run says nothing") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({at_0905(), std::nullopt});
    CHECK_FALSE(indicator.update({at_0905(), std::nullopt}, false).has_value());
    CHECK_FALSE(indicator.update({at_0905(), std::nullopt}, true).has_value());
    const auto done = indicator.update({at_0905(), std::nullopt}, false);
    REQUIRE(done.has_value());
    CHECK(done->kind == app::NoticeKind::Success);
    CHECK(done->text == core::tr("notice.sync_done"));
}

TEST_CASE("after disable() the same error is notified again") {
    core::init_i18n("en");
    SyncIndicator indicator;
    (void)indicator.update({}, true);
    REQUIRE(indicator.update({std::nullopt, failure()}, false).has_value());
    indicator.disable();
    (void)indicator.update({}, true);
    CHECK(indicator.update({std::nullopt, failure()}, false).has_value());
}
