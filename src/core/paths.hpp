#pragma once

#include <filesystem>
#include <string_view>

namespace noteapp::core {

// Platform dizinleri (SPEC.md §8). NOTEAPP_HOME ayarlıysa config = $NOTEAPP_HOME/config,
// state = $NOTEAPP_HOME/state. Dizinler burada oluşturulmaz.
std::filesystem::path home_dir();
std::filesystem::path config_dir();
std::filesystem::path state_dir();
std::filesystem::path config_file();  // config_dir()/config.toml

// "~" ve "~/…" (Windows'ta "~\…") ile $VAR, ${VAR} (ve yalnız Windows'ta %VAR%) genişletir.
// Tanımsız değişken olduğu gibi bırakılır. Girdi UTF-8'dir; Windows'ta ortam geniş dizgeyle okunur.
std::filesystem::path expand_user_path(std::string_view text);

}  // namespace noteapp::core
