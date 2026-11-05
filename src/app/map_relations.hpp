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
// Related: remove_relation. Tag: etiketin notun metnindeki her yazımı çıkar (core::remove_note_tag). Link:
// reddedilir (map.link_in_text).
std::expected<std::string, core::Error> disconnect_map_nodes(core::NoteStore& store, const MapNode& from,
                                                             const MapNode& to, MapEdgeKind kind,
                                                             const BeforeWrite& before_write = {});

// İki düğüm arasındaki kenarın türü (yön önemsiz), kaldırılabilen önce: ilişkiyle bağlantı birlikteyse
// ilişki. Kenar yoksa nullopt. TUI'de klavyeyle kaldırma (`d`) içindir.
std::optional<MapEdgeKind> map_edge_between(const NoteMap& map, const MapNode& first, const MapNode& second);

// `at`a (yerleşim birimi) `reach`ten yakın en yakın kenarın dizini; yoksa nullopt. Fare isabeti için: kamera
// arayüzündedir, ekran noktasını yerleşime o çevirir.
std::optional<std::size_t> map_edge_at(const NoteMap& map, std::span<const LayoutPoint> positions,
                                       LayoutPoint at, float reach);

}  // namespace noteapp::app
