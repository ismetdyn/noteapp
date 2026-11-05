#pragma once

#include "app/settings.hpp"
#include "core/config.hpp"

#include <expected>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace noteapp::gui {

struct Deciders;

// Sayfanın GuiState'e bağlandığı yerler (sayfa onu tanımaz).
struct SettingsHost {
    // notes_dir değişmeden önce açık notu kaydeder; false = kaydedilemedi, ayar kaydı iptal olur.
    std::function<bool()> flush;
    // Kaydedilen config'i canlı uygular (liste, autosave, senkron).
    std::function<void(const core::Config& before, const core::Config& after)> applied;
    // "Bağlantıyı test et" senkronu bitti (çekilen notlar listeye yansısın).
    std::function<void()> synced;
    // İşçi iş parçacığından UI iş parçacığına iş aktarır.
    std::function<void(std::function<void()>)> post;
    // Bağlantı testinin sonucu bildirim olur (true = başarılı).
    std::function<void(bool ok, std::string text)> notify = {};
};

// Ayarlar formunun modeli (ImGui bilmez): config_entries'teki 12 anahtar + token satırı + bağlantı testi.
// Doğrulama set_config_value'dadır (CLI/yükleyiciyle aynı kurallar); burada kopyalanmaz. Deciders
// GuiState'indir, onunla yaşar.
class SettingsPage {
public:
    SettingsPage(core::Config config, const Deciders& deciders, SettingsHost host);
    ~SettingsPage();
    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    app::TokenStore tokens = app::system_token_store();  // testler değiştirir
    std::string token;  // maskeli alanın tamponu; kasaya yazılınca silinir, config'e ve loga girmez

    const std::vector<std::string>& keys() const { return keys_; }                // form sırası
    std::string& field(const std::string& key) { return fields_.at(key).value; }  // ImGui doğrudan yazar
    void field_edited(const std::string& key);                                    // field'a yazdıktan sonra
    void set_field(const std::string& key, std::string value);
    const std::string& field_error(const std::string& key) const { return fields_.at(key).error; }
    const std::string& global_error() const { return global_error_; }
    const std::vector<std::string>& types() const { return applied_.types; }
    // Form kayıtlı değerlerden farklı mı ya da kasaya gitmemiş token yazılı mı (TUI changed() ile aynı).
    bool has_changes() const;

    bool token_enabled() const { return token_enabled_; }
    bool token_stored() const { return token_stored_; }
    const std::string& test_result() const { return test_result_; }
    bool testing() const { return testing_; }

    void save();             // Kaydet; yazılmış token da kasaya gider (TUI Kaydet gibi)
    void revert();           // Geri al: alanlar kaydedilmiş değerlere döner, yazılmış token silinir
    void test_connection();  // app::run_connection_test (enable + tek senkron), işçi iş parçacığında
    void browse_notes_dir();
    void save_token();
    void clear_token();

private:
    struct Field {
        std::string value;
        std::string error;
    };

    void load(const core::Config& config);
    void clear_errors();
    void refresh_token_row();
    // Formdaki değişen anahtarları `target`'a uygular; her alanın hatası kendi altında. false = hata var.
    bool apply_form(core::Config& target);
    // enable akışı: uyuşmazlıkta karar sorulur. Boş metin = başarılı; yoksa gösterilecek hata.
    std::string enable_sync(const core::Config& candidate);
    // Kayıt reddedilirken: kullanıcı config'i bozuksa (söz dizimi/okunamıyor) o uyarıyı verir.
    std::expected<core::Config, std::string> load_config_for_update() const;

    core::Config applied_;
    const Deciders& deciders_;
    SettingsHost host_;
    std::vector<std::string> keys_;
    std::map<std::string, Field> fields_;
    std::string global_error_;
    std::string test_result_;
    std::string token_host_;  // `token`'ın yazıldığı host; değişince token silinir
    bool token_enabled_ = false;
    bool token_stored_ = false;
    bool testing_ = false;
    std::thread worker_;  // bağlantı testi senkronu; yıkıcı bekler (sync_now zaman aşımıyla sınırlı)
};

}  // namespace noteapp::gui
