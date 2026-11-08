#include "app/document.hpp"
#include "app/settings.hpp"
#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "sync/sync_service.hpp"

#include <utility>

namespace noteapp::gui {

void GuiState::changed() {
    ++changes_;
    word_tables_.request();  // yazılan not sözcük tablosuna girsin
    language_servers_.notes_changed();  // çalışma alanı yansıtması boşta tazelenir
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
        sync_.disable();
        show_sync_label();
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
    sync_.restore(sync::SyncService(config_).status());  // ilk senkronun ⟳'u işçiden gelir
    show_sync_label();
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
    const auto notice = sync_.update(status, running);
    show_sync_label();
    if (notice)
        notify(notice->kind, notice->text);
}

void GuiState::show_sync_label() {
    switch (sync_.state()) {
    case SyncIndicator::Disabled:
        sync_label_ = core::tr("app.sync_off");
        break;
    case SyncIndicator::Running:
        sync_label_ = core::tr("gui.sync_running");
        break;
    case SyncIndicator::Ok:
        sync_label_ = core::tr("gui.sync_ok", {{"time", sync_.success_time()}});
        break;
    case SyncIndicator::Failed:
        sync_label_ = core::tr("gui.sync_error", {{"detail", sync_.error_text()}});
        break;
    }
}

// Kaydedilen ayarları canlı uygular. Dil değişimi canlı çevrilmez (SPEC-gui kapsam dışı): yalnız bildirilir.
void GuiState::apply_settings(const core::Config& before, const core::Config& after) {
    config_ = after;
    autosave_.set_delay(std::chrono::milliseconds(config_.tuning.autosave_ms));
    notices_.set_lifetime(app::NoticeQueue::lifetime_of(config_.tuning.notice_ms.value));
    map_.set_node_limit(config_.limits.app.map_nodes);  // app::adopt_live_limits: dosyadan gelir
    map_.set_depth_limit(config_.limits.app.map_depth);
    model_.listing().set_relation_depth(app::relation_depth(config_));
    model_.refresh();  // açık arama yeni ilişki ayarıyla yeniden süzülür
    if (before.notes_dir != after.notes_dir) {
        // SettingsPage kaydetmeden önce flush ettirdi: tampon temiz, eski dizinin notu bırakılır.
        store_ = core::NoteStore::for_config(config_);
        clear_map_query();  // eski deponun sonucu ve yolda olan açılış görünümü yeni depoya uygulanmaz
        word_tables_.set_store(store_);
        language_servers_.set_store(store_);
        content_index_.reset();
        // Eski dizinin notlarının sekmeleri kapanır; taslaklar dizine bağlı değildir, kalır.
        tabs_.close_if([](const NoteTab& tab) { return !tab.saved.new_note; });
        tabs_.activate(tabs_.empty() ? 0 : tabs_.front().id);
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
    else if (before.sync.enabled) {
        sync_status_.cancel();  // yoldaki istek eski durumu geri getirmesin
        apply_sync_states({});  // kapandı: hepsi sync_off
    }
    if (app::sync_restart_needed(before, after))
        start_sync();
    if (before.language != after.language)
        deciders.info(core::tr("gui.restart_needed"));
}

void GuiState::show_config_warnings(const std::vector<core::Error>& warnings) {
    std::string messages = app::config_warnings_message(warnings);
    // Geçiş bekleyen ya da açılamayan depoda liste boştur: kullanıcı bunu notların kaybı sanmasın.
    store_issue_ = store_.health();
    if (store_issue_) {
        if (!messages.empty())
            messages += "; ";
        messages += core::format_error(*store_issue_);
    }
    if (!messages.empty())
        error_status_ = status_ = std::move(messages);
}

}  // namespace noteapp::gui
