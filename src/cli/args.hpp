#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::cli {

// command_table.hpp'deki komut adları (help dahil) ve gizli `__complete`.
bool is_reserved_command(std::string_view word);

struct ParsedArgs {
    enum class Kind {
        Add,       // hızlı ekleme (metin/stdin/editör)
        Help,      // --help, -h, help [KOMUT]; `help KOMUT` için `command` dolu
        Version,   // --version
        Reserved,  // ayrılmış alt komut; `command` + `rest` dolu
        Invalid    // kullanım hatası; `error_key` + `error_args` dolu (çıkış 2)
    };

    Kind kind = Kind::Add;

    // Add
    std::optional<std::string> type;
    std::optional<std::string> folder;
    std::optional<std::string> name;
    bool no_sync = false;
    bool editor = false;  // -e: editörü her durumda aç; metin argümanları ön içerik olur
    std::vector<std::string> text;

    // Reserved
    std::string command;
    std::vector<std::string> rest;

    // Invalid
    std::string error_key;
    std::vector<std::pair<std::string, std::string>> error_args;
};

// `args` argv[0] olmadan, UTF-8. Saf fonksiyon: G/Ç yok.
ParsedArgs parse_args(const std::vector<std::string>& args);

}  // namespace noteapp::cli
