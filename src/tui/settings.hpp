#pragma once

#include "core/config.hpp"
#include "tui/modals.hpp"
#include "tui/tui_state.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// F2 ayarlar formunun modeli. Satırlar config_entries()'ten gelir (tek şema); doğrulama set_config_value
// ile parse_config'inkiyle AYNIDIR, burada tekrarlanmaz. FTXUI'ye bağlı değildir.
class SettingsState {
public:
    enum class Kind { Choice, Toggle, Text };
    enum class Outcome { Editing, Cancel, Submit };

    struct Row {
        std::string key;  // config anahtarı ("sync.enabled")
        Kind kind;
        std::string value;
        std::string original;
        std::string error;  // satır içi (kırmızı) hata; kaydetme denemesinde dolar
    };

    explicit SettingsState(const core::Config& config);

    Outcome handle(Key key, std::string_view text = {});

    // Değişen her satırı `base`'in kopyasına uygular, config.toml'u yazar ve yeni Config'i döner. İlk
    // doğrulama hatasında hata o satıra yazılır; kullanıcı dosyası bozuksa üzerine yazılmaz (varsayılanlarla
    // elle yazılmış ayarlar ezilirdi). Başarısızlıkta hiçbir şey yazılmaz, nullopt döner.
    std::optional<core::Config> commit(const core::Config& base);

    const std::vector<Row>& rows() const { return rows_; }
    // rows().size() = "Kaydet" düğmesi satırı.
    std::size_t focus() const { return focus_; }
    bool on_button() const { return focus_ == rows_.size(); }
    bool changed() const;
    const std::string& error() const { return error_; }  // forma ait (satıra değil) hata
    const ConfirmState* discard_confirm() const { return discard_ ? &*discard_ : nullptr; }

private:
    void cycle(int delta);

    std::vector<Row> rows_;
    std::size_t focus_ = 0;
    std::string error_;
    std::optional<ConfirmState> discard_;
};

// Satır etiketi (katalog: tui.settings_<anahtar>, noktalar altçizgi).
std::string settings_label(const SettingsState::Row& row);

}  // namespace noteapp::tui
