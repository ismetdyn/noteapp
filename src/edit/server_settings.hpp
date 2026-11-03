#pragma once

#include "core/json_parser.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Bir dil sunucusunu başlatmak için gereken her şey: argv'ye eklenenler, ortam ve LSP ayarları.
struct ServerLaunch {
    std::vector<std::string> command{};
    std::vector<std::string> environment{};               // "AD=değer"
    std::string initialization_options{};                 // JSON; boşsa gönderilmez
    core::Json settings{.kind = core::Json::Kind::Null};  // workspace/configuration cevapları
    std::string configuration_change{};        // didChangeConfiguration `settings`ı (JSON); boşsa gönderilmez
    std::vector<std::string> notifications{};  // initialized'dan sonra
};

// argv[0]'ın küçük harf, uzantısız dosya adı ("/usr/bin/rust-analyzer" -> "rust-analyzer",
// "typescript-language-server.cmd" -> "typescript-language-server"): gömülü tablonun anahtarı.
std::string server_program(std::string_view argv0);

// Program gömülü ayar tablosunda (sertleştirme) var mı.
bool has_hardening(std::string_view program);

// `command`ın gömülü ayarı ve kullanıcının [lsp.settings.<dil>] tablosu (`user`, yoksa nullptr) birleşmiş
// hâli; kullanıcı anahtar anahtar üste biner. Tabloda olmayan program ayarsız başlar. Veri dizini isteyen
// sunucuya (jdtls) `lsp_dir/<program>-data` 0700 açılır ve argv'ye eklenir; dizin açılamazsa nullopt.
std::optional<ServerLaunch> server_launch(std::vector<std::string> command, const core::Json* user,
                                          const std::filesystem::path& lsp_dir);

}  // namespace noteapp::tui
