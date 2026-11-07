#include "tui/settings.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace noteapp::tui {

namespace {

constexpr std::array<std::string_view, 3> kLanguages = {"auto", "tr", "en"};
constexpr std::array<std::string_view, 2> kIconModes = {"nerd", "off"};
constexpr std::string_view kLanguageKey = "language";
constexpr std::string_view kIconsKey = "icons";
constexpr std::string_view kDefaultTypeKey = "default_type";
constexpr std::string_view kTypesKey = "types";
constexpr std::string_view kTokenKey = "sync.token";  // config anahtarı DEĞİL: yalnız etiket için
constexpr std::size_t kMaskCells = 10;

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

}  // namespace

SettingsState::SettingsState(const core::Config& config, SettingsServices services)
    : form_(config, std::move(services)) {}

void SettingsState::cycle(int delta) {
    Row& row = form_.fields()[focus_];
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
        options = split_types(form_.field(kTypesKey).value);
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

SettingsState::Row SettingsState::token_row() const {
    // Maske sabit üst sınırlı: uzunluk dışarı ipucu vermesin, satır da taşmasın.
    std::string mask;
    for (std::size_t i = 0; i < std::min(form_.token.size(), kMaskCells); ++i)
        mask += "•";
    return {std::string(kTokenKey),
            Kind::Token,
            std::move(mask),
            {},
            {},
            core::tr(form_.token_stored() ? "tui.token_stored" : "app.token_none")};
}

// Her tuştan sonra remote'a bakılır; kasa yalnız host değişince sorulur (her tuşta OS kasasına gidilmez).
SettingsState::Outcome SettingsState::handle(Key key, std::string_view text) {
    const Outcome outcome = handle_key(key, text);
    form_.remote_edited();
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
    if (leave_)
        return handle_leave_key(key);
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
        if (on_test())
            return Outcome::Test;
        if (on_token() && !form_.token.empty()) {
            form_.store_token();  // kasaya yazar, odak yerinde kalır
            return Outcome::Editing;
        }
        focus_ = (focus_ + 1) % slots;
        return Outcome::Editing;
    case Key::Escape:
        if (!changed())
            return Outcome::Cancel;
        leave_ = app::CloseChoice::Cancel;  // varsayılan "Vazgeç": formda kalınır, veri kaybı olmaz
        quitting_ = false;
        return Outcome::Editing;
    default:
        break;
    }
    if (on_button() || on_test())
        return Outcome::Editing;
    if (on_token()) {
        switch (key) {
        case Key::Char:
            if (core::is_printable(text))
                form_.token.append(text);
            break;
        case Key::Backspace:
            core::pop_code_point(form_.token);
            break;
        case Key::CtrlD:
            form_.clear_token();
            break;
        default:
            break;
        }
        return Outcome::Editing;
    }
    Row& row = form_.fields()[focus_];
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
        } else if ((row.kind == Kind::Text || row.key == kDefaultTypeKey) && core::is_printable(text)) {
            row.value.append(text);
            row.error.clear();
        }
        break;
    case Key::Backspace:
        if (row.kind == Kind::Text || row.key == kDefaultTypeKey) {
            core::pop_code_point(row.value);
            row.error.clear();
        }
        break;
    default:
        break;
    }
    return Outcome::Editing;
}

// Kaydet / At / Vazgeç (GUI'nin Ayarlar'dan çıkış sorusu): oklar ve Tab düğmeler arasında döner. Kaydet,
// Kaydet düğmesinin akışıdır (Submit): doğrulama hatası formda kalır, ekran kapanmaz. At formu kapatır;
// yazılmış token yalnız bellekteydi, kasaya gitmez.
bool SettingsState::ask_before_quit() {
    if (!changed())
        return false;
    replace_.reset();  // origin sorusu açıksa yerini çıkış sorusu alır (Vazgeç: forma döner)
    leave_ = app::CloseChoice::Cancel;
    quitting_ = true;
    return true;
}

SettingsState::Outcome SettingsState::handle_leave_key(Key key) {
    constexpr int kChoices = 3;
    const auto step = [this](int delta) {
        const int next = (static_cast<int>(*leave_) + delta + kChoices) % kChoices;
        leave_ = static_cast<app::CloseChoice>(next);
    };
    switch (key) {
    case Key::Left:
    case Key::Up:
    case Key::ShiftTab:
        step(-1);
        return Outcome::Editing;
    case Key::Right:
    case Key::Down:
    case Key::Tab:
        step(+1);
        return Outcome::Editing;
    case Key::Escape:
        leave_.reset();
        quitting_ = false;
        return Outcome::Editing;
    case Key::Enter: {
        const bool quit = std::exchange(quitting_, false);
        switch (*std::exchange(leave_, std::nullopt)) {
        case app::CloseChoice::Save:
            return quit ? Outcome::SubmitAndQuit : Outcome::Submit;
        case app::CloseChoice::Discard:
            return quit ? Outcome::Quit : Outcome::Cancel;
        case app::CloseChoice::Cancel:
            return Outcome::Editing;
        }
        return Outcome::Editing;
    }
    default:
        return Outcome::Editing;  // "s" gibi kısayol yok: yanlışlıkla atmayı önler (ConfirmState gibi)
    }
}

std::optional<core::Config> SettingsState::submit(const core::Config& base, bool replace) {
    auto saved = form_.save(base, replace);
    if (const auto& request = form_.replace_request())
        replace_.emplace(core::tr("tui.settings_confirm_replace", request->args));
    return saved;
}

std::string settings_label(const SettingsState::Row& row) {
    std::string key = row.key;
    std::ranges::replace(key, '.', '_');
    return core::tr("tui.settings_" + key);
}

}  // namespace noteapp::tui
