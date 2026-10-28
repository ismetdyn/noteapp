#include "tui/settings.hpp"

#include "core/i18n.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>

namespace noteapp::tui {

namespace {

constexpr std::array<std::string_view, 3> kLanguages = {"auto", "tr", "en"};
constexpr std::string_view kLanguageKey = "language";
constexpr std::string_view kDefaultTypeKey = "default_type";
constexpr std::string_view kTypesKey = "types";
constexpr std::string_view kToggleKey = "sync.enabled";
constexpr char kDelete = 0x7f;

SettingsState::Kind kind_of(std::string_view key) {
    if (key == kLanguageKey || key == kDefaultTypeKey)
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

// Kullanıcının config.toml'u okunamıyorsa save_config onu varsayılanlarla ezerdi (note config set ile aynı
// kural).
bool user_config_broken() {
    for (const core::Error& warning : core::load_config().warnings)
        if (warning.message_key == "config.syntax_error" || warning.message_key == "config.unreadable")
            return true;
    return false;
}

}  // namespace

SettingsState::SettingsState(const core::Config& config) {
    // Sıra, config_entries'in (şema) sırasıdır ve zaten istenen insan sırasıdır.
    for (auto& [key, value] : core::config_entries(config)) {
        const Kind kind = kind_of(key);
        rows_.push_back({std::move(key), kind, value, value, {}});
    }
}

bool SettingsState::changed() const {
    return std::ranges::any_of(rows_, [](const Row& r) { return r.value != r.original; });
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

SettingsState::Outcome SettingsState::handle(Key key, std::string_view text) {
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
    const std::size_t slots = rows_.size() + 1;  // + düğme satırı
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

std::optional<core::Config> SettingsState::commit(const core::Config& base) {
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

    if (user_config_broken()) {
        error_ = core::tr("tui.settings_config_broken", {{"path", to_utf8(core::config_file())}});
        return std::nullopt;
    }
    if (const auto saved = core::save_config(next); !saved) {
        error_ = core::format_error(saved.error());
        return std::nullopt;
    }
    for (Row& row : rows_)
        row.original = row.value;
    return next;
}

std::string settings_label(const SettingsState::Row& row) {
    std::string key = row.key;
    std::ranges::replace(key, '.', '_');
    return core::tr("tui.settings_" + key);
}

}  // namespace noteapp::tui
