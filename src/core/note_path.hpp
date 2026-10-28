#pragma once

#include "core/error.hpp"

#include <compare>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace noteapp::core {

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

}  // namespace noteapp::core
