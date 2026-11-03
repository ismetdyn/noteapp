#include "tui/settings.hpp"

#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "sync/auth.hpp"
#include "sync/sync_service.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>

namespace noteapp::tui {

namespace {

constexpr std::array<std::string_view, 3> kLanguages = {"auto", "tr", "en"};
constexpr std::array<std::string_view, 2> kIconModes = {"nerd", "off"};
constexpr std::string_view kLanguageKey = "language";
constexpr std::string_view kIconsKey = "icons";
constexpr std::string_view kDefaultTypeKey = "default_type";
constexpr std::string_view kTypesKey = "types";
constexpr std::string_view kToggleKey = "sync.enabled";
constexpr std::string_view kRemoteKey = "sync.remote";
constexpr std::string_view kTokenKey = "sync.token";  // config anahtarı DEĞİL: yalnız etiket için
constexpr std::size_t kMaskCells = 10;
constexpr char kDelete = 0x7f;

SettingsState::Kind kind_of(std::string_view key) {
    if (key == kLanguageKey || key == kDefaultTypeKey || key == kIconsKey)
        return SettingsState::Kind::Choice;
    return key == kToggleKey ? SettingsState::Kind::Toggle : SettingsState::Kind::Text;
}

// "md, txt ,py" -> {"md","txt","py"}; boş parçalar atılır (doğrulamayı set_config_value yapar).
std::vector<std::string> split_types(std::string_view list) {
    std::vector<std::string> out;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        std::string_view part = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        const std::size_t first = part.find_first_not_of(' ');
        if (first == std::string_view::npos)
            continue;
        part = part.substr(first, part.find_last_not_of(' ') - first + 1);
        out.emplace_back(part);
    }
    return out;
}

// UTF-8 devam baytı (10xxxxxx): Backspace bir kod noktasının tamamını silsin.
void pop_code_point(std::string& s) {
    while (s.size() > 1 && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

// Kontrol karakterleri alana girmez (terminal olaylarından sızabilir).
bool is_printable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20 && text.front() != kDelete;
}

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

bool user_config_broken() {
    const core::LoadedConfig loaded = core::load_config();
    for (const core::Error& warning : loaded.warnings)
        if (warning.message_key == "config.syntax_error" || warning.message_key == "config.unreadable")
            return true;
    return false;
}

TokenStore system_token_store() {
    return {[](std::string_view host) { return sync::has_token(host); },
            [](std::string_view host, std::string_view token) { return sync::store_token(host, token); },
            [](std::string_view host) { return sync::clear_token(host); }};
}

SyncEnabler system_sync_enabler() {
    return [](const core::Config& next, bool replace_origin) {
        return sync::SyncService(next).enable(next.sync.remote, replace_origin);
    };
}

SettingsState::SettingsState(const core::Config& config, SettingsServices services)
    : services_(std::move(services)) {
    // Sıra, config_entries'in (şema) sırasıdır ve zaten istenen insan sırasıdır.
    for (auto& [key, value] : core::config_entries(config)) {
        const Kind kind = kind_of(key);
        rows_.push_back({std::move(key), kind, value, value, {}, {}});
    }
    refresh_stored();
}

bool SettingsState::changed() const {
    return !token_.empty() || std::ranges::any_of(rows_, [](const Row& r) { return r.value != r.original; });
}

void SettingsState::cycle(int delta) {
    Row& row = rows_[focus_];
    row.error.clear();
    if (row.kind == Kind::Toggle) {
        row.value = row.value == "true" ? "false" : "true";
        return;
    }
    std::vector<std::string> options;
    if (row.key == kLanguageKey) {
        options.assign(kLanguages.begin(), kLanguages.end());
    } else if (row.key == kIconsKey) {
        options.assign(kIconModes.begin(), kIconModes.end());
    } else {
        // default_type seçenekleri, ekranda o an yazılı `types`'tan gelir (kaydedilmemiş düzenleme dahil).
        const auto types = std::ranges::find(rows_, kTypesKey, &Row::key);
        options = split_types(types->value);
    }
    if (options.empty())
        return;
    const auto n = static_cast<int>(options.size());
    const auto at = std::ranges::find(options, row.value);
    int next = delta > 0 ? 0 : n - 1;  // listede olmayan değerden ilk/son öğeye
    if (at != options.end())
        next = (static_cast<int>(at - options.begin()) + delta + n) % n;
    row.value = options[static_cast<std::size_t>(next)];
}

std::optional<std::string> SettingsState::https_host() const {
    const auto remote = std::ranges::find(rows_, kRemoteKey, &Row::key);
    const sync::RemoteEndpoint endpoint = sync::parse_remote(remote->value);
    if (endpoint.auth != sync::Auth::Https)
        return std::nullopt;
    return endpoint.host;
}

bool SettingsState::token_visible() const {
    return https_host().has_value();
}

SettingsState::Row SettingsState::token_row() const {
    // Maske sabit üst sınırlı: uzunluk dışarı ipucu vermesin, satır da taşmasın.
    std::string mask;
    for (std::size_t i = 0; i < std::min(token_.size(), kMaskCells); ++i)
        mask += "•";
    return {std::string(kTokenKey),
            Kind::Token,
            std::move(mask),
            {},
            {},
            core::tr(stored_ ? "tui.token_stored" : "tui.token_none")};
}

// Kasa göstergesi yalnız host değişince yeniden sorulur (her tuşta/çizimde OS kasasına gidilmez).
void SettingsState::refresh_stored() {
    const std::string host = https_host().value_or("");
    if (host == stored_host_)
        return;
    stored_host_ = host;
    stored_ = !host.empty() && services_.tokens.has(host);
}

bool SettingsState::store_token() {
    const std::string host = https_host().value_or("");
    if (host.empty() || token_.empty())
        return true;
    if (const auto stored = services_.tokens.store(host, token_); !stored) {
        error_ = core::format_error(stored.error());  // hata token'ı taşımaz (store_token sözleşmesi)
        return false;
    }
    error_.clear();
    token_.clear();
    stored_ = true;
    return true;
}

void SettingsState::clear_token() {
    const std::string host = https_host().value_or("");
    if (host.empty())
        return;
    if (const auto cleared = services_.tokens.clear(host); !cleared) {
        error_ = core::format_error(cleared.error());
        return;
    }
    error_.clear();
    token_.clear();
    stored_ = false;
}

SettingsState::Outcome SettingsState::handle(Key key, std::string_view text) {
    const Outcome outcome = handle_key(key, text);
    refresh_stored();
    return outcome;
}

SettingsState::Outcome SettingsState::handle_key(Key key, std::string_view text) {
    if (replace_) {
        switch (replace_->handle(key, text)) {
        case ConfirmState::Outcome::Pending:
            return Outcome::Editing;
        case ConfirmState::Outcome::Cancelled:
            replace_.reset();
            return Outcome::Editing;
        case ConfirmState::Outcome::Confirmed:
            replace_.reset();
            return Outcome::Replace;
        }
    }
    if (discard_) {
        switch (discard_->handle(key, text)) {
        case ConfirmState::Outcome::Pending:
            return Outcome::Editing;
        case ConfirmState::Outcome::Cancelled:
            discard_.reset();
            return Outcome::Editing;
        case ConfirmState::Outcome::Confirmed:
            return Outcome::Cancel;
        }
    }
    const std::size_t slots = slot_count();
    switch (key) {
    case Key::Up:
    case Key::ShiftTab:
        focus_ = (focus_ + slots - 1) % slots;
        return Outcome::Editing;
    case Key::Down:
    case Key::Tab:
        focus_ = (focus_ + 1) % slots;
        return Outcome::Editing;
    case Key::Enter:
        if (on_button())
            return Outcome::Submit;
        if (on_token() && !token_.empty()) {
            store_token();  // kasaya yazar, odak yerinde kalır
            return Outcome::Editing;
        }
        focus_ = (focus_ + 1) % slots;
        return Outcome::Editing;
    case Key::Escape:
        if (!changed())
            return Outcome::Cancel;
        discard_.emplace(core::tr("tui.settings_confirm_discard"));  // varsayılan "Vazgeç": veri kaybı olmaz
        return Outcome::Editing;
    default:
        break;
    }
    if (on_button())
        return Outcome::Editing;
    if (on_token()) {
        switch (key) {
        case Key::Char:
            if (is_printable(text))
                token_.append(text);
            break;
        case Key::Backspace:
            pop_code_point(token_);
            break;
        case Key::CtrlD:
            clear_token();
            break;
        default:
            break;
        }
        return Outcome::Editing;
    }
    Row& row = rows_[focus_];
    switch (key) {
    case Key::Left:
        if (row.kind != Kind::Text)
            cycle(-1);
        break;
    case Key::Right:
        if (row.kind != Kind::Text)
            cycle(+1);
        break;
    case Key::Char:
        if (row.kind == Kind::Toggle) {
            if (text == " ")
                cycle(+1);
        } else if ((row.kind == Kind::Text || row.key == kDefaultTypeKey) && is_printable(text)) {
            row.value.append(text);
            row.error.clear();
        }
        break;
    case Key::Backspace:
        if (row.kind == Kind::Text || row.key == kDefaultTypeKey) {
            pop_code_point(row.value);
            row.error.clear();
        }
        break;
    default:
        break;
    }
    return Outcome::Editing;
}

std::optional<core::Config> SettingsState::build(const core::Config& base) {
    error_.clear();
    for (Row& row : rows_)
        row.error.clear();

    // `types` önce uygulanır: aynı kayıtta değişen default_type yeni listeye göre doğrulanmalı.
    std::vector<Row*> order;
    for (Row& row : rows_)
        if (row.value != row.original)
            order.insert(row.key == kTypesKey ? order.begin() : order.end(), &row);
    core::Config next = base;
    for (Row* row : order)
        if (const auto set = core::set_config_value(next, row->key, row->value); !set) {
            row->error = core::format_error(set.error());
            return std::nullopt;
        }

    // Enable akışı depoyu kurmadan önce bakılır: yazılamayacak config için depo kurulmasın.
    if (user_config_broken()) {
        error_ = core::tr("tui.settings_config_broken", {{"path", to_utf8(core::config_file())}});
        return std::nullopt;
    }
    return next;
}

std::expected<void, core::Error> SettingsState::persist(const core::Config& next) {
    if (auto saved = core::save_config(next); !saved)
        return saved;
    for (Row& row : rows_)
        row.original = row.value;
    return {};
}

std::optional<core::Config> SettingsState::commit(const core::Config& base) {
    auto next = build(base);
    if (!next)
        return std::nullopt;
    if (const auto saved = persist(*next); !saved) {
        error_ = core::format_error(saved.error());
        return std::nullopt;
    }
    return next;
}

std::optional<core::Config> SettingsState::submit(const core::Config& base, bool replace) {
    auto next = build(base);
    if (!next)
        return std::nullopt;
    // Yazılmış ama kasaya gitmemiş token enable'dan önce saklanır: enable onu kasadan okur.
    if (token_visible() && !token_.empty() && !store_token())
        return std::nullopt;
    // Açılış ya da (açıkken) remote değişimi depoyu yeniden kurmayı gerektirir; diğer anahtarlar gerektirmez.
    const bool needs_enable =
        next->sync.enabled && (!base.sync.enabled || next->sync.remote != base.sync.remote);
    if (needs_enable) {
        if (const auto enabled = services_.enable(*next, replace); !enabled) {
            const core::Error& error = enabled.error();
            if (error.message_key != "sync.remote_mismatch" || replace) {
                error_ = core::format_error(error);
                return std::nullopt;
            }
            // Hata argümanları adresi taşır; kimlik sızmasın diye kullanıcı bilgisi atılır.
            core::Args shown;
            for (const auto& [name, value] : error.args)
                shown.emplace_back(name, sync::without_userinfo(value));
            replace_.emplace(core::tr("tui.settings_confirm_replace", shown));
            return std::nullopt;
        }
    }
    if (const auto saved = persist(*next); !saved) {
        error_ = core::format_error(saved.error());
        return std::nullopt;
    }
    return next;
}

std::string settings_label(const SettingsState::Row& row) {
    std::string key = row.key;
    std::ranges::replace(key, '.', '_');
    return core::tr("tui.settings_" + key);
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

}  // namespace noteapp::tui
