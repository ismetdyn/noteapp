#include "app/update_controller.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"

#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>
#include <variant>

namespace noteapp::app {

namespace {

std::string available_text(const UpdateAvailable& available, std::string_view current_version) {
    return core::tr("cli.update_available",
                    {{"version", available.version}, {"current", std::string(current_version)}});
}

std::string done_text(const UpdateDone& done) {
    std::string text = core::tr("cli.update_installed", {{"version", done.version}});
    if (done.restart_required)
        text += ". " + core::tr("cli.update_restart");
    return text;
}

}  // namespace

const char* update_action_label(UpdateAction action) {
    switch (action) {
    case UpdateAction::Install:
        return "update.action_install";
    case UpdateAction::Cancel:
        return "update.action_cancel";
    case UpdateAction::CopyLink:
        return "update.action_copy_link";
    case UpdateAction::Close:
        break;
    }
    return "update.action_close";
}

UpdateView update_view(const UpdateStatus& status, std::string_view current_version) {
    using enum UpdateAction;
    const std::vector<UpdateAction> running = {Cancel, Close};
    UpdateView view;
    if (std::holds_alternative<UpdateChecking>(status)) {
        view = {.lines = {core::tr("cli.update_checking")}, .link = {}, .actions = running, .busy = true};
    } else if (const auto* downloading = std::get_if<UpdateDownloading>(&status)) {
        view = {.lines = {core::tr("cli.update_downloading", {{"version", downloading->version}})},
                .link = {},
                .actions = running,
                .busy = true};
    } else if (const auto* installing = std::get_if<UpdateInstalling>(&status)) {
        view = {.lines = {core::tr("cli.update_installing", {{"version", installing->version}})},
                .link = {},
                .actions = running,
                .busy = true};
    } else if (const auto* available = std::get_if<UpdateAvailable>(&status)) {
        // Kendini güncelleyemeyen kurulum: Kur yerine release sayfası (kullanıcı paketi oradan indirir).
        view.lines = {available_text(*available, current_version),
                      available->installable
                          ? core::tr("update.ui_confirm", {{"version", available->version}})
                          : core::tr("update.ui_download_from")};
        view.link = available->release_page;
        view.actions = {available->installable ? Install : CopyLink, Close};
    } else if (const auto* latest = std::get_if<UpToDate>(&status)) {
        view.lines = {core::tr("cli.update_up_to_date", {{"current", std::string(current_version)},
                                                         {"latest", latest->latest_version}})};
        view.actions = {Close};
    } else if (const auto* done = std::get_if<UpdateDone>(&status)) {
        view.lines = {core::tr("cli.update_installed", {{"version", done->version}})};
        if (done->restart_required)
            view.lines.push_back(core::tr("cli.update_restart"));
        view.actions = {Close};
    } else if (const auto* failed = std::get_if<UpdateFailed>(&status)) {
        view.lines = {core::format_error(failed->error)};
        view.actions = {Close};
    } else {
        view.actions = {Close};  // UpdateIdle: denetim başlatılamadı (loglandı)
    }
    return view;
}

UpdateController::UpdateController(UpdateSettings settings, UpdateWorkerDeps deps)
    : worker_(std::move(settings), std::move(deps)) {}

bool UpdateController::start_check(Origin origin) {
    // İş parçacığı açılamazsa (system_error) arayüz çalışmaya devam eder: denetim yalnız yapılmamış olur.
    try {
        if (!worker_.check())
            return false;
    } catch (const std::system_error& error) {
        core::log_error(std::string("event=update_check_not_started detail=\"") + error.what() + "\"");
        return false;
    }
    pending_ = origin;
    return true;
}

bool UpdateController::check_on_startup(std::chrono::system_clock::time_point now,
                                        const core::Limit& interval) {
    // Tavan yok: milisaniyeye sığmayan aralık en büyük süre olur (pratikte bir daha denetlenmez).
    using Rep = std::chrono::milliseconds::rep;
    constexpr auto kMaxMs = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
    const std::chrono::milliseconds every = interval.value >= kMaxMs
                                                ? std::chrono::milliseconds::max()
                                                : std::chrono::milliseconds(static_cast<Rep>(interval.value));
    if (!should_check_on_startup(now, load_update_state(), every))
        return false;
    return start_check(Origin::Startup);
}

void UpdateController::open(NoticeQueue& notices) {
    open_ = true;
    notices.dismiss_update();
    if (worker_.busy()) {
        // Kullanıcı süren açılış denetimini görmek istedi: sonucu artık sessiz değildir.
        if (pending_ == Origin::Startup)
            pending_ = Origin::Manual;
        return;
    }
    // Bulunan sürüm onay bekler; kurulan sürüm yeniden önerilmez (çalışan süreç hâlâ eski sürümdür).
    const UpdateStatus status = worker_.status();
    if (std::holds_alternative<UpdateAvailable>(status) || std::holds_alternative<UpdateDone>(status))
        return;
    start_check(Origin::Manual);
}

void UpdateController::act(UpdateAction action) {
    switch (action) {
    case UpdateAction::Install:
        try {
            if (worker_.install())
                pending_ = Origin::Install;
        } catch (const std::system_error& error) {
            core::log_error(std::string("event=update_install_not_started detail=\"") + error.what() + "\"");
        }
        break;
    case UpdateAction::Cancel:
        worker_.cancel();
        break;
    case UpdateAction::Close:
        open_ = false;
        break;
    case UpdateAction::CopyLink:
        break;
    }
}

UpdateView UpdateController::view() const {
    return update_view(worker_.status(), worker_.current_version());
}

bool UpdateController::changed(NoticeQueue& notices, NoticeQueue::Clock::time_point now) {
    // Ara durumlar (Checking, Downloading) yalnız yeniden çizimdir; biten işlem bir kez söylenir.
    if (!pending_ || worker_.busy())
        return false;
    const Origin origin = *std::exchange(pending_, std::nullopt);
    const UpdateStatus status = worker_.status();
    if (const auto* available = std::get_if<UpdateAvailable>(&status);
        available && origin != Origin::Install) {
        if (open_)
            return false;
        notices.post_update(available_text(*available, worker_.current_version()), available->version, now);
        return true;
    }
    if (origin == Origin::Startup) {
        // Açılış denetimi sessizdir: kullanıcı istemedi, yalnız yeni sürüm söylenir.
        if (const auto* latest = std::get_if<UpToDate>(&status))
            core::log_info("event=update_check_up_to_date latest=" + latest->latest_version);
        else if (const auto* failed = std::get_if<UpdateFailed>(&status))
            core::log_warn("event=update_check_failed detail=" + core::format_error(failed->error));
        return false;
    }
    if (open_)
        return false;  // pencere sonucu gösteriyor
    if (const auto* latest = std::get_if<UpToDate>(&status))
        notices.post(NoticeKind::Info,
                     core::tr("cli.update_up_to_date",
                              {{"current", worker_.current_version()}, {"latest", latest->latest_version}}),
                     now);
    else if (const auto* failed = std::get_if<UpdateFailed>(&status))
        notices.post(NoticeKind::Error, core::format_error(failed->error), now);
    else if (const auto* done = std::get_if<UpdateDone>(&status))
        notices.post(NoticeKind::Success, done_text(*done), now);
    else if (std::holds_alternative<UpdateAvailable>(status))
        notices.post(NoticeKind::Info, core::tr("cli.update_cancelled"), now);  // kurulum iptal edildi
    else
        return false;
    return true;
}

bool UpdateController::stop_for_exit(std::chrono::milliseconds grace) {
    worker_.cancel();
    return !worker_.wait_idle_for(grace);
}

}  // namespace noteapp::app
