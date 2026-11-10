#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::app {

// `notes` ve `notes-gui` komut satırı. Argümansız açılış eskisi gibidir.
struct UiArgs {
    enum class Kind {
        Run,      // argüman yok: arayüz açılır
        Help,     // --help, -h: kısa kullanım, çıkış 0
        Version,  // --version: `note --version` gibi sürüm satırı, çıkış 0
        Import,   // `notes import YOL... [--folder K] [--name AD] [--overwrite]`
        Invalid,  // kullanım hatası (çıkış 2); `error_key` boşsa yalnız kullanım yazılır
    };
    Kind kind = Kind::Run;

    // Import
    std::vector<std::filesystem::path> sources = {};
    std::optional<std::string> folder = std::nullopt;  // boş: arayüzün app::new_note_folder'ı
    std::string name = {};                             // yalnız tek YOL'la
    bool overwrite = false;

    // Invalid
    std::string error_key = {};
    std::vector<std::pair<std::string, std::string>> error_args = {};
};

// `args` argv[0] olmadan, UTF-8. Saf fonksiyon. `allow_import`: alt komut yalnız `notes`'tadır (GUI'de dosya
// diyaloğu ve sürükle-bırak var).
UiArgs parse_ui_args(std::span<const std::string> args, bool allow_import);

}  // namespace noteapp::app
