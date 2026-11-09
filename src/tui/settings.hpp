#pragma once

#include "app/settings.hpp"
#include "core/config.hpp"
#include "tui/modals.hpp"
#include "tui/tui_state.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Tek tanımlar src/app'tedir; bu adlar mevcut çağıranlar ve testler için kalır.
using app::config_warnings_message;
using app::SettingsServices;
using app::sync_restart_needed;
using app::SyncEnabler;
using app::system_sync_enabler;
using app::TokenStore;

// F2 ayarlar formu: alanlar, doğrulama, token ve kaydet/test akışı app::SettingsForm'dadır (GUI ile tek
// kural); burada yalnız odak, tuş eşlemesi ve sorular (çıkış, origin değiştirme) var. FTXUI'ye bağlı
// değildir.
class SettingsState {
public:
    using Kind = app::SettingKind;
    using Row = app::SettingsField;
    // Replace: origin değiştirme onayı "evet" ile bitti; çağıran submit(config, true) çağırır. Test:
    // "Bağlantıyı test et"e basıldı; çağıran begin_test ile adayı alır, senkronu işçide koşar.
    // Quit / SubmitAndQuit: çıkış sorusunda (ask_before_quit) At / Kaydet; çağıran uygulamadan çıkar (Kaydet
    // yalnız başarılıysa). CheckUpdates: "Güncellemeleri denetle"ye basıldı; çağıran palet `update` akışını
    // koşar (app::UpdateController::open), form açık kalır.
    enum class Outcome { Editing, Cancel, Submit, Replace, Test, Quit, SubmitAndQuit, CheckUpdates };

    explicit SettingsState(const core::Config& config, SettingsServices services = {});

    Outcome handle(Key key, std::string_view text = {});

    // Kaydet (SPEC-tui "Ayarlar ekranı"): app::SettingsForm::save. origin farklıysa nullopt döner ve
    // replace_confirm() dolar; "evet" gelince Outcome::Replace çıkar ve çağıran `replace=true` ile yeniden
    // dener.
    std::optional<core::Config> submit(const core::Config& base, bool replace = false);

    // Bağlantı testi (GUI ile aynı akış): app::SettingsForm::begin_test / finish_test.
    std::optional<core::Config> begin_test(const core::Config& base) { return form_.begin_test(base); }
    void finish_test(const std::expected<void, core::Error>& outcome) { form_.finish_test(outcome); }
    bool testing() const { return form_.testing(); }
    const std::string& test_result() const { return form_.test_result(); }

    // Satırlar config_entries()'ten gelir (14). Token satırı bunların DIŞINDADIR (token config değildir).
    const std::vector<Row>& rows() const { return form_.fields(); }
    // Token satırı yalnız remote https iken var; odak sırası: satırlar, token, Kaydet düğmesi.
    bool token_visible() const { return form_.token_enabled(); }
    bool on_token() const { return token_visible() && focus_ == rows().size(); }
    std::size_t token_length() const { return form_.token.size(); }  // metnin kendisi dışarı verilmez
    // Çizim için: değer = en çok 10 noktalı maske, note = kasa göstergesi.
    Row token_row() const;
    std::size_t focus() const { return focus_; }
    bool on_button() const { return focus_ == slot_count() - 1; }
    bool on_test() const { return focus_ == slot_count() - 2; }
    bool on_update() const { return focus_ == slot_count() - 3; }
    // Satırlar + (token) + güncelleme denetimi + bağlantı testi + Kaydet.
    std::size_t slot_count() const { return rows().size() + (token_visible() ? 1 : 0) + 3; }
    bool changed() const { return form_.changed(); }
    const std::string& error() const { return form_.error(); }  // forma ait (satıra değil) hata
    // Değişiklikle Esc'te açılan Kaydet / At / Vazgeç sorusunun seçili düğmesi; kapalıysa nullopt.
    std::optional<app::CloseChoice> leave_prompt() const { return leave_; }
    // Ctrl+C (çıkış), GUI pencere kapanışı gibi: değişiklik yoksa false, çağıran hemen çıkar; varsa aynı soru
    // çıkış için açılır (At -> Quit, Kaydet -> SubmitAndQuit, Vazgeç -> formda kalınır).
    bool ask_before_quit();
    const ConfirmState* replace_confirm() const { return replace_ ? &*replace_ : nullptr; }

private:
    Outcome handle_key(Key key, std::string_view text);
    Outcome handle_leave_key(Key key);
    void cycle(int delta);

    app::SettingsForm form_;
    std::size_t focus_ = 0;
    std::optional<app::CloseChoice> leave_;
    bool quitting_ = false;  // açık soru Ctrl+C'den geldi
    std::optional<ConfirmState> replace_;
};

// Satır etiketi (katalog: tui.settings_<anahtar>, noktalar altçizgi).
std::string settings_label(const SettingsState::Row& row);

}  // namespace noteapp::tui
