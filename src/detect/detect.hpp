#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::detect {

struct Candidate {
    std::string type;  // "md", "java", ...
    double score{};    // [0, 1]
};

struct Suggestion {
    std::string type;
    double confidence{};            // [0, 1]; seçilen tipin puanı
    std::vector<Candidate> ranked;  // tüm tipler, puana göre azalan
    bool detected{};                // false: içerik algılamaya yetmedi, default_type döndü
};

// Değerler resources/defaults.toml'dan gelir (T08); bilerek varsayılanı yok.
struct Options {
    std::string default_type;
    double min_confidence{};
    std::size_t max_scan_bytes{};
};

// Saf ve deterministik öneri (SPEC-detect). Karar her zaman kullanıcınındır.
Suggestion suggest(std::string_view content, const Options& options);

// Satırlar tutarlı bir tablo mu: `,`, `;`, sekme içinden tutarlı ayırıcı; yoksa nullopt. CSV algısıyla aynı
// kural.
std::optional<char> table_delimiter(const std::vector<std::string_view>& lines);

}  // namespace noteapp::detect
