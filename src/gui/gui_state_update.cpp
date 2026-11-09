// Güncelleme (SPEC-gui "Güncelleme"): akışın kararları app::UpdateController'dadır (TUI ile ortak); burada
// işçinin UI iş parçacığına bağlanması, pano ve kapanışın ertelenmesi var.
#include "gui/gui_state.hpp"

#include <algorithm>
#include <chrono>
#include <memory>

namespace noteapp::gui {

app::UpdateController& GuiState::updates() {
    if (!update_) {
        app::UpdateSettings settings;
        settings.limits = config_.limits.update;
        app::UpdateWorkerDeps deps = update_deps();
        // İşçi iş parçacığından: yalnız post (check/install çağrılmaz, UpdateWorkerDeps::on_change).
        deps.on_change = [this] { post([this] { update_changed(); }); };
        update_ = std::make_unique<app::UpdateController>(std::move(settings), std::move(deps));
    }
    return *update_;
}

void GuiState::check_updates_on_startup() {
    updates().check_on_startup(std::chrono::system_clock::now(), config_.update.check_interval);
}

void GuiState::open_update() {
    updates().open(notices_);
}

void GuiState::update_action(app::UpdateAction action) {
    if (action == app::UpdateAction::CopyLink) {
        copy_to_clipboard(update_view().link);
        return;
    }
    updates().act(action);
}

bool GuiState::update_installable() const {
    return std::ranges::contains(update_view().actions, app::UpdateAction::Install);
}

void GuiState::update_changed() {
    if (!update_)
        return;
    update_->changed(notices_, std::chrono::steady_clock::now());
    // Kapanış değiştirme evresindeki kurulumu bekliyordu: bitti, kapanış yeniden istenir (kaydedilmemiş iş
    // yine sorulur).
    if (quit_after_update_ && !update_->busy()) {
        quit_after_update_ = false;
        request_quit();
    }
}

}  // namespace noteapp::gui
