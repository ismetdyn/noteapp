#pragma once

#include "core/error.hpp"
#include "core/note_path.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::core {

struct NoteInfo {
    NotePath path;
    std::filesystem::file_time_type mtime;
    std::uintmax_t size;
    bool binary;  // ilk 8 KB'ta NUL baytı var: önizleme/arama yapılmaz
};

struct NewNote {
    std::string folder;  // "" = kök
    std::string name;    // "" = içeriğin ilk dolu satırından slug
    std::string type;    // uzantı, başında nokta olmadan
    std::string content;
};

// Okuma anındaki mtime, sonraki write'ın expected_mtime'ı olur (dışarıdan değişiklik tespiti).
struct NoteContent {
    std::string text;
    std::filesystem::file_time_type mtime;
};

// notes_dir kökü altındaki notlar. Her erişim kanonik yolun kök içinde kaldığını doğrular
// (sembolik bağla kaçış reddedilir). Hata durumunda istisna fırlatmaz.
class NoteStore {
public:
    // Kök burada oluşturulmaz; ilk create() oluşturur.
    explicit NoteStore(std::filesystem::path root);

    std::vector<NoteInfo> list() const;        // mtime azalan, eşitlikte yol artan
    std::vector<std::string> folders() const;  // göreli, '/' ayırıcılı, sıralı

    std::expected<NoteContent, Error> read(const NotePath& path) const;
    std::expected<NotePath, Error>
    create(const NewNote& note, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    // Başarıda dosyanın yeni mtime'ını döner. Diskteki mtime expected_mtime'dan farklıysa
    // ModifiedExternally; dosya değişmez.
    std::expected<std::filesystem::file_time_type, Error>
    write(const NotePath& path, std::string_view content, std::filesystem::file_time_type expected_mtime);

private:
    std::expected<std::filesystem::path, Error> resolve(const NotePath& path) const;

    std::filesystem::path root_;
};

}  // namespace noteapp::core
