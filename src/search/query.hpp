#pragma once

#include "core/error.hpp"
#include "core/note_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::search {

enum class SortField { Name, Size, Updated, Created };

struct SortSpec {
    SortField field{};
    bool descending = false;
};

// Tek filtre; bir sorgudaki filtrelerin hepsi sağlanmalı (AND). Yalnız kind'ın kullandığı alanlar anlamlıdır.
struct Filter {
    enum class Kind { Type, Folder, Size, Updated, Created, Origin, Sync, Tag, View };
    enum class Compare { Less, Greater, Within };

    Kind kind{};
    Compare compare = Compare::Greater;               // Size, Updated, Created
    std::string text = {};                            // katlanmış: Type, Folder ("work/"), Origin, Tag
    std::uintmax_t bytes = 0;                         // Size
    std::chrono::sys_days day = {};                   // Updated/Created, Less/Greater
    std::chrono::days age = {};                       // Updated/Created, Within
    core::SyncState sync = core::SyncState::SyncOff;  // Sync
    // View: görünümün döndürdüğü notlar; text ham ad. Arama bağlanmadan ayrıştırıldıysa boş: not geçmez.
    std::shared_ptr<const std::set<core::NotePath>> notes = nullptr;
};

// `view:` değerinin notları (ön yüz depoya bağlar: core::view_notes). Hata, değerin yerinde QueryError olur.
using ViewLookup =
    std::function<std::expected<std::vector<core::NotePath>, core::Error>(std::string_view name)>;

struct ParsedQuery {
    std::string text;  // kalan sözcükler, tırnaklarıyla: parse_terms ve fuzzy aynı metni görür
    std::vector<Filter> filters;
    std::optional<SortSpec> sort;  // birden çok sort: verilirse sonuncusu
};

// Gösterim için yeri belli hata: [begin, end) sorgunun bozuk değerinin bayt aralığıdır. format_error(error)
// mesajı verir ("error.query_invalid": key, value, reason).
struct QueryError {
    std::size_t begin{};
    std::size_t end{};
    core::Error error;
};

// Ayrıştırıcı; tek G/Ç views'tur. Bilinen anahtarlar: sort type in size updated created origin sync tag view.
// Anahtarlar ve değerler search'ün katlama kurallarıyla karşılaştırılır. Bilinmeyen anahtar (foo:bar,
// http://x) düz metindir; bilinen anahtarın geçersiz değeri metne düşmez, QueryError döner. views verilmezse
// `view:` yalnız ad kuralından geçer (girdi boyama), süzgeci hiçbir notu geçirmez. Biçimler için SPEC-search.
std::expected<ParsedQuery, QueryError> parse_query(std::string_view query, const ViewLookup& views = {});

// indices'i verilen alana göre kararlı sıralar (eşitlikte gelen sıra kalır); bilinmeyen created her iki yönde
// sonda.
void sort_notes(std::vector<std::size_t>& indices, const SortSpec& sort,
                std::span<const core::NoteMeta> notes);

// Filtrelerden geçen notların notes içindeki sırası; sort yoksa gelen sıra. Bilinmeyen created/origin
// filtreye takılır. Argümanlara göre saftır; yalnız tarih sınırları yerel saat dilimiyle hesaplanır ("gün"
// kullanıcının günüdür).
std::vector<std::size_t> apply(std::span<const Filter> filters, const std::optional<SortSpec>& sort,
                               std::span<const core::NoteMeta> notes,
                               std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

}  // namespace noteapp::search
