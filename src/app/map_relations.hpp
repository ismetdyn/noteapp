#pragma once

#include "app/graph_layout.hpp"
#include "app/note_map.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Haritada ilişki kurma ve kaldırma (SPEC-gui/SPEC-tui "Harita"): GUI Shift+sürükleme ve kenara sağ tık, TUI
// sağ sürükleme ve `C` kipi aynı kuralı çağırır. Depo işlemleri var olanlardır: ilişki add/remove_relation,
// bağlantı ve etiket notun metnindedir (write).

// Bir düğüm ötekinin üstüne bırakılınca ne olur.
enum class MapDrop {
    None,            // aynı düğüm, etiket → etiket
    ChooseNoteEdge,  // not → not: ilişki mi bağlantı mı, kullanıcı seçer
    AddTag,          // not ↔ etiket (iki yön de): etiket nota eklenir
};
MapDrop map_drop(const MapNode& from, const MapNode& to);

// Kenar haritadan kaldırılabilir mi: ilişki ve etiket evet; bağlantı notun metnindeki [[...]]'dır, metin
// sessizce düzenlenmez.
bool map_edge_removable(MapEdgeKind kind);

// İşlemin gövdesini yazacağı not: bağlantıda kaynak, etikette not ucu; ilişkide yok (gövdeye dokunmaz).
// Arayüz yazmadan önce bu notun bekleyen düzenlemesini kaydeder.
std::optional<core::NotePath> map_edit_note(const MapNode& from, const MapNode& to, MapEdgeKind kind);

// `kind`: Related (add_relation), Link (`from`un sonuna `[[to]]`, yol tam yazılır: kısa ad başka nota
// çözülebilir) ya da Tag (not ↔ etiket, yön önemsiz: notun sonuna `#etiket`). Başarıda durum satırı iletisi;
// yollar güvenilmezdir, arayüz kendi temizliğini uygular. Bağlantı ve etiket yalnız düzyazı notuna eklenir
// (core::is_prose_note); var olan ilişki, bağlantı ya da etiket AlreadyExists, not yoksa NotFound, dışarıda
// değişmişse ModifiedExternally (not değişmez). `before_write` test kancasıdır: gövde okunduktan sonra,
// yazılmadan önce çağrılır (başka cihazın araya giren yazması); arayüzler vermez.
using BeforeWrite = std::function<void()>;
std::expected<std::string, core::Error> connect_map_nodes(core::NoteStore& store, const MapNode& from,
                                                          const MapNode& to, MapEdgeKind kind,
                                                          const BeforeWrite& before_write = {});
// Related: yalnız `relation` (MapEdge::relation) silinir, çiftin öbür ilişkileri kalır; kimlik boşsa
// NotFound. Tag: etiketin notun metnindeki her yazımı çıkar (core::remove_note_tag). Link: reddedilir
// (map.link_in_text).
std::expected<std::string, core::Error> disconnect_map_nodes(core::NoteStore& store, const MapNode& from,
                                                             const MapNode& to, MapEdgeKind kind,
                                                             const core::RelationId& relation = {},
                                                             const BeforeWrite& before_write = {});

// Adlı ilişki (SPEC-graph "Adlı ilişkiler"): GUI "Bağla…" penceresi ve TUI ad satırı bunu çağırır.
// connect_map_nodes(Related) adsız yönsüz ilişkidir. Hatalar NoteStore::add_relation'ınkilerdir.
std::expected<std::string, core::Error> relate_map_notes(core::NoteStore& store, const MapNode& from,
                                                         const MapNode& to,
                                                         const core::RelationSpec& relation);
// Yalnız `relation` silinir (disconnect_map_nodes(Related) budur); ileti adını söyler.
std::expected<std::string, core::Error> unrelate_map_notes(core::NoteStore& store, const MapNode& from,
                                                           const MapNode& to,
                                                           const core::RelationId& relation);

// Kenar menüsü, bağlantı paneli, TUI `n` ve `note relation rename`: yalnız `relation`ın adı değişir, kimliği
// kalır (NoteStore::rename_relation; boş ad adsız yapar). `first`/`second` iletinin uçlarıdır; kimliği
// doğrulamaz, depo doğrular. İleti kırpılmış yeni adı söyler (güvenilmez: arayüz yine temizler).
std::expected<std::string, core::Error> rename_map_relation(core::NoteStore& store, const core::NotePath& first,
                                                            const core::NotePath& second,
                                                            const core::RelationId& relation,
                                                            std::string_view new_name);
// Yön: `source` boşsa yönsüz, doluysa o uçtan öbürüne (NoteStore::set_relation_direction; kaynak değişirse
// kenarın kimliği de değişir). İleti yönü söyler; `first`/`second` iletinin uçlarıdır.
std::expected<std::string, core::Error> direct_map_relation(core::NoteStore& store, const core::NotePath& first,
                                                            const core::NotePath& second,
                                                            const core::RelationId& relation,
                                                            const std::optional<core::NotePath>& source);
// "Yön çevir": `shown` notunun gözünden `direction`daki ilişkinin yeni kaynağı. Çıkan ilişki gelen olur
// (kaynak `other`), gelen ve yönsüz ilişki çıkan olur (kaynak `shown`).
core::NotePath flipped_source(const core::NotePath& shown, const core::NotePath& other,
                              core::RelationDirection direction);
// Bağlantı bölümünün ilişkili satırı (TUI ve GUI): yön oku (↗ bu nottan çıkan, ↙ gelen), yol ve ad:
// "↗ b.md — uses". Ad ve yol güvenilmez: arayüz kendi temizliğinden geçirir.
std::string related_row_text(const core::RelatedNote& row);

// Seçicinin süzgeci: yazılan metin kırpılıp tag_key ile katlanır (Türkçe İ/I); anahtarı onunla başlayan adlar
// önce, sonra içerenler, ikisinde de `names` sırası (relation_names: sayı azalan). Boş metin hepsini verir.
// exact: yazılan, var olan bir adla aynı anahtarda (seçici "yeni ad" satırını göstermez). Geçersiz ad
// (denetim karakteri) eşleşmez; arayüz hatayı NoteStore'dan alır.
struct RelationMatches {
    std::vector<core::RelationName> names;
    bool exact = false;
};
RelationMatches match_relation_names(std::span<const core::RelationName> names, std::string_view typed);
// "Yönlü" kutusunun varsayılanı: var olan adda RelationName::default_directed, yeni adda yönsüz.
bool default_directed(std::span<const core::RelationName> names, std::string_view typed);

// İki düğüm arasındaki kenar (yön önemsiz), kaldırılabilen önce: ilişkiyle bağlantı birlikteyse ilişki;
// birden çok ilişkide çizgedeki ilki. Kenar yoksa nullptr; işaretçi `map` değişene dek geçerlidir. TUI'de
// klavyeyle kaldırma (`d`) içindir: iletisi hangi ilişkinin kalktığını söyler.
const MapEdge* map_edge_between(const NoteMap& map, const MapNode& first, const MapNode& second);

// `at`a (yerleşim birimi) `reach`ten yakın en yakın kenarın dizini; yoksa nullopt. Fare isabeti için: kamera
// arayüzündedir, ekran noktasını yerleşime o çevirir.
std::optional<std::size_t> map_edge_at(const NoteMap& map, std::span<const LayoutPoint> positions,
                                       LayoutPoint at, float reach);

}  // namespace noteapp::app
