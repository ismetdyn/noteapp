#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::search {

// Bir eşleşen satır. `line` 1 tabanlıdır; `ranges` kesit (`snippet`) içindeki bayt aralıklarıdır [başlangıç,
// bitiş).
struct Hit {
    std::size_t line;
    std::string snippet;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
};

struct ContentResult {
    core::NotePath path;
    std::size_t hit_count;        // en az bir terim geçen satır sayısı
    std::vector<Hit> first_hits;  // ilk 3 eşleşen satır
};

// İçerik sorgusunun katlanmış terimleri: boşlukla ayrılır, "..." arası tek ifade. Arama ve arayüz vurgusu
// aynı kuralı kullansın diye tek kaynak burasıdır.
std::vector<std::u32string> parse_terms(std::string_view query);

// Bellek içi tam metin önbelleği; G/Ç yalnız refresh'te. Tek iş parçacığından kullanılır (SPEC-search
// "Eşzamanlılık").
class ContentIndex {
public:
    ContentIndex(const core::NoteStore& store, std::size_t max_note_bytes);

    // Yalnız yeni veya (mtime, boyut) değişen notları yeniden okur; silinenleri, ikilileri ve büyükleri
    // düşürür.
    void refresh();
    std::vector<ContentResult> search(std::string_view query, std::size_t limit) const;
    // Test kancası: şimdiye kadar (yeniden) okunan dosya sayısı.
    std::size_t read_count() const { return read_count_; }

private:
    struct Entry {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
        std::string text;                         // özgün metin (kesitler buradan kesilir)
        std::u32string folded;                    // katlanmış kod noktaları; refresh'te bir kez hesaplanır
        std::vector<std::uint32_t> byte_offsets;  // kod noktası -> özgün bayt ofseti
        std::vector<std::uint32_t> line_starts;   // satır başlarının kod noktası indeksi
    };

    const core::NoteStore& store_;
    std::size_t max_note_bytes_;
    std::map<core::NotePath, Entry> entries_;
    std::size_t read_count_ = 0;
};

}  // namespace noteapp::search
