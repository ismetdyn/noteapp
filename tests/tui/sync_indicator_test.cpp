#include "core/i18n.hpp"
#include "tui/sync_indicator.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <ctime>
#include <doctest/doctest.h>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

using namespace noteapp;
using tui::SyncIndicator;

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

core::Error failure() {
    return {core::Error::Code::Io, "sync.timeout", {}};
}

}  // namespace

TEST_CASE("a new indicator is disabled and shows the off text") {
    core::init_i18n("en");
    const SyncIndicator indicator;
    CHECK(indicator.state() == SyncIndicator::State::Disabled);
    CHECK(tui::sync_text(indicator) == core::tr("app.sync_off"));
}

TEST_CASE("a last success shows a check mark and the local HH:MM") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({at_0905(), std::nullopt});
    CHECK(indicator.state() == SyncIndicator::State::Ok);
    CHECK(tui::sync_text(indicator) == "✓ 09:05");
    CHECK(indicator.error_text().empty());
}

TEST_CASE("a running sync shows only the spinner glyph, even over an old error") {
    core::init_i18n("en");
    SyncIndicator indicator;
    (void)indicator.update({at_0905(), failure()}, true);
    CHECK(indicator.state() == SyncIndicator::State::Running);
    CHECK(tui::sync_text(indicator) == "⟳");
    CHECK(indicator.error_text().empty());
}

TEST_CASE("an error shows a warning glyph with a short reason and keeps the full text for the message line") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({at_0905(), failure()});
    CHECK(indicator.state() == SyncIndicator::State::Failed);
    const std::string full = core::format_error(failure());
    CHECK(indicator.error_text() == full);
    CHECK(tui::sync_text(indicator).starts_with("⚠ "));
    CHECK(tui::sync_text(indicator).size() < full.size() + 8);
}

TEST_CASE("a long error reason is cut with an ellipsis, a short one is kept") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({std::nullopt, core::Error{core::Error::Code::Io, "sync.timeout", {}}});
    const std::string full = indicator.error_text();
    REQUIRE_FALSE(full.empty());
    CAPTURE(full);
    CHECK(full.size() > 16);  // katalog metni kısa nedenden uzun: aşağıdaki kırpma dalı sınanıyor
    CHECK(tui::sync_text(indicator).find("…") != std::string::npos);
    // Kısa metin kırpılmaz.
    indicator.restore({std::nullopt, core::Error{core::Error::Code::Io, "x", {}}});
    CHECK(tui::sync_text(indicator) == "⚠ " + indicator.error_text());
}

TEST_CASE("neither success nor error yet means the first sync is about to start") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({});
    CHECK(indicator.state() == SyncIndicator::State::Running);
    CHECK(tui::sync_text(indicator) == "⟳");
}

TEST_CASE("disable() returns to the off text from any state") {
    core::init_i18n("en");
    SyncIndicator indicator;
    indicator.restore({at_0905(), std::nullopt});
    indicator.disable();
    CHECK(indicator.state() == SyncIndicator::State::Disabled);
    CHECK(tui::sync_text(indicator) == core::tr("app.sync_off"));
    CHECK(indicator.error_text().empty());
}

namespace {

// 60x15'te üst çubuk (sorgu + mod + sayaç + senkron parçası) tek satıra sığmalı.
std::string render_bar(const SyncIndicator& indicator, const std::string& message) {
    tui::TuiState state;
    state.set_notes({});
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, tui::render(state, {}, {tui::sync_text(indicator), message}, {60, 15}));
    return screen.ToString();
}

}  // namespace

TEST_CASE("every indicator variant fits the 60x15 top bar; an error also fills the message line") {
    core::init_i18n("en");
    SyncIndicator indicator;
    CHECK(render_bar(indicator, "").find(core::tr("app.sync_off")) != std::string::npos);
    indicator.restore({at_0905(), std::nullopt});
    CHECK(render_bar(indicator, "").find("✓ 09:05") != std::string::npos);
    (void)indicator.update({at_0905(), std::nullopt}, true);
    CHECK(render_bar(indicator, "").find("⟳") != std::string::npos);
    indicator.restore({at_0905(), failure()});
    const std::string screen = render_bar(indicator, indicator.error_text());
    CHECK(screen.find("⚠ ") != std::string::npos);
    CHECK(screen.find("[name] · 0 notes") != std::string::npos);  // sayaç kesilmedi
    CHECK(screen.find(indicator.error_text().substr(0, 20)) != std::string::npos);
}
