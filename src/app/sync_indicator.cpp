#include "app/sync_indicator.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <format>

namespace noteapp::app {

void SyncIndicator::disable() {
    state_ = State::Disabled;
    last_success_.reset();
    last_error_.reset();
    reported_error_.reset();
}

void SyncIndicator::restore(const sync::SyncStatus& status) {
    apply(status, false);
}

std::optional<SyncIndicator::Notice> SyncIndicator::update(const sync::SyncStatus& status, bool running) {
    const bool was_running = state_ == State::Running;
    apply(status, running);
    if (state_ == State::Ok)
        reported_error_.reset();
    if (!was_running)
        return std::nullopt;
    if (state_ == State::Ok)
        return Notice{NoticeKind::Success, core::tr("notice.sync_done")};
    if (state_ == State::Failed && error_text() != reported_error_) {
        reported_error_ = error_text();
        return Notice{NoticeKind::Error, *reported_error_};
    }
    return std::nullopt;
}

void SyncIndicator::apply(const sync::SyncStatus& status, bool running) {
    last_success_ = status.last_success;
    // Başarılı ama bir cihazın günlüğü duruyor: gösterge sessiz kalmaz.
    last_error_ = status.last_error
                      ? status.last_error
                      : (status.log_issues.empty() ? std::nullopt : std::optional(status.log_issues.front()));
    if (running || (!last_error_ && !last_success_))
        state_ = State::Running;
    else
        state_ = last_error_ ? State::Failed : State::Ok;
}

std::string SyncIndicator::success_time() const {
    if (state_ != State::Ok)
        return {};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- Ok yalnız last_success_ doluyken atanır
    const std::tm local = core::local_tm(*last_success_);
    return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
}

std::string SyncIndicator::error_text() const {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- Failed yalnız last_error_ doluyken atanır
    return state_ == State::Failed ? core::format_error(*last_error_) : std::string{};
}

}  // namespace noteapp::app
