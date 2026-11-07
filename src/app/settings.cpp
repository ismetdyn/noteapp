#include "app/settings.hpp"

#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "sync/auth.hpp"
#include "sync/sync_service.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace noteapp::app {

TokenStore system_token_store() {
    return {[](std::string_view host) { return sync::has_token(host); },
            [](std::string_view host, std::string_view token) { return sync::store_token(host, token); },
            [](std::string_view host) { return sync::clear_token(host); }};
}

std::optional<std::string> https_host(std::string_view remote) {
    sync::RemoteEndpoint endpoint = sync::parse_remote(remote);
    if (endpoint.auth != sync::Auth::Https)
        return std::nullopt;
    return std::move(endpoint.host);
}

std::optional<core::Error> config_broken_warning(const core::LoadedConfig& loaded) {
    for (const core::Error& warning : loaded.warnings)
        if (warning.message_key == "config.syntax_error" || warning.message_key == "config.unreadable")
            return warning;
    return std::nullopt;
}

std::string broken_config_message(const core::Error& warning, std::string_view message_key) {
    const std::u8string path = core::config_file().u8string();
    return core::tr(message_key, {{"path", std::string(path.begin(), path.end())},
                                  {"detail", core::format_error(warning)}});
}

std::expected<void, std::string>
write_settings(const std::vector<std::pair<std::string, std::string>>& changes) {
    core::LoadedConfig disk = core::load_config();
    if (const auto broken = config_broken_warning(disk))
        return std::unexpected(broken_config_message(*broken));
    for (const auto& [key, value] : changes)
        if (const auto set = core::set_config_value(disk.config, key, value); !set)
            return std::unexpected(core::format_error(set.error()) + ' ' +
                                   core::tr("app.settings_not_saved"));
    if (const auto saved = core::save_config(disk.config); !saved)
        return std::unexpected(core::format_error(saved.error()) + ' ' + core::tr("app.settings_not_saved"));
    return {};
}

SyncEnabler system_sync_enabler() {
    return [](const core::Config& next, bool replace_origin) {
        return sync::SyncService(next).enable(next.sync.remote, replace_origin);
    };
}

std::expected<void, core::Error> run_connection_test(const core::Config& candidate,
                                                     const SyncEnabler& enable) {
    if (auto enabled = enable(candidate, false); !enabled)
        return enabled;
    if (auto synced = sync::SyncService(candidate).sync_now(); !synced)
        return std::unexpected(std::move(synced.error()));
    return {};
}

std::string config_warnings_message(const std::vector<core::Error>& warnings) {
    std::string joined;
    for (const auto& warning : warnings) {
        if (!joined.empty())
            joined += "; ";
        joined += core::format_error(warning);
    }
    return joined;
}

bool sync_restart_needed(const core::Config& before, const core::Config& after) {
    return before.sync.enabled != after.sync.enabled || before.sync.remote != after.sync.remote ||
           before.sync.branch != after.sync.branch || before.sync.idle_seconds != after.sync.idle_seconds ||
           before.notes_dir != after.notes_dir;
}

void adopt_live_limits(core::Config& live, const core::Config& on_disk) {
    live.limits.app.map_nodes = on_disk.limits.app.map_nodes;
    live.limits.app.map_depth = on_disk.limits.app.map_depth;
    live.limits.search.relation_depth = on_disk.limits.search.relation_depth;
    live.tuning.search_relations = on_disk.tuning.search_relations;
}

SettingKind setting_kind(std::string_view key) {
    if (key == "language" || key == "default_type" || key == "icons")
        return SettingKind::Choice;
    return key == "sync.enabled" ? SettingKind::Toggle : SettingKind::Text;
}

SettingsForm::SettingsForm(const core::Config& applied, SettingsServices services)
    : services_(std::move(services)) {
    load(applied);
}

SettingsField& SettingsForm::field(std::string_view key) {
    const auto found = std::ranges::find(fields_, key, &SettingsField::key);
    if (found == fields_.end())
        throw std::out_of_range("not a settings key");  // çağıran sabit anahtar verir: hata bug'dır
    return *found;
}

const SettingsField& SettingsForm::field(std::string_view key) const {
    const auto found = std::ranges::find(fields_, key, &SettingsField::key);
    if (found == fields_.end())
        throw std::out_of_range("not a settings key");  // çağıran sabit anahtar verir: hata bug'dır
    return *found;
}

void SettingsForm::load(const core::Config& applied) {
    // Sıra, config_entries'in (şema) sırasıdır ve zaten istenen insan sırasıdır.
    fields_.clear();
    for (auto& [key, value] : core::config_entries(applied)) {
        const SettingKind kind = setting_kind(key);
        fields_.push_back({std::move(key), kind, value, value, {}, {}});
    }
    error_.clear();
    remote_edited();
}

void SettingsForm::clear_errors() {
    error_.clear();
    for (SettingsField& f : fields_)
        f.error.clear();
}

