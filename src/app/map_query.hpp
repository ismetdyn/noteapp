#pragma once

// Harita sorgu çubuğu (F3; SPEC-gui "Harita" → "Sorgu çubuğu", SPEC-tui "Harita" → "Sorgu"): GUI ve TUI aynı
// akışı çağırır. Okuma hemen çalışır; yazma önce önizlenir (commit edilmeyen işlem), arayüz özeti onaya
// sorar, onayda uygulanır. Sonuç tablo ve haritada vurgudur. Geri alma yoktur (SPEC-graph "Sorgu çubuğu").
#include "app/note_map.hpp"
#include "core/error.hpp"
#include "core/note_query.hpp"
#include "core/note_store.hpp"
#include "graph/query.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Tablonun hücresi. Metin güvenilmez kaynaktandır (not adı, özellik değeri senkronla gelebilir) ama
// query_outcome'da temizlenmiş ve kısaltılmıştır: arayüz olduğu gibi çizer (GUI TextUnformatted/AddText, etiket
// yapmaz; TUI hücreye sığdırır).
struct QueryCell {
    enum class Kind { Value, Null, Note, Tag, Node, Edge, List };
    Kind kind = Kind::Value;
    std::string text;
    std::string note = {};  // Note: canlı notun yolu (tıklayınca açılır)
    bool operator==(const QueryCell&) const = default;
};

struct QueryTable {
    std::vector<std::string> columns;
    std::vector<std::vector<QueryCell>> rows;
};

struct QueryOutcome {
    QueryTable table;
    MapHighlight highlight;
    core::QueryChanges changes;
    bool wrote = false;
    std::string summary;  // durum satırı ve bildirim; ham (yol taşımaz ama arayüz yine temizler)
};

// defaults.toml `limits.app.query_cell_bytes`ın yapılandırmasız karşılığı; bir test ikisinin aynı olduğunu
// denetler.
inline constexpr std::size_t kDefaultQueryCellBytes = 4096;

// Güvenilmez metnin tablo hücresi: `max_bytes`ta kod noktası sınırında `…` ile kesilir, sonra
// core::terminal_safe'ten geçer (sorgu tablosu ve özellik denetçisi aynı kural).
std::string cell_text(std::string_view raw, std::size_t max_bytes);

// Sorgu sonucunu tabloya ve vurguya çevirir (saf). Hücre ve sütun adı burada bir kez temizlenir
// (core::terminal_safe) ve `cell_bytes`ta (SIZE_MAX: sınırsız) kod noktası sınırında `…` ile kesilir: arayüz kare
// başına yeniden temizlemez, dev değer tabloyu şişirmez.
// `view`: sonuç kayıtlı görünümündür (özet adını söyler; görünüm yazılan metni gölgeleyebilir).
QueryOutcome query_outcome(const core::UserQuery& query, bool wrote, std::size_t cell_bytes = SIZE_MAX,
                           std::string_view view = {});
// Aynı hücreler (not yolu, `#etiket`, `(:Etiket kimlik)`, kenar uçlarıyla, UTC zaman), temizlenmemiş ve
// kesilmemiş: CLI tablosu kendi terminal kuralıyla basar (`note query`, `view run`).
QueryTable query_table(const core::UserQuery& query);
// "nodes created: 2, properties set: 3"; değişiklik yoksa "no changes" (çoğul gerektirmeyen biçim).
std::string query_changes_text(const core::QueryChanges& changes);

// Çubuğun durumu ve akışı. Depo çağıranındır (arayüzler kilidiyle çağırır); limitler kullanıcının profili
// (limits.query.*). Geçmiş arayüzlerdedir (tui::InputLine + remember_search, arama geçmişiyle aynı yardımcı).
class MapQuery {
public:
    enum class Step {
        Ran,      // sonuç hazır (okuma, görünüm ya da uygulanan yazma)
        Confirm,  // yazma önizlendi: confirm_text() sorulur, apply() ya da cancel()
        Failed,   // error() doludur; önceki sonuç kalır
    };

    MapQuery() = default;
    // cell_bytes: limits.app.query_cell_bytes (SIZE_MAX: sınırsız).
    explicit MapQuery(const graph::QueryLimits& limits, std::size_t cell_bytes = kDefaultQueryCellBytes)
        : limits_(limits), cell_bytes_(cell_bytes) {}
    void set_limits(const graph::QueryLimits& limits) { limits_ = limits; }
    void set_cell_bytes(std::size_t cell_bytes) { cell_bytes_ = cell_bytes; }

    Step run(core::NoteStore& store, std::string_view text);
    // Kayıtlı görünüm (salt-okunur, görünüm profili).
    Step run_view(const core::NoteStore& store, std::string_view name);
    // Onaylanan yazma: önizlenen metin yeniden ve commit edilerek çalışır (araya giren değişiklik de denetlenir).
    Step apply(core::NoteStore& store);
    void cancel() { pending_.reset(); }
    // Sonuç, vurgu, hata ve bekleyen onay düşer; geçmiş arayüzde kalır.
    void clear();

    bool confirming() const { return pending_.has_value(); }
    const std::string& confirm_text() const { return confirm_text_; }
    const std::optional<QueryOutcome>& outcome() const { return outcome_; }
    const std::string& error() const { return error_; }
    // Tablo görünümü (GUI düğmesi, TUI `O`): yalnız sonuç varken açılır.
    bool table_shown() const { return table_ && outcome_.has_value(); }
    void toggle_table() { table_ = !table_; }

    // Gösterilen sonucun okuma sorgusu (görünüm ya da yazma değil); yoksa boş. `view.save` bunu kaydeder.
    const std::string& read_query() const { return read_query_; }
    // Kayıtlı görünüm (palet `view.save`, TUI haritada `K`, GUI çubuğun düğmesi): ad ve sorgu kuralı CLI
    // `note view save` ile aynı (core::save_view: ad baş/son boşluk ve `/` taşımaz, sorgu salt-okunur
    // olmalı). Aynı ad varsa ezilmez (AlreadyExists): önce silinir. Okuma sonucu yoksa InvalidQuery
    // (error.view_needs_query).
    std::expected<void, core::Error> save_view(core::NoteStore& store, std::string_view name) const;

private:
    Step fail(const core::Error& error);
    Step finish(std::expected<core::UserQuery, core::Error> done, bool wrote, std::string_view view = {});

    graph::QueryLimits limits_;
    std::size_t cell_bytes_ = kDefaultQueryCellBytes;
    std::optional<std::string> pending_;  // onay bekleyen yazma
    std::string confirm_text_;
    std::optional<QueryOutcome> outcome_;
    std::string error_;
    std::string read_query_;
    bool table_ = false;
};

}  // namespace noteapp::app
