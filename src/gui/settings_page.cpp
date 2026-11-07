#include "gui/settings_page.hpp"

#include "core/i18n.hpp"
#include "gui/gui_state.hpp"

#include <utility>

namespace noteapp::gui {

// ponytail: Kaydet'te enable ağa çıkabilir ve UI iş parçacığında koşar (bekleme imleci); karar penceresi
// (uyuşmazlık) UI iş parçacığı ister. Yavaş uzaklarda pencere donarsa bağlantı testi gibi işçiye alınır.
SettingsPage::SettingsPage(core::Config config, const Deciders& deciders, SettingsHost host)
    : applied_(std::move(config)), deciders_(deciders), host_(std::move(host)),
      form_(applied_,
            {.tokens = app::system_token_store(), .enable = [this](const core::Config& next, bool replace) {
                 std::expected<void, core::Error> result;
                 deciders_.while_busy([&] { result = app::system_sync_enabler()(next, replace); });
                 return result;
             }}) {
    for (const app::SettingsField& entry : form_.fields())
        keys_.push_back(entry.key);
}

SettingsPage::~SettingsPage() {
    if (worker_.joinable())
        worker_.join();
}

void SettingsPage::field_edited(const std::string& key) {
    if (key == "sync.remote")
        form_.remote_edited();
}

void SettingsPage::set_field(const std::string& key, std::string value) {
    field(key) = std::move(value);
    field_edited(key);
}

void SettingsPage::opened() {
    form_.refresh_token_stored();
}

void SettingsPage::revert() {
    form_.load(applied_);
    token.clear();
}

void SettingsPage::browse_notes_dir() {
    if (auto chosen = deciders_.browse_dir(field("notes_dir")))
        field("notes_dir") = std::move(*chosen);
}

void SettingsPage::save() {
    auto saved = form_.save(applied_, false, host_.flush);
    if (!saved && form_.replace_request()) {
        std::string message = core::format_error(*form_.replace_request());
        if (!deciders_.confirm_replace_origin(message)) {
            // Reddedildi: config yazılmaz, açıklama alanın altında kalır. Yazılmış token enable'dan önce
            // kasaya gitti ve orada kalır (TUI submit ile aynı).
            form_.field("sync.remote").error = std::move(message);
            return;
        }
        saved = form_.save(applied_, true, host_.flush);
    }
    if (!saved)
        return;
    const core::Config before = std::exchange(applied_, std::move(*saved));
    form_.load(applied_);  // normalleşmiş değerler (ör. "md,py" -> "md, py") forma döner
    host_.applied(before, applied_);
}

void SettingsPage::test_connection() {
    auto candidate = form_.begin_test(applied_);
    if (!candidate)
        return;
    if (worker_.joinable())
        worker_.join();  // önceki iş bitti (testing false iken yalnız birikmiş iş parçacığı)
    // enable ve senkron ağa çıkar: UI'yi dondurmasın diye işçide (TUI ile aynı akış); sonuç kuyruktan UI iş
    // parçacığına döner.
    worker_ = std::thread([this, candidate = std::move(*candidate)] {
        auto outcome = app::run_connection_test(candidate, app::system_sync_enabler());
        host_.post([this, outcome = std::move(outcome)] {
            form_.finish_test(outcome);
            if (outcome && host_.synced)
                host_.synced();  // çekilen notlar listeye yansısın
            if (host_.notify)
                host_.notify(outcome.has_value(), form_.test_result());
        });
    });
}

}  // namespace noteapp::gui
