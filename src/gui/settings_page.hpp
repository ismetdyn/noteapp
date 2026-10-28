#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "gui/main_window.hpp"

#include <QString>
#include <QWidget>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>

class QLabel;
class QLineEdit;
class QPushButton;

namespace noteapp::gui {

// Sayfanın MainWindow'a bağlandığı yerler (sayfa pencereyi tanımaz).
struct SettingsHost {
    // notes_dir değişmeden önce açık notu kaydeder; false = kaydedilemedi, ayar kaydı iptal olur.
    std::function<bool()> flush;
    // Kaydedilen config'i canlı uygular (liste, tip kutusu, autosave, senkron).
    std::function<void(const core::Config& before, const core::Config& after)> applied;
    // "Bağlantıyı test et" senkronu bitti (çekilen notlar listeye yansısın).
    std::function<void()> synced;
};

// OS kasası dikişi: testler gerçek anahtarlığa yazmaz.
struct TokenStore {
    std::function<bool(std::string_view host)> has;
    std::function<std::expected<void, core::Error>(std::string_view host, std::string_view token)> store;
    std::function<std::expected<void, core::Error>(std::string_view host)> clear;
};

// Ayarlar sekmesi: config_entries'teki 12 anahtarın formu + token satırı + bağlantı testi.
// Doğrulama set_config_value'dadır (CLI/yükleyiciyle aynı kurallar); burada kopyalanmaz.
class SettingsPage : public QWidget {
public:
    SettingsPage(core::Config config, const Deciders& deciders, SettingsHost host, QWidget* parent = nullptr);
    ~SettingsPage() override;

    TokenStore tokens;  // üretimde sync/auth kasası; testler değiştirir

    void save();             // Kaydet
    void revert();           // Geri al: alanlar kaydedilmiş değerlere döner
    void test_connection();  // enable (gerekirse) + tek senkron, işçi iş parçacığında

private:
    struct Field;
    void build_ui();
    void load(const core::Config& config);
    std::string value_of(const std::string& key) const;
    void show_error(const std::string& key, const QString& message);
    void clear_errors();
    void refresh_token_row();
    void save_token();
    void clear_token();
    // Formdaki değişen anahtarları `target`'a uygular; her alanın hatası satır içi gösterilir. false = hata
    // var.
    bool apply_form(core::Config& target);
    // enable akışı: uyuşmazlıkta karar sorulur. Boş metin = başarılı; yoksa gösterilecek hata.
    QString enable_sync(const core::Config& candidate);
    // Kayıt reddedilirken: kullanıcı config'i bozuksa (söz dizimi/okunamıyor) o uyarıyı verir.
    std::expected<core::Config, QString> load_config_for_update() const;

    core::Config applied_;
    const Deciders& deciders_;
    SettingsHost host_;
    std::map<std::string, Field*> fields_;
    QLabel* global_error_ = nullptr;
    QLineEdit* token_edit_ = nullptr;
    QPushButton* token_save_ = nullptr;
    QPushButton* token_clear_ = nullptr;
    QLabel* token_state_ = nullptr;
    QPushButton* test_button_ = nullptr;
    QLabel* test_result_ = nullptr;
    bool testing_ = false;
    std::thread worker_;  // bağlantı testi senkronu; yıkıcı bekler (sync_now zaman aşımıyla sınırlı)
};

}  // namespace noteapp::gui
