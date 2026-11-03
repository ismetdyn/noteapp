#pragma once

#include "core/error.hpp"

#include <compare>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace noteapp::core {

// Yol bileşeninin (dosya ya da klasör adı) en çok baytı: ext4 NAME_MAX ve NTFS'in 255 UTF-16 birimi; tek
// sahip budur (NotePath kapısı, çakışma kopyası adları, senkron kopya dosyaları). OS sınırıdır, config değil.
inline constexpr std::size_t kMaxFileNameBytes = 255;

// notes_dir'e göre göreli, doğrulanmış yol: mutlak yol, "..", ayrılmış ad ve geçersiz karakter içeremez.
// Ayırıcı yalnız '/' kabul edilir. Sembolik bağla kaçış denetimi NoteStore'un işidir (T07).
class NotePath {
public:
    static std::expected<NotePath, Error> parse(std::string_view relative);

    const std::filesystem::path& value() const { return value_; }
    // '/' ayırıcılı UTF-8 biçim; platformdan bağımsız (listeleme, arama ve harita anahtarı için).
    std::string string() const;

    friend bool operator==(const NotePath&, const NotePath&) = default;
    friend auto operator<=>(const NotePath&, const NotePath&) = default;

private:
    explicit NotePath(std::filesystem::path value) : value_(std::move(value)) {}
    std::filesystem::path value_;
};

// Yolun klasörü, adı ve uzantısı: son '/' ve adın son noktası (baştaki nokta uzantı değildir; `a.tar.gz` ->
// `a.tar` + `gz`). Var olan notun adını bölen her yol (kopya adı, taşıma formu, -2 eki) bu kuralı kullanır.
struct NoteParts {
    std::string folder;  // "" kök
    std::string name;
    std::string type;  // uzantısızsa ""
};
NoteParts split_note_path(std::string_view path);

}  // namespace noteapp::core
