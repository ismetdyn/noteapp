#include "tui/sync_indicator.hpp"

#include "core/i18n.hpp"

#include <cstddef>
#include <ctime>
#include <format>

namespace noteapp::tui {

namespace {

// Üst çubukta hata nedenine ayrılan en çok kod noktası: 60 sütunda sayaç ve mod kesilmesin. Tam metin
// durum satırındadır.
constexpr std::size_t kShortReasonCodePoints = 16;

std::string clock_text(std::chrono::system_clock::time_point when) {
    const std::time_t time = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
}

// UTF-8'i kod noktası sınırında keser (devam baytı 10xxxxxx).
std::string shorten(const std::string& text, std::size_t limit) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
            continue;
        if (count++ == limit)
            return text.substr(0, i) + "…";
    }
    return text;
}

}  // namespace

void SyncIndicator::disable() {
    state_ = State::Disabled;
    last_success_.reset();
    last_error_.reset();
}

void SyncIndicator::set_status(const sync::SyncStatus& status, bool running) {
    last_success_ = status.last_success;
    last_error_ = status.last_error
                      ? status.last_error
                      : (status.log_issues.empty() ? std::nullopt : std::optional(status.log_issues.front()));
    if (running || (!last_error_ && !last_success_))
        state_ = State::Running;
    else
        state_ = last_error_ ? State::Failed : State::Ok;
}

std::string SyncIndicator::text() const {
    switch (state_) {
    case State::Disabled:
        return core::tr("tui.sync_off");
    case State::Running:
        return "⟳";
    case State::Ok:
        return "✓ " + clock_text(*last_success_);
    case State::Failed:
        return "⚠ " + shorten(error_text(), kShortReasonCodePoints);
    }
    return {};
}

std::string SyncIndicator::error_text() const {
    return state_ == State::Failed ? core::format_error(*last_error_) : std::string{};
}

}  // namespace noteapp::tui
