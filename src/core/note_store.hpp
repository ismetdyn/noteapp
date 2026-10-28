#pragma once

#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/text.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
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
    explicit NoteStore(std::filesystem::path root, std::size_t slug_max_length = kDefaultSlugMaxLength);

    std::vector<NoteInfo> list() const;        // mtime azalan, eşitlikte yol artan
    std::vector<std::string> folders() const;  // göreli, '/' ayırıcılı, sıralı

    // Test kancası: list()'in diskten dosya başı okuma sayısı (önbellek isabeti okutmaz).
    std::size_t head_read_count() const { return head_reads_; }

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
    std::size_t slug_max_length_;

    // list() ikili bayrağı (mtime, boyut) anahtarıyla önbelleğe alır: her çağrıda 5.000 dosyanın başını
    // okumak ~540 ms tutuyordu. const list() içinde güncellendiği için mutable; NoteStore tek iş
    // parçacığından kullanılır (SPEC-search "Eşzamanlılık"), kilit yok.
    struct BinaryCacheEntry {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
        bool binary;
    };
    mutable std::unordered_map<std::string, BinaryCacheEntry> binary_cache_;
    mutable std::size_t head_reads_ = 0;
};

}  // namespace noteapp::core
