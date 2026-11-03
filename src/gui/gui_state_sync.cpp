#include "app/document.hpp"
#include "app/settings.hpp"
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"

#include <ctime>
#include <format>
#include <utility>

namespace noteapp::gui {

namespace {

std::string local_hh_mm(std::chrono::system_clock::time_point when) {
    const std::time_t time = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
}

}  // namespace

void GuiState::changed() {
    ++changes_;
    if (auto_sync_)
        auto_sync_->notify_changed();
}

void GuiState::sync_now() {
    ++sync_requests_;
    if (auto_sync_)
        auto_sync_->request_now();
}

void GuiState::start_sync() {
    if (auto_sync_) {
        // Yıkıcı stop(0) çağırır ve süren birleştirmeyi öldürürdü: önce bekleyen son senkron bitsin.
        deciders.while_busy([this] { stop_sync(std::chrono::seconds(config_.tuning.exit_timeout_seconds)); });
    }
    if (!config_.sync.enabled) {
        show_sync_status({}, false);
        return;
    }
    // Çağrı İŞÇİ iş parçacığından gelir: durum kopyalanıp UI iş parçacığına aktarılır. AutoSync bu nesneden
    // önce yıkılır, `this` geri çağrı sürdükçe geçerlidir. Çekilen notlar sonraki taramayı beklemeden listeye
    // yansır.
    auto_sync_ = std::make_unique<sync::AutoSync>(
        config_, sync_note_exe,
        [this, generation = sync_generation_](const sync::SyncStatus& status, bool running) {
            post([this, generation, status, running] {
                if (generation != sync_generation_)
                    return;
                show_sync_status(status, running);
                if (!running) {
                    rescan();
                    request_sync_states();  // gönderim notları değiştirmeden durumlarını değiştirir
                }
            });
        });
    ++sync_restarts_;
    show_sync_status({}, true);
    auto_sync_->start();
}

void GuiState::stop_sync(std::chrono::seconds timeout) {
    if (!auto_sync_)
        return;
    auto_sync_->stop(timeout);
    auto_sync_.reset();
    ++sync_generation_;
}

void GuiState::show_sync_status(const sync::SyncStatus& status, bool running) {
    if (!auto_sync_ && !running && !status.last_success && !status.last_error)
        sync_label_ = core::tr("gui.sync_off");
    else if (running)
        // NOLINTNEXTLINE(bugprone-branch-clone) -- hata ve son başarı denetiminden önce gelmeli
        sync_label_ = core::tr("gui.sync_running");
    else if (status.last_error)
        sync_label_ = core::tr("gui.sync_error", {{"detail", core::format_error(*status.last_error)}});
    else if (!status.log_issues.empty())  // başarılı ama bir cihazın günlüğü duruyor: sessiz kalmaz
        sync_label_ = core::tr("gui.sync_error", {{"detail", core::format_error(status.log_issues.front())}});
    else if (status.last_success)
        sync_label_ = core::tr("gui.sync_ok", {{"time", local_hh_mm(*status.last_success)}});
    else
        sync_label_ = core::tr("gui.sync_running");  // açıldı, henüz sonuç yok
}

// Kaydedilen ayarları canlı uygular. Dil değişimi canlı çevrilmez (SPEC-gui kapsam dışı): yalnız bildirilir.
void GuiState::apply_settings(const core::Config& before, const core::Config& after) {
    config_ = after;
    autosave_.set_delay(std::chrono::milliseconds(config_.tuning.autosave_ms));
    if (before.notes_dir != after.notes_dir) {
        // SettingsPage kaydetmeden önce flush ettirdi: tampon temiz, eski dizinin notu bırakılır.
        store_ = core::NoteStore::for_config(config_);
        content_index_.reset();
        // Eski dizinin notlarının sekmeleri kapanır; taslaklar dizine bağlı değildir, kalır.
        std::erase_if(tabs_, [](const NoteTab& tab) { return !tab.saved.new_note; });
        active_tab_ = tabs_.empty() ? 0 : tabs_.front().id;
        parked_ = !tabs_.empty();
        shown_.reset();
        doc_.reset_draft();
        current_folder_.clear();
        selected_folder_.reset();
        model_.listing().set_open_folders({});
        folders_.clear();
        model_.listing().set_folders({});
        set_editor({}, false);
    }
    rescan();
    if (after.sync.enabled)
        request_sync_states();
    else if (before.sync.enabled)
        apply_sync_states({});  // kapandı: hepsi sync_off
    if (app::sync_restart_needed(before, after))
        start_sync();
    if (before.language != after.language)
        deciders.info(core::tr("gui.restart_needed"));
}

void GuiState::show_config_warnings(const std::vector<core::Error>& warnings) {
    std::string messages = app::config_warnings_message(warnings);
    // Geçiş bekleyen ya da açılamayan depoda liste boştur: kullanıcı bunu notların kaybı sanmasın.
    if (const auto issue = store_.health()) {
        if (!messages.empty())
            messages += "; ";
        messages += core::format_error(*issue);
    }
    if (!messages.empty())
        status_ = std::move(messages);
}

}  // namespace noteapp::gui
