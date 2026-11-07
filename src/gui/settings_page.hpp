#pragma once

#include "app/settings.hpp"
#include "core/config.hpp"

#include <expected>
#include <functional>
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

// Ayarlar sekmesi: alanlar, doğrulama, token ve kaydet/test akışı app::SettingsForm'dadır (TUI ile tek
// kural); burada ImGui'nin yazdığı alanlar, karar pencereleri (origin değiştirme, bekleme imleci) ve işçi iş
// parçacığı var. Deciders GuiState'indir, onunla yaşar.
class SettingsPage {
    core::Config applied_;
    const Deciders& deciders_;
    SettingsHost host_;
    app::SettingsForm form_;

public:
    SettingsPage(core::Config config, const Deciders& deciders, SettingsHost host);
    ~SettingsPage();
    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    app::TokenStore& tokens = form_.tokens();  // testler değiştirir
    std::string& token =
        form_.token;  // maskeli alanın tamponu; kasaya yazılınca silinir, config'e ve loga girmez

    const std::vector<std::string>& keys() const { return keys_; }                 // form sırası
    std::string& field(const std::string& key) { return form_.field(key).value; }  // ImGui doğrudan yazar
    void field_edited(const std::string& key);                                     // field'a yazdıktan sonra
    void set_field(const std::string& key, std::string value);
    const std::string& field_error(const std::string& key) const { return form_.field(key).error; }
    const std::string& global_error() const { return form_.error(); }
    const std::vector<std::string>& types() const { return applied_.types; }
    // Form kayıtlı değerlerden farklı mı ya da kasaya gitmemiş token yazılı mı (TUI changed() ile aynı).
    bool has_changes() const { return form_.changed(); }

    bool token_enabled() const { return form_.token_enabled(); }
    bool token_stored() const { return form_.token_stored(); }
    const std::string& test_result() const { return form_.test_result(); }
    bool testing() const { return form_.testing(); }

    void opened();           // Ayarlar sekmesi açıldı: kasa göstergesi yeniden sorulur
    void save();             // Kaydet; yazılmış token da kasaya gider (TUI Kaydet gibi)
    void revert();           // Geri al: alanlar kaydedilmiş değerlere döner, yazılmış token silinir
    void test_connection();  // app::run_connection_test (enable + tek senkron), işçi iş parçacığında
    void browse_notes_dir();
    void save_token() { form_.store_token(); }
    void clear_token() { form_.clear_token(); }

private:
    std::vector<std::string> keys_;
    std::thread worker_;  // bağlantı testi senkronu; yıkıcı bekler (sync_now zaman aşımıyla sınırlı)
};

}  // namespace noteapp::gui
