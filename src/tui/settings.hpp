#pragma once

#include "app/settings.hpp"
#include "core/config.hpp"
#include "tui/modals.hpp"
#include "tui/tui_state.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Tek tanımlar src/app'tedir; bu adlar mevcut çağıranlar ve testler için kalır.
using app::config_warnings_message;
using app::sync_restart_needed;
using app::SyncEnabler;
using app::system_sync_enabler;
using app::TokenStore;

// Kullanıcının config.toml'u okunamıyorsa save_config onu varsayılanlarla ezerdi (note config set ile aynı
// kural).
bool user_config_broken();

struct SettingsServices {
    TokenStore tokens = app::system_token_store();
    SyncEnabler enable = system_sync_enabler();
};

// F2 ayarlar formunun modeli. Satırlar config_entries()'ten gelir (tek şema); doğrulama set_config_value
// ile parse_config'inkiyle AYNIDIR, burada tekrarlanmaz. FTXUI'ye bağlı değildir.
class SettingsState {
public:
    enum class Kind { Choice, Toggle, Text, Token };
    // Replace: origin değiştirme onayı "evet" ile bitti; çağıran submit(config, true) çağırır. Test:
    // "Bağlantıyı test et"e basıldı; çağıran begin_test ile adayı alır, senkronu işçide koşar.
    // Quit / SubmitAndQuit: çıkış sorusunda (ask_before_quit) At / Kaydet; çağıran uygulamadan çıkar (Kaydet
    // yalnız başarılıysa).
    enum class Outcome { Editing, Cancel, Submit, Replace, Test, Quit, SubmitAndQuit };

    struct Row {
        std::string key;  // config anahtarı ("sync.enabled")
        Kind kind{};
        std::string value;
        std::string original;
        std::string error;  // satır içi (kırmızı) hata; kaydetme denemesinde dolar
        std::string note;   // Token satırı: kasa göstergesi ("kasada kayıtlı ✓" / "yok")
    };

    explicit SettingsState(const core::Config& config, SettingsServices services = {});

    Outcome handle(Key key, std::string_view text = {});

    // Değişen her satırı `base`'in kopyasına uygular, config.toml'u yazar ve yeni Config'i döner. İlk
    // doğrulama hatasında hata o satıra yazılır; kullanıcı dosyası bozuksa üzerine yazılmaz (varsayılanlarla
    // elle yazılmış ayarlar ezilirdi). Başarısızlıkta hiçbir şey yazılmaz, nullopt döner.
    std::optional<core::Config> commit(const core::Config& base);

    // Kaydet: commit()'in üstüne token yazımı ve senkron `enable` akışı (SPEC-tui "Ayarlar ekranı"). Senkron
    // yeni açılıyorsa ya da remote değiştiyse depo kurulur; yalnız başarıda config.toml yazılır. Hata formda
    // satır içi kalır. origin farklıysa nullopt döner ve replace_confirm() dolar; "evet" gelince
    // Outcome::Replace çıkar ve çağıran `replace=true` ile yeniden dener.
    std::optional<core::Config> submit(const core::Config& base, bool replace = false);

    // Bağlantı testi (GUI ile aynı akış): formdaki değerler `base`'e uygulanır; config.toml'a ve kasaya
    // yazılmaz (yazılmış token Kaydet ya da token satırında Enter ile saklanır). Çağıran dönen adayla
    // app::run_connection_test'i (enable + tek senkron, ağa çıkar) işçide koşar, sonucu finish_test'e verir.
    // Test sürüyorsa ya da form geçersizse nullopt; hata satırda ya da formdadır.
    std::optional<core::Config> begin_test(const core::Config& base);
    // Süren test yoksa (form yeniden açıldı) yok sayılır.
    void finish_test(const std::expected<void, core::Error>& outcome);
    bool testing() const { return testing_; }
    const std::string& test_result() const { return test_result_; }

    // Satırlar config_entries()'ten gelir (12). Token satırı bunların DIŞINDADIR (token config değildir).
    const std::vector<Row>& rows() const { return rows_; }
    // Token satırı yalnız remote https iken var; odak sırası: satırlar, token, Kaydet düğmesi.
    bool token_visible() const;
    bool on_token() const { return token_visible() && focus_ == rows_.size(); }
    std::size_t token_length() const { return token_.size(); }  // metnin kendisi dışarı verilmez
    // Çizim için: değer = en çok 12 noktalı maske, note = kasa göstergesi.
    Row token_row() const;
    std::size_t focus() const { return focus_; }
    bool on_button() const { return focus_ == slot_count() - 1; }
    bool on_test() const { return focus_ == slot_count() - 2; }
    // Satırlar + (token) + bağlantı testi + Kaydet.
    std::size_t slot_count() const { return rows_.size() + (token_visible() ? 1 : 0) + 2; }
    bool changed() const;
    const std::string& error() const { return error_; }  // forma ait (satıra değil) hata
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
    // Doğrulayıp `base` kopyasına uygular; dosyaya yazmaz.
    std::optional<core::Config> build(const core::Config& base);
    std::expected<void, core::Error> persist(const core::Config& next);
    std::optional<std::string> https_host() const;  // remote https ise kasa hesabı olan host
    void refresh_stored();
    bool store_token();  // false: kasa hatası (error_ dolu)
    void clear_token();

    std::vector<Row> rows_;
    std::size_t focus_ = 0;
    std::string error_;
    std::optional<app::CloseChoice> leave_;
    bool quitting_ = false;  // açık soru Ctrl+C'den geldi
    std::optional<ConfirmState> replace_;
    SettingsServices services_;
    std::string token_;        // yazılan, henüz kasaya gitmemiş token; yalnız bellekte
    std::string stored_host_;  // kasa göstergesinin hesaplandığı host
    bool stored_ = false;
    std::string test_result_;
    bool testing_ = false;
};

// Satır etiketi (katalog: tui.settings_<anahtar>, noktalar altçizgi).
std::string settings_label(const SettingsState::Row& row);

}  // namespace noteapp::tui
