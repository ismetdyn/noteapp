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

// notes_dir kökü altındaki notlar. Her erişimde kökün altındaki hiçbir yol bileşeni bağ olamaz (sembolik
// bağ ve Windows junction'ı; reason "link") ve kanonik yol kök içinde kalmalıdır. Hata durumunda istisna
// fırlatmaz.
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

    // Taşı/yeniden adlandır; hedef klasörler oluşur, uzantı değişimi tip değişimidir. Hedef varsa
    // AlreadyExists (asla ezilmez). Başarıda hedef yolu döner.
    std::expected<NotePath, Error> move(const NotePath& from, const NotePath& to);
    // Notu kalıcı siler (çöp kutusu yok); onay ön yüzün işidir.
    std::expected<void, Error> remove(const NotePath& path);

    // Klasör adları NotePath kurallarıyla doğrulanır. create_folder boş klasöre gizli .keep koyar.
    std::expected<void, Error> create_folder(std::string_view folder);
    std::expected<void, Error> rename_folder(std::string_view from, std::string_view to);
    // Not (ya da bilinmeyen dosya) içeren klasör force olmadan NotEmpty ({count} = dosya sayısı) verir
    // ve hiçbir şey silinmez; force özyinelemeli siler (.keep dahil). İçinde bağ olan klasör force'la da
    // silinmez (InvalidPath, reason "contains_link").
    std::expected<void, Error> remove_folder(std::string_view folder, bool force);

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
