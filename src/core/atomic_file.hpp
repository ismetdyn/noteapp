#pragma once

#include "core/error.hpp"

#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace noteapp::core {

struct FileWriteResult {
    std::error_code ec;
    bool already_exists = false;
};

// Dosyayı YALNIZ yoksa oluşturur (O_EXCL / CREATE_NEW: yarış olsa bile ezmez), içeriği yazar,
// diske zorlar (fsync / FlushFileBuffers) ve kapatır. Hatada dosyayı silmek çağıranın işidir.
// owner_only: POSIX'ta 0600 (kişisel/geçici dosyalar); aksi hâlde 0666 & ~umask (notlar).
FileWriteResult write_new_file(const std::filesystem::path& file, std::string_view content,
                               bool owner_only = false);

// Var olan ya da olmayan dosyayı atomik değiştirir: aynı dizinde geçici dosya → diske zorla → rename.
// Hata olursa geçici dosya silinir, hedef bozulmaz. İstisna fırlatmaz; Io hatası "path" argümanı
// olarak verilen yolun kendisini taşır.
std::expected<void, Error> write_file_atomically(const std::filesystem::path& target, std::string_view bytes);

}  // namespace noteapp::core