bool SettingsForm::changed() const {
    return (token_enabled() && !token.empty()) ||
           std::ranges::any_of(fields_, [](const SettingsField& f) { return f.value != f.original; });
}

void SettingsForm::remote_edited() {
    const std::string host = https_host(field("sync.remote").value).value_or("");
    if (host == token_host_)
        return;
    token_host_ = host;
    token.clear();
    refresh_token_stored();
}

void SettingsForm::refresh_token_stored() {
    token_stored_ = token_enabled() && services_.tokens.has(token_host_);
}

bool SettingsForm::store_token() {
    if (!token_enabled() || token.empty())
        return true;
    if (const auto stored = services_.tokens.store(token_host_, token); !stored) {
        error_ = core::format_error(stored.error());  // hata token'ı taşımaz (store_token sözleşmesi)
        return false;
    }
    error_.clear();
    token.clear();
    token_stored_ = true;
    return true;
}

void SettingsForm::clear_token() {
    if (!token_enabled())
        return;
    if (const auto cleared = services_.tokens.clear(token_host_); !cleared) {
        error_ = core::format_error(cleared.error());
        return;
    }
    error_.clear();
    token.clear();  // yazılı kalırsa remote değişince sonraki kayıtta başka host'a giderdi
    token_stored_ = false;
}

std::optional<core::Config> SettingsForm::build(const core::Config& applied, std::string_view broken_key) {
    clear_errors();
    // Enable akışı depoyu kurmadan önce bakılır: yazılamayacak config için depo kurulmasın.
    const core::LoadedConfig disk = core::load_config();
    if (const auto broken = config_broken_warning(disk)) {
        error_ = broken_config_message(*broken, broken_key);
        return std::nullopt;
    }
    // Yalnız değişen anahtarlar uygulanır; her hatalı alan kendi hatasını alır, hatalı değer config'i
    // değiştirmez.
    core::Config live = applied;
    bool valid = true;
    for (SettingsField& f : fields_) {
        if (f.value == f.original)
            continue;
        if (const auto set = core::set_config_value(live, f.key, f.value); !set) {
            f.error = core::format_error(set.error());
            valid = false;
        }
    }
    if (!valid)
        return std::nullopt;
    adopt_live_limits(live, disk.config);  // çağıran haritaya uygular
    return live;
}

// Dosya yazmadan hemen önce yeniden okunur (write_settings): flush, kasa ve enable (ağ) sürerken `note config
// set` ile değişen ayar ezilmez.
bool SettingsForm::persist() {
    std::vector<std::pair<std::string, std::string>> changes;
    for (const SettingsField& f : fields_)
        if (f.value != f.original)
            changes.emplace_back(f.key, f.value);
    if (auto written = write_settings(changes); !written) {
        error_ = std::move(written.error());
        return false;
    }
    for (SettingsField& f : fields_)
        f.original = f.value;
    return true;
}

std::optional<core::Config> SettingsForm::save(const core::Config& applied, bool replace,
                                               const std::function<bool()>& flush) {
    replace_request_.reset();
    auto live = build(applied);
    if (!live)
        return std::nullopt;
    const bool dir_changed = live->notes_dir != applied.notes_dir;
    if (dir_changed && flush && !flush()) {
        error_ = core::tr("app.settings_flush_failed");
        return std::nullopt;
    }
    // Yazılmış ama kasaya gitmemiş token enable'dan önce saklanır: enable onu kasadan okur.
    if (!store_token())
        return std::nullopt;
    // Herhangi bir senkron alanı ya da not dizini değişince depo yeni ayarla kurulur; diğerleri gerektirmez.
    if (live->sync.enabled && (live->sync != applied.sync || dir_changed)) {
        if (const auto enabled = services_.enable(*live, replace); !enabled) {
            const core::Error& error = enabled.error();
            if (error.message_key != "sync.remote_mismatch" || replace) {
                error_ = core::format_error(error);
                return std::nullopt;
            }
            // Hata argümanları adresi taşır; kimlik sızmasın diye kullanıcı bilgisi atılır.
            core::Error shown = error;
            for (auto& [name, value] : shown.args)
                value = sync::without_userinfo(value);
            replace_request_ = std::move(shown);
            return std::nullopt;
        }
    }
    if (!persist())
        return std::nullopt;
    return live;
}

std::optional<core::Config> SettingsForm::begin_test(const core::Config& applied) {
    if (testing_)
        return std::nullopt;
    test_result_.clear();
    auto live = build(applied, "app.test_config_broken");
    if (!live)
        return std::nullopt;
    testing_ = true;
    test_result_ = core::tr("app.test_running");
    return live;
}

void SettingsForm::finish_test(const std::expected<void, core::Error>& outcome) {
    if (!testing_)
        return;
    testing_ = false;
    test_result_ = outcome ? core::tr("app.test_ok")
                           : core::tr("app.test_failed", {{"detail", core::format_error(outcome.error())}});
}

}  // namespace noteapp::app
