#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace noteapp::core {

// Platform dizinleri (SPEC.md §8). NOTEAPP_HOME ayarlıysa config = $NOTEAPP_HOME/config,
// state = $NOTEAPP_HOME/state, data = $NOTEAPP_HOME/data, varsayılan notes_dir = $NOTEAPP_HOME/notes.
// Dizinler burada oluşturulmaz.
std::filesystem::path home_dir();
std::filesystem::path config_dir();
std::filesystem::path state_dir();
// Kalıcı kullanıcı verisi (graph.db): NOTEAPP_HOME/data, $XDG_DATA_HOME/noteapp (~/.local/share/noteapp),
// macOS
// ~/Library/Application Support/noteapp, Windows %LOCALAPPDATA%/noteapp. Durum ya da önbellek değildir:
// silinmesi senkronsuz cihazda notların kaybıdır.
std::filesystem::path data_dir();
// Yalnız sahibinin erişebileceği dizin (POSIX 0700; var olan da daralır). Windows'ta kullanıcı profilinin
// ACL'si geçerli.
std::error_code create_private_directories(const std::filesystem::path& dir);
std::filesystem::path config_file();  // config_dir()/config.toml
// NOTEAPP_HOME ayarlıysa varsayılan notes_dir; ayarlı değilse defaults.toml'deki geçerlidir.
std::optional<std::filesystem::path> isolated_notes_dir();

// "~" ve "~/…" (Windows'ta "~\…") ile $VAR, ${VAR} (ve yalnız Windows'ta %VAR%) genişletir.
// Tanımsız değişken olduğu gibi bırakılır. Girdi UTF-8'dir; Windows'ta ortam geniş dizgeyle okunur.
std::filesystem::path expand_user_path(std::string_view text);

}  // namespace noteapp::core
