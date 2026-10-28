#pragma once

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

// Token kasası dikişi (test enjekte eder; gerçek OS kasasına dokunulmaz). Üretimde sync/auth.hpp'ye gider.
struct TokenStore {
    std::function<bool(std::string_view host)> has;
    std::function<std::expected<void, core::Error>(std::string_view host, std::string_view token)> store;
    std::function<std::expected<void, core::Error>(std::string_view host)> clear;
};
TokenStore system_token_store();

// "Senkron açıldı" akışı: depoyu kurar (SyncService::enable). `replace_origin` yalnız kullanıcı onayından
// sonra.
using SyncEnabler =
    std::function<std::expected<void, core::Error>(const core::Config& next, bool replace_origin)>;
SyncEnabler system_sync_enabler();

struct SettingsServices {
    TokenStore tokens = system_token_store();
    SyncEnabler enable = system_sync_enabler();
};

// F2 ayarlar formunun modeli. Satırlar config_entries()'ten gelir (tek şema); doğrulama set_config_value
// ile parse_config'inkiyle AYNIDIR, burada tekrarlanmaz. FTXUI'ye bağlı değildir.
class SettingsState {
public:
    enum class Kind { Choice, Toggle, Text, Token };
    // Replace: origin değiştirme onayı "evet" ile bitti; çağıran submit(config, true) çağırır.
    enum class Outcome { Editing, Cancel, Submit, Replace };

    struct Row {
        std::string key;  // config anahtarı ("sync.enabled")
        Kind kind;
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
    // Satırlar + (token) + düğme.
    std::size_t slot_count() const { return rows_.size() + (token_visible() ? 1 : 0) + 1; }
    bool changed() const;
    const std::string& error() const { return error_; }  // forma ait (satıra değil) hata
    const ConfirmState* discard_confirm() const { return discard_ ? &*discard_ : nullptr; }
    const ConfirmState* replace_confirm() const { return replace_ ? &*replace_ : nullptr; }

private:
    Outcome handle_key(Key key, std::string_view text);
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
    std::optional<ConfirmState> discard_;
    std::optional<ConfirmState> replace_;
    SettingsServices services_;
    std::string token_;        // yazılan, henüz kasaya gitmemiş token; yalnız bellekte
    std::string stored_host_;  // kasa göstergesinin hesaplandığı host
    bool stored_ = false;
};

// Satır etiketi (katalog: tui.settings_<anahtar>, noktalar altçizgi).
std::string settings_label(const SettingsState::Row& row);

}  // namespace noteapp::tui
