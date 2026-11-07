#pragma once

#include "core/config.hpp"
#include "core/error.hpp"

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::app {

// Kaydedilmemiş değişiklikle kapanışta kullanıcının seçimi; sıra iki arayüzde düğme sırasıdır. Ayarlar'dan
// çıkışta (TUI Esc; GUI ←, ⚙, Ctrl+, ve pencere kapanışı) varsayılan Vazgeç'tir: ekranda kalınır, hiçbir şey
// yazılmaz ya da atılmaz.
enum class CloseChoice { Save, Discard, Cancel };

// Token kasası dikişi: testler gerçek OS kasasına dokunmaz, üretimde sync/auth.hpp'ye gider.
struct TokenStore {
    std::function<bool(std::string_view host)> has;
    std::function<std::expected<void, core::Error>(std::string_view host, std::string_view token)> store;
    std::function<std::expected<void, core::Error>(std::string_view host)> clear;
};
TokenStore system_token_store();

// Token yalnız https remote'a aittir; kasa hesabı host'tur. Https değilse nullopt.
std::optional<std::string> https_host(std::string_view remote);

// Kullanıcının config.toml'u okunamıyor ya da söz dizimi bozuksa o uyarı: kayıt dosyayı varsayılanlarla ezer,
// elle yazılmış ayarlar silinirdi (CLI `config set` ile aynı kural). Sağlamsa nullopt.
std::optional<core::Error> config_broken_warning(const core::LoadedConfig& loaded);
// Bozuk config yüzünden reddedilen işin metni (TUI, GUI): dosyanın yolu ve uyarının kendisi (satır/sütun).
// Varsayılan anahtar kaydındır; bağlantı testi kendi anahtarını verir (test kaydetmez).
std::string broken_config_message(const core::Error& warning,
                                  std::string_view message_key = "app.settings_config_broken");

// Ayar değişikliklerini diskteki config'in üstüne yazar: dosya yazmadan hemen önce okunur, uygulama açıkken
// `note config set` ile değişen ayar ezilmez; bozuksa yazılmaz. Ayarlar formu ve tek ayarlık kısayollar
// (icons.toggle) buradan yazar. Hata, gösterilecek metindir.
std::expected<void, std::string>
write_settings(const std::vector<std::pair<std::string, std::string>>& changes);

// "Senkron açıldı" akışı: depoyu kurar (SyncService::enable). `replace_origin` yalnız kullanıcı onayından
// sonra.
using SyncEnabler =
    std::function<std::expected<void, core::Error>(const core::Config& next, bool replace_origin)>;
SyncEnabler system_sync_enabler();

// Bağlantı testi (TUI ve GUI aynı akış): depoyu kurar ve tek senkron koşar. Ağa çıkar: işçi iş parçacığında
// çağrılır. origin değiştirilmez (uyuşmazlık sonuçtur, Kaydet sorar); config.toml'a ve kasaya yazmaz.
std::expected<void, core::Error> run_connection_test(const core::Config& candidate,
                                                     const SyncEnabler& enable);

// Açılışta gösterilecek config uyarıları: her biri format_error ile çevrilir, "; " ile birleşir; uyarı yoksa
// boş. Bozuk config.toml sessizce varsayılan dizine düşmesin diye kullanıcıya gösterilir.
std::string config_warnings_message(const std::vector<core::Error>& warnings);

// Çalışan AutoSync'in yeniden kurulması gerekir mi? Yalnız kurulurken okunan alanlar (etkin/remote/dal/boşta
// süresi/notes_dir) sayılır. Yazar adı/e-postasını `note sync --background` config'ten taze okur; editör gibi
// ilgisiz bir ayar için süren birleştirmeyi öldürmek gerekmez.
bool sync_restart_needed(const core::Config& before, const core::Config& after);

// Ayarlar kaydında dosyadan çalışan config'e geçen sınırlar (TUI ve GUI aynı küme): haritanın düğüm ve
// derinlik sınırı ve ilişki araması (limits.search.relation_depth, tuning.search_relations), uygulama açıkken
// `note config set` ile değişmiş olabilir; arayüz onları haritaya ve listeye hemen uygular. Diğer sınırlar
// açılışta kurulan modüllere verilir, yeniden başlatınca geçer.
void adopt_live_limits(core::Config& live, const core::Config& on_disk);

// Ayar satırının girdi türü: seçenek (oklar), anahtar (boşluk), serbest metin; Token yalnız TUI'nin çizdiği
// token satırındadır (config anahtarı değil).
enum class SettingKind { Choice, Toggle, Text, Token };
SettingKind setting_kind(std::string_view key);

// Formun bir satırı: `original` kayıtlı değer; `error` kaydetme denemesinde dolar; `note` yalnız token
// satırında (kasa göstergesi).
struct SettingsField {
    std::string key;  // config anahtarı ("sync.enabled")
    SettingKind kind{};
    std::string value;
    std::string original;
    std::string error;
    std::string note;
};

struct SettingsServices {
    TokenStore tokens = system_token_store();
    SyncEnabler enable = system_sync_enabler();
};

// Ayarlar formunun modeli (TUI F2 ve GUI Ayarlar sekmesi aynı kurallar): alanlar config_entries() sırasında,
// doğrulama set_config_value'da (CLI ve yükleyiciyle aynı), token ve kaydet/bağlantı testi akışı burada.
// Arayüzler yalnız çizer ve girdiyi alanlara eşler. Sır kuralı: yazılan token yalnız bellekte; kasaya
// TokenStore ile gider, config'e, loga ve hata metnine girmez.
class SettingsForm {
public:
    explicit SettingsForm(const core::Config& applied, SettingsServices services = {});

    std::vector<SettingsField>& fields() { return fields_; }
    const std::vector<SettingsField>& fields() const { return fields_; }
    SettingsField& field(std::string_view key);
    const SettingsField& field(std::string_view key) const;
    // Değerleri ve kayıtlı değerleri `applied`'dan yükler (geri al, kayıt sonrası); hatalar silinir.
    void load(const core::Config& applied);
    void clear_errors();
    // Kaydedilmemiş değer ya da kasaya gitmemiş token var mı.
    bool changed() const;
    const std::string& error() const { return error_; }  // forma ait (alana değil) hata

    // Yazılan token yazıldığı host'a aittir: remote başka host'a dönünce silinir, kayıt onu başka sunucuya
    // götürmez. Token yalnız https remote'ta yazılır; kasa hesabı host'tur.
    std::string token;
    TokenStore& tokens() { return services_.tokens; }
    // sync.remote değiştikten sonra: host değiştiyse yazılan token silinir ve kasa sorulur. Kasa her karede
    // değil, yalnız host değişince ve refresh_token_stored'da (form açılışı) sorulur.
    void remote_edited();
    void refresh_token_stored();
    bool token_enabled() const { return !token_host_.empty(); }
    bool token_stored() const { return token_stored_; }
    // Yazılan token'ı kasaya yazar. Boş token ya da https dışı remote: hiçbir şey yazılmaz, true (kayıtlı
    // token'ı silmek clear_token'ın işidir). false: kasa hatası, error() dolu.
    bool store_token();
    void clear_token();

    // Kaydet: değişen alanlar doğrulanır (her hata kendi alanında), yazılmış token kasaya gider (enable onu
    // kasadan okur), senkron açıkken herhangi bir senkron alanı ya da notes_dir değiştiyse depo kurulur,
    // sonra değişen anahtarlar diskteki config'in üstüne yazılır. Başarıda çalışan config'i (`applied` +
    // değişiklikler + dosyadaki canlı sınırlar) döner; hata formda kalır, nullopt. origin farklıysa nullopt
    // döner ve replace_request() dolar; kullanıcı onaylarsa çağıran `replace=true` ile yeniden çağırır.
    // `flush`: notes_dir değişecekse yazmadan önce açık notu kaydeder; false = kayıt iptal.
    std::optional<core::Config> save(const core::Config& applied, bool replace = false,
                                     const std::function<bool()>& flush = {});
    // Origin uyuşmazlığı; argümanlardan kullanıcı bilgisi çıkarılmıştır (kimlik gösterilmez).
    const std::optional<core::Error>& replace_request() const { return replace_request_; }

    // Bağlantı testi: formdaki değerlerle aday config (bozuk config.toml'da başlamaz); config.toml'a ve
    // kasaya yazmaz. Test sürüyorsa ya da form geçersizse nullopt. Çağıran app::run_connection_test'i işçide
    // koşar, sonucu finish_test'e verir; süren test yoksa finish_test yok sayılır.
    std::optional<core::Config> begin_test(const core::Config& applied);
    void finish_test(const std::expected<void, core::Error>& outcome);
    bool testing() const { return testing_; }
    const std::string& test_result() const { return test_result_; }

private:
    // Doğrular; çalışan config'i (applied + değişiklikler + dosyadaki canlı sınırlar) döner, dosyaya yazmaz.
    // `broken_key`: bozuk config.toml'da forma yazılan metnin anahtarı (kayıt ya da bağlantı testi).
    std::optional<core::Config> build(const core::Config& applied,
                                      std::string_view broken_key = "app.settings_config_broken");
    bool persist();  // değişen alanları write_settings ile yazar; false: error_ dolu

    std::vector<SettingsField> fields_;
    std::string error_;
    SettingsServices services_;
    std::string token_host_;  // `token`'ın ait olduğu host; https değilse boş
    bool token_stored_ = false;
    std::optional<core::Error> replace_request_;
    std::string test_result_;
    bool testing_ = false;
};

}  // namespace noteapp::app
