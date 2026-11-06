#pragma once

// Özellik denetçisi (F4; SPEC-gui "Özellik denetçisi", SPEC-tui "Denetçi"): GUI yan paneli ve TUI sağ panel
// bölümü aynı modeli çizer. Konu bir not ya da bir ilişki (RELATED_TO kenarı); satırlar kullanıcının
// özellikleri, notun etiketleri ("label" = etiket, notun metnindeki `#etiket`) ve uygulamanın salt okunur
// anahtarlarıdır. Yazma kuralı core'dadır (core/note_props.hpp: sorgu çubuğunun korumasıyla aynı); burada
// sıra, gösterim metni ve iletiler.
#include "app/map_query.hpp"
#include "core/error.hpp"
#include "core/note_props.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

struct InspectTarget {
    enum class Kind { Note, Relation };
    Kind kind = Kind::Note;
    // Note: notun yolu (hep dolu). Relation: başlığın ilk ucu (yönlüde kaynak), `other` öbür uç; kimlikle
    // açılan ilişkide (CLI) boş.
    std::optional<core::NotePath> note = std::nullopt;
    std::optional<core::NotePath> other = std::nullopt;
    core::RelationId relation = {};  // Relation
    std::string name = {};           // Relation: adı (güvenilmez); boş = adsız
    bool directed = false;           // Relation
    bool operator==(const InspectTarget&) const = default;

    static InspectTarget of_note(core::NotePath path) {
        return {.kind = Kind::Note, .note = std::move(path)};
    }
    static InspectTarget of_relation(core::RelationId id, std::optional<core::NotePath> from = std::nullopt,
                                     std::optional<core::NotePath> to = std::nullopt, std::string name = {},
                                     bool directed = false) {
        return {Kind::Relation, std::move(from), std::move(to), id, std::move(name), directed};
    }
};

struct InspectorRow {
    enum class Kind { Property, Label };
    Kind kind = Kind::Property;
    std::string key = {};         // ham anahtar (Label: etiketin ilk yazılışı); yazma bununla
    std::string key_text = {};    // gösterim: terminal_safe ve hücre sınırında kesik
    std::string value_text = {};  // gösterim: aynı kural; büyük değer "<N bytes>", etikette boş
    std::string type = {};        // "text" "number" "bool" "time" "bytes" "label": inspector.type_<ad>
    std::string input =
        {};  // düzenleme satırının metni (core::property_input); salt okunurda ve etikette boş
    std::optional<core::PropertyType> value_type = std::nullopt;  // düzenlenebilir değerin türü
    bool read_only = false;
    bool operator==(const InspectorRow&) const = default;
};

class Inspector {
public:
    // cell_bytes: limits.app.query_cell_bytes (sorgu tablosunun hücresiyle aynı sınır; SIZE_MAX sınırsız).
    explicit Inspector(std::size_t cell_bytes = kDefaultQueryCellBytes) : cell_bytes_(cell_bytes) {}
    void set_cell_bytes(std::size_t cell_bytes) { cell_bytes_ = cell_bytes; }

    // Konu değişince satırlar ve seçim düşer; aynı konu dokunmaz. Satırlar reload ile gelir.
    void inspect(InspectTarget target);
    void clear();
    const std::optional<InspectTarget>& target() const { return target_; }

    // Konunun satırları depodan: önce düzenlenebilir özellikler, sonra etiketler (notta), sonra uygulamanın
    // anahtarları; her grup anahtar sırasıyla. Hata satırları boşaltır, error() doludur (ham).
    void reload(const core::NoteStore& store);
    const std::vector<InspectorRow>& rows() const { return rows_; }
    const std::string& error() const { return error_; }

    // Seçim (TUI; GUI satırı tıklayarak seçer). Satırlar değişince sınıra çekilir.
    std::size_t selected() const { return selected_; }
    const InspectorRow* selected_row() const { return rows_.empty() ? nullptr : &rows_[selected_]; }
    void move(int delta);
    void select(std::size_t row);

    // Tek işlemlik yazmalar; başarıda ileti (ham: arayüz temizler) ve satırlar yeniden okunur. Hata
    // core::Error'dır (anahtar, ayrılmış anahtar, değer, bulunamadı), hiçbir şey yazılmamıştır. `input`
    // core::parse_property'den geçer (`type` yoksa önek ya da kendiliğinden).
    std::expected<std::string, core::Error> set(core::NoteStore& store, std::string_view key,
                                                std::string_view input,
                                                std::optional<core::PropertyType> type = std::nullopt);
    std::expected<std::string, core::Error> remove(core::NoteStore& store, std::string_view key);
    // Etiket: notun metninin sonuna `#etiket` (connect_map_nodes Tag) ya da her yazımı çıkar (disconnect).
    // Gövdeyi yazar: arayüz önce notun bekleyen düzenlemesini kaydeder. `#` yazılabilir. Yalnız not
    // konusunda.
    std::expected<std::string, core::Error> add_label(core::NoteStore& store, std::string_view tag);
    std::expected<std::string, core::Error> remove_label(core::NoteStore& store, std::string_view tag);

    // Silme onayının sorusu (ham); satır bir özellik ya da etiket.
    std::string confirm_remove_text(const InspectorRow& row) const;
    // Başlık: "a.md" ya da "a.md → b.md 'uses'" (ham).
    std::string title() const;

private:
    std::optional<InspectTarget> target_;
    std::vector<InspectorRow> rows_;
    std::string error_;
    std::size_t selected_ = 0;
    std::size_t cell_bytes_;
};

}  // namespace noteapp::app
