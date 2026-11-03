#pragma once

#include "edit/editor_buffer.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Not içi arama: search::fold kuralı (Türkçe dahil büyük/küçük harf duyarsız, ı = i, ş = s). Bayt aralıkları
// özgün metne aittir. Metin satır satır katlanır: tek seferde katlamak 10 MiB'te 160 MB geçici bellek
// isterdi.
std::vector<ByteRange> find_folded(std::string_view text, std::string_view query);

// "3/12": geçerli eşleşme / toplam; eşleşme yoksa boş.
std::string match_label(std::optional<std::size_t> current, std::size_t count);

// Önizlemedeki not içi aramanın durumu: eşleşmeler satır bazlıdır, çünkü önizleme satır dizisi tutar.
class PreviewFind {
public:
    void set_query(std::string query, std::span<const std::string> lines);
    void recompute(std::span<const std::string> lines);
    void clear();
    void next();
    void previous();

    const std::string& query() const { return query_; }
    std::size_t count() const { return matches_.size(); }
    std::optional<std::size_t> current() const;
    std::optional<std::size_t> current_line() const;
    // Satırdaki eşleşmeler, sıralı.
    std::vector<ByteRange> ranges_on_line(std::size_t line) const;
    std::optional<ByteRange> current_range_on_line(std::size_t line) const;

private:
    struct Match {
        std::size_t line = 0;
        ByteRange range;
    };

    std::string query_;
    std::vector<Match> matches_;
    std::size_t index_ = 0;
};

}  // namespace noteapp::tui
