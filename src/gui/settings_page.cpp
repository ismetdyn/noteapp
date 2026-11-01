#include "gui/settings_page.hpp"

#include "core/i18n.hpp"
#include "gui/gui_state.hpp"
#include "sync/auth.hpp"
#include "sync/sync_service.hpp"

#include <utility>

namespace noteapp::gui {

namespace {

// Token yalnız https remote'a aittir; kasa hesabı host'tur. Https değilse boş.
std::string https_host(const std::string& remote) {
    const sync::RemoteEndpoint endpoint = sync::parse_remote(remote);
    return endpoint.auth == sync::Auth::Https ? endpoint.host : std::string{};
}

}  // namespace

SettingsPage::SettingsPage(core::Config config, const Deciders& deciders, SettingsHost host)
    : applied_(std::move(config)), deciders_(deciders), host_(std::move(host)) {
    tokens.has = [](std::string_view h) { return sync::has_token(h); };
    tokens.store = [](std::string_view h, std::string_view t) { return sync::store_token(h, t); };
    tokens.clear = [](std::string_view h) { return sync::clear_token(h); };
    for (const auto& [key, ignored] : core::config_entries(applied_))
        keys_.push_back(key);
    load(applied_);
}

SettingsPage::~SettingsPage() {
    if (worker_.joinable())
        worker_.join();
}

void SettingsPage::load(const core::Config& config) {
    for (const auto& [key, value] : core::config_entries(config))
        fields_[key].value = value;
    clear_errors();
    refresh_token_row();
}

void SettingsPage::field_edited(const std::string& key) {
    if (key == "sync.remote")
        refresh_token_row();
}

void SettingsPage::set_field(const std::string& key, std::string value) {
    field(key) = std::move(value);
    field_edited(key);
}

bool SettingsPage::has_changes() const {
    for (const auto& [key, saved_value] : core::config_entries(applied_)) {
        if (fields_.at(key).value != saved_value)
            return true;
    }
    return false;
}

void SettingsPage::clear_errors() {
    for (auto& [key, entry] : fields_)
        entry.error.clear();
    global_error_.clear();
}

void SettingsPage::refresh_token_row() {
    const std::string host = https_host(field("sync.remote"));
    token_enabled_ = !host.empty();
    token_stored_ = token_enabled_ && tokens.has(host);
}

void SettingsPage::revert() {
    load(applied_);
}

void SettingsPage::browse_notes_dir() {
    if (auto chosen = deciders_.browse_dir(field("notes_dir")))
        field("notes_dir") = std::move(*chosen);
}

std::expected<core::Config, std::string> SettingsPage::load_config_for_update() const {
    // CLI `config set` ile aynı kural: bozuk dosya varsayılanlarla ezilip kullanıcının ayarları silinmesin.
    core::LoadedConfig fresh = core::load_config();
    for (const core::Error& warning : fresh.warnings) {
        if (warning.message_key == "config.syntax_error" || warning.message_key == "config.unreadable")
            return std::unexpected(core::format_error(warning) + ' ' + core::tr("gui.settings_not_saved"));
    }
    return std::move(fresh.config);
}

bool SettingsPage::apply_form(core::Config& target) {
    bool valid = true;
    // Yalnız değişen anahtarlar uygulanır: dokunulmayan ayar (ör. elle yazılmış, formda görünmeyen biçim)
    // kaydedilirken yeniden yazılmaz. Doğrulama set_config_value'da; kural burada kopyalanmaz.
    for (const auto& [key, saved_value] : core::config_entries(applied_)) {
        Field& entry = fields_.at(key);
        if (entry.value == saved_value)
            continue;
        if (const auto set = core::set_config_value(target, key, entry.value); !set) {
            entry.error = core::format_error(set.error());
            valid = false;
        }
    }
    return valid;
}

// ponytail: enable ağa çıkabilir ve UI iş parçacığında koşar (bekleme imleci); yavaş uzaklarda pencere
// donarsa işçi iş parçacığına alınır. Karar penceresi (uyuşmazlık) zaten UI iş parçacığı ister.
std::string SettingsPage::enable_sync(const core::Config& candidate) {
    sync::SyncService service(candidate);
    std::expected<void, core::Error> result;
    deciders_.while_busy([&] { result = service.enable(candidate.sync.remote); });
    if (!result && result.error().message_key == "sync.remote_mismatch") {
        std::string message = core::format_error(result.error());
        if (!deciders_.confirm_replace_origin(message))
            return message;  // reddedildi: hiçbir şey kaydedilmez, açıklama alanın altında kalır
        deciders_.while_busy([&] { result = service.enable(candidate.sync.remote, true); });
    }
    return result ? std::string{} : core::format_error(result.error());
}

void SettingsPage::save() {
    clear_errors();
    const auto fresh = load_config_for_update();
    if (!fresh) {
        global_error_ = fresh.error();
        return;
    }
    core::Config live = applied_;  // canlı uygulanacak (tuning dahil, çalışan config)
    if (!apply_form(live))
        return;
    core::Config persisted = *fresh;  // dosyaya yazılacak: diskteki ayarların üstüne aynı değişiklikler
    if (!apply_form(persisted))
        return;

    const bool dir_changed = live.notes_dir != applied_.notes_dir;
    if (dir_changed && !host_.flush()) {
        global_error_ = core::tr("gui.settings_flush_failed");
        return;
    }
    if (live.sync.enabled && (live.sync != applied_.sync || dir_changed)) {
        if (std::string error = enable_sync(live); !error.empty()) {
            fields_.at("sync.remote").error = std::move(error);
            return;
        }
    }
    if (const auto saved = core::save_config(persisted); !saved) {
        global_error_ = core::format_error(saved.error()) + ' ' + core::tr("gui.settings_not_saved");
        return;
    }
    const core::Config before = std::exchange(applied_, live);
    load(applied_);  // normalleşmiş değerler (ör. "md,py" -> "md, py") forma döner
    host_.applied(before, applied_);
}

void SettingsPage::save_token() {
    const std::string host = https_host(field("sync.remote"));
    if (host.empty())
        return;
    if (const auto stored = tokens.store(host, token); !stored) {
        global_error_ = core::format_error(stored.error());
        return;
    }
    global_error_.clear();
    token.clear();  // token ekranda bekletilmez
    refresh_token_row();
}

void SettingsPage::clear_token() {
    const std::string host = https_host(field("sync.remote"));
    if (host.empty())
        return;
    if (const auto cleared = tokens.clear(host); !cleared) {
        global_error_ = core::format_error(cleared.error());
        return;
    }
    global_error_.clear();
    token.clear();  // yazılı kalırsa remote değişince sonraki kayıtta başka host'a giderdi
    refresh_token_row();
}

void SettingsPage::test_connection() {
    if (testing_)
        return;
    clear_errors();
    test_result_.clear();
    core::Config candidate = applied_;
    if (!apply_form(candidate))
        return;
    if (const std::string error = enable_sync(candidate); !error.empty()) {
        test_result_ = core::tr("gui.test_failed", {{"detail", error}});
        return;
    }
    testing_ = true;
    if (worker_.joinable())
        worker_.join();  // önceki iş bitti (testing_ false iken yalnız birikmiş iş parçacığı)
    // Ağ senkronu UI'yi dondurmasın diye işçide; sonuç kuyruktan UI iş parçacığına döner.
    worker_ = std::thread([this, candidate = std::move(candidate)] {
        auto outcome = sync::SyncService(candidate).sync_now();
        host_.post([this, outcome = std::move(outcome)] {
            testing_ = false;
            test_result_ =
                outcome ? core::tr("gui.test_ok")
                        : core::tr("gui.test_failed", {{"detail", core::format_error(outcome.error())}});
            if (outcome && host_.synced)
                host_.synced();  // çekilen notlar listeye yansısın
        });
    });
}

}  // namespace noteapp::gui
