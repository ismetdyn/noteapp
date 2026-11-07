#pragma once

#include "app/listing.hpp"
#include "app/trash_ops.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::app {

// Çoklu seçimin, yank'in ve taşımanın öğesi: not ya da klasör.
struct ItemRef {
    core::NotePath path;
    bool is_folder = false;
};

// Seçim kümesindeki anahtar: klasör `/` ile biter, aynı yoldaki notla karışmaz.
std::string mark_key(std::string_view path, bool is_folder);
// Seçim anahtarlarından öğeler, anahtar sırasıyla. Seçili klasörün altındaki seçili öğe atlanır: klasörle
// birlikte gider, ikinci kez işlenip hata vermesin. Bozuk anahtar atlanır.
std::vector<ItemRef> marked_items(const std::set<std::string>& marks);
// Taramadan sonra listede artık olmayan öğenin (senkron, başka süreç) işareti düşer: toplu işlem onu hata diye
// raporlamasın.
void prune_marks(std::set<std::string>& marks, const Listing& listing);
// Seçim öğeleri çöp hedefi olarak (TrashOps::trash_items, delete_items).
std::vector<TrashTarget> trash_targets(std::span<const ItemRef> items);

// Panoya giden not metni: özgün baytlar (editörün LF gösterimi değil). Biri doludur: okuma hatası `error`; ikili
// ya da geçersiz UTF-8 not pano taşıyamaz, `refused` nedenin i18n anahtarıdır.
struct ClipboardText {
    std::string text = {};
    std::optional<core::Error> error = std::nullopt;
    const char* refused = nullptr;
};
ClipboardText clipboard_text(const core::NoteStore& store, const core::NotePath& path);

// Yapıştırma: öğeler `dest`e kopyalanır, kaynak yerinde kalır. Not baytları aynen kopyalanır; ad doluysa
// deponun numarası (`ad-2.md`, `ad-3.md`...), klasörde de aynı ek; deneme tavanı yok (dolu ad sonludur). Biri
// düşerse kalanlar sürer. Klasör kendi içine kopyalanmaz.
BatchReport paste_items(core::NoteStore& store, std::span<const ItemRef> items, std::string_view dest);

struct MoveResult {
    std::size_t moved = 0;
    std::optional<ItemRef> first = std::nullopt;        // ilk taşınan öğe, yeni yoluyla
    std::optional<core::Error> failure = std::nullopt;  // ilk hata; kalan öğeler yerinde
};

// Taşıma: öğeler `dest`e aynı adla gider. Hedefte ad doluysa (gösterilen listeye göre) ya da klasör kendi
// içine gidecekse hiçbir öğe taşınmaz, ezme yok. Depo hatasında durur; o ana dek taşınanlar taşınmış kalır.
// Hızlı ekleme satırı (TUI `a`, GUI A): metin klasör mü (`/` ile biter, kök değildir).
bool names_folder(std::string_view text);
// Ad satırındaki metinden boş not: uzantı yazılmadıysa `default_type`, klasör yazılmadıysa `default_folder`
// (app::parse_draft_name). Yazılan ad doluysa `-2` eklenmez, AlreadyExists döner: satır açık kalır.
std::expected<core::CreatedNote, core::Error> create_named_note(core::NoteStore& store, std::string_view name,
                                                                std::string_view default_folder,
                                                                std::string_view default_type,
                                                                std::size_t slug_max_length);

MoveResult move_items(core::NoteStore& store, const Listing& listing, std::span<const ItemRef> items,
                      std::string_view dest);

// Hızlı ekleme satırının sonucu (TUI `a`, GUI A).
struct QuickAdd {
    enum class Kind { Nothing, Kept, FolderCreated, FolderExists, NoteCreated };
    Kind kind = Kind::Nothing;
    std::string folder = {};                               // klasör sonucunda yolu
    std::optional<core::CreatedNote> note = std::nullopt;  // NoteCreated
    std::string message = {};                              // klasör sonucunun iletisi; yol güvenilmez
};
// Satırın metni baştaki ve sondaki boşluktan arınır; boşsa Nothing (satır kapanır). Ad `base` klasörüne göredir
// (arayüzün yeni not klasörü; klasör görünümünde geçerli klasör). `/` ile biten ad klasördür: varsa seçilmek
// üzere FolderExists, yoksa kurulur. Değilse boş not (create_named_note); `before_note` false dönerse (kirli
// tampon kaydedilemedi) Kept: satır açık kalır. Baştaki `/` hatadır: yol notların köküne görelidir.
std::expected<QuickAdd, core::Error> quick_add(core::NoteStore& store, const core::FolderTree& tree,
                                               std::string_view typed, std::string_view base,
                                               std::string_view default_type, std::size_t slug_max_length,
                                               const std::function<bool()>& before_note = {});

}  // namespace noteapp::app
