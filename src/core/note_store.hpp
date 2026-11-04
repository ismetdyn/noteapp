#pragma once

#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/text.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::core {

struct Config;

// Bir notun gövdesini yazan işlemin kimliği (cihaz, seq): "okuduğumdan beri değişti mi" sorusunun cevabı.
// Zaman damgası değildir, saat çözünürlüğü ve kayması onu etkilemez. Sıfır değer: gövde hiç yazılmamış.
struct Revision {
    std::array<std::byte, 16> device{};
    std::uint64_t seq = 0;
    auto operator<=>(const Revision&) const = default;
};

struct NoteInfo {
    NotePath path;
    std::filesystem::file_time_type mtime;  // Note.updated_at: sıralama ve gösterim içindir
    std::uintmax_t size{};
    Revision revision{};
    std::optional<std::filesystem::file_time_type> created_at = std::nullopt;
    std::optional<std::string> origin = std::nullopt;
    std::uint64_t local_seq = 0;          // bu cihazın nota son yazmasının seq'i; 0: yazmadı
    std::uint64_t created_local_seq = 0;  // not bu cihazda oluştuysa oluşturan seq; 0: uzaktan geldi
    std::vector<std::string> tags = {};   // TAGGED kenarlarının etiket anahtarları (tag_key), sıralı
};

// Notun senkron durumu. Değeri sync::fill_sync_status hesaplar; core git bilmez.
enum class SyncState { Synced, Modified, New, ConflictCopy, SyncOff };

// Sıralama/süzme için not üst verisi.
struct NoteMeta {
    NotePath path;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type updated_at{};
    std::optional<std::filesystem::file_time_type> created_at = std::nullopt;
    std::optional<std::string> origin = std::nullopt;
    SyncState sync_status = SyncState::SyncOff;
    std::uint64_t local_seq = 0;
    std::uint64_t created_local_seq = 0;
    std::vector<std::string> tags = {};  // NoteInfo::tags
};

// NoteInfo'nun süzme/sıralama görünümü; sync_status SyncOff kalır (sync::fill_sync_status doldurur).
NoteMeta meta_of(NoteInfo info);

// Çöpteki not. path, notun çöpe atıldığı yoldur; canlı yollarla ayrı ad uzayındadır.
struct TrashItem {
    NotePath path;
    std::filesystem::file_time_type trashed_at;  // Note.deleted_at
    std::uintmax_t size{};
    std::optional<std::filesystem::file_time_type> created_at = std::nullopt;
    std::optional<std::string> origin = std::nullopt;
    // Notun düğüm kimliği (hex): aynı çöp yoluna sonradan gelen başka nottan ayırır. trashed_at ms
    // çözünürlüğündedir, aynı milisaniyede atılan iki not onunla ayrılamaz.
    std::string id = {};
};

// İlk 8 KB'ta NUL baytı varsa ikili: önizleme/arama/düzenleme yapılmaz. Notu zaten okuyan yer (önizleme,
// içerik indeksi, show/export) çağırır; list() gövde yüklemez.
bool looks_binary(std::string_view bytes);

// Notun `#etiket`leri (SPEC-graph "Etiketler"): yazıldığı haliyle, anahtarı aynı olanların ilki, anahtarın
// bayt sırasıyla. Yalnız düzyazı notunda (md, markdown, txt, uzantısız); ikili notta boş. Yazma ve ön yüzler
// aynı kuralı kullansın diye tek yer burasıdır.
std::vector<std::string> note_tags(std::string_view path, std::string_view body);
// Eşleştirme anahtarı: küçük harf (Latin-1 ve Latin Extended-A; İ ve I -> i). Tag.name budur.
std::string tag_key(std::string_view tag);

// `note tags` satırı: canlı notu olan etiket.
struct TagCount {
    std::string name;     // tag_key
    std::string display;  // ilk yazıldığı hali
    std::size_t count = 0;
};

struct NewNote {
    std::string folder;  // "" = kök
    std::string name;    // "" = içeriğin ilk dolu satırından slug
    std::string type;    // uzantı, başında nokta olmadan
    std::string content;
    bool exact_name = false;  // ad doluysa `-2` aranmaz, AlreadyExists döner
    // Dolu: Note.created_at bu andır (dosyadan içe almada dosyanın doğumu); boş: oluşturma anı (`now`).
    std::optional<std::chrono::system_clock::time_point> created_at = std::nullopt;
};

// Okuma anındaki revizyon, sonraki write'ın beklediği değerdir (dışarıdan değişiklik tespiti).
struct NoteContent {
    std::string text;
    Revision revision;
    std::filesystem::file_time_type mtime;
};

struct CreatedNote {
    NotePath path;
    Revision revision;
};

// Geçişin tek notu. Kimlik yoldan türetilir: iki cihaz aynı notu aynı düğüme getirir. deleted_at dolu not
// çöpe gider.
struct ImportedNote {
    NotePath path;
    std::string bytes;
    std::optional<std::chrono::system_clock::time_point> updated_at = std::nullopt;
    std::optional<std::chrono::system_clock::time_point> created_at = std::nullopt;
    std::optional<std::chrono::system_clock::time_point> deleted_at = std::nullopt;
};

// Bir notun grafta komşuları (SPEC-graph "Bağlantılar"); yollar sıralı, çöp ve ulaşılamayan not yok.
struct NoteLinks {
    std::vector<NotePath> outgoing;      // LINKS_TO çıkan, ayrıca metinde olup kenarı henüz kurulmamış hedef
    std::vector<NotePath> incoming;      // LINKS_TO gelen
    std::vector<NotePath> related;       // RELATED_TO, iki yönde
    std::vector<std::string> missing;    // canlı nota çözülemeyen [[hedef]]; ham metin, güvenilmez
    std::vector<std::string> tags = {};  // TAGGED: etiketin ilk yazılışı, anahtar sırası; güvenilmez
    bool empty() const {
        return outgoing.empty() && incoming.empty() && related.empty() && missing.empty() && tags.empty();
    }
};

// Canlı iki not arasındaki kenar (GUI haritası). RelatedTo yönsüzdür: from, to'dan küçüktür (yol sırası).
enum class EdgeKind { LinksTo, RelatedTo };
struct NoteEdge {
    NotePath from;
    NotePath to;
    EdgeKind kind = EdgeKind::LinksTo;
    auto operator<=>(const NoteEdge&) const = default;
};

// `[[` ile `]]` arasındaki ham metnin çözüm anahtarı: "|takma ad" ve "#başlık" atılır, kenar boşlukları
// kırpılır. Satır sonlu hedef bağlantı değildir: boş döner. Uzunluk sınırı yok (L3). NoteLinks::missing bu
// anahtarları taşır.
std::string_view link_target(std::string_view raw);

// Panelin düz satırları: TUI ve GUI aynı sırada çizer (gelen, giden, ilişkili, eksik). Boş grubun satırı yok.
enum class LinkGroup { Incoming, Outgoing, Related, Missing };
struct LinkLine {
    LinkGroup group = LinkGroup::Incoming;
    bool is_header = false;
    std::size_t index = 0;  // grubun içinde; başlıkta grubun öğe sayısı
};
std::vector<LinkLine> link_lines(const NoteLinks& links);
// Satırın notu; başlık ve eksik hedefte nullptr.
const NotePath* link_path(const NoteLinks& links, const LinkLine& line);
// Satırın ham metni (güvenilmez): yol ya da eksik hedef. Başlıkta boş.
std::string link_text(const NoteLinks& links, const LinkLine& line);
// Başlığın katalog anahtarı, {count} alır.
std::string_view link_group_key(LinkGroup group);

// Veride duran ama güvenli yola çıkamayan not: adı geçersiz, klasörü geçersiz ya da yolu sınırdan uzun, ekli
// adı dosya adı sınırını aşıyor.
struct UnreachableNote {
    std::string id;      // 32 küçük hex
    std::string name;    // ham ad; terminale temizlenmeden yazılmaz
    std::string reason;  // invalid_name, invalid_folder, too_many_collisions
};

struct ImportOutcome {
    std::size_t imported = 0;
    std::size_t unchanged = 0;              // aynı baytlar ya da aynı geçiş tabanı zaten var
    std::vector<NotePath> conflict_copies;  // grafta başka içerik vardı: dosyanın sürümü kopya oldu
};

// Notlar <dizin>/graph.db içindedir (SPEC-graph); dosya ağacı yoktur, yol IN_FOLDER ve CHILD_OF'tan
// türetilir. Kopyalar aynı bağlantıyı paylaşır, thread-safe'tir. Hata durumunda istisna fırlatmaz.
class NoteStore {
public:
    // Veritabanı burada açılmaz: ilk yazma kurar, okuma yoksa boş görür.
    explicit NoteStore(std::filesystem::path directory, std::size_t slug_max_length = kDefaultSlugMaxLength);
    // Uygulamanın deposu: data_dir()/graph.db. notes_dir'de geçirilmemiş eski not varsa yazmayı reddeder.
    static NoteStore for_config(const Config& config);

    std::vector<NoteInfo> list() const;        // mtime azalan, eşitlikte yol artan
    std::vector<std::string> folders() const;  // göreli, '/' ayırıcılı, sıralı

    // note graph export bunları kimlik ve baytlarıyla verir; export --all raporlar.
    std::vector<UnreachableNote> unreachable() const;

    // Depo kullanılamıyorsa nedeni: geçiş gerekli, daha yeni şema, bozuk dosya. list() hata taşımaz; ön yüz
    // bunu açılışta gösterir, yoksa kullanıcı boş listeyi notların kaybı sanar.
    std::optional<Error> health() const;
    // Başka bağlantının (senkron süreci, CLI) son yazmasında değişen sayaç (graph::Graph::data_version).
    // Listesi aynı kalan değişikliği (yalnız ilişki) yeniden taramanın görmesi içindir; bu deponun kendi
    // yazmaları değiştirmez. Depo yoksa ya da okunamıyorsa 0.
    std::uint64_t data_version() const;

    std::expected<NoteContent, Error> read(const NotePath& path) const;
    // Çöpteki notun baytları; item TrashItem::path. Dışa aktarma içindir.
    std::expected<NoteContent, Error> read_trash(const NotePath& item) const;
    std::expected<NotePath, Error>
    create(const NewNote& note, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    // Editör kaydı içindir: create() gibi ad seçer ama baytlara dokunmaz (CRLF, BOM kalır).
    std::expected<CreatedNote, Error>
    create_exact(const NewNote& note,
                 std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    // Başarıda yeni revizyonu döner. Revizyon expected'dan farklıysa ModifiedExternally; not değişmez.
    // Baytlar aynıysa işlem yazılmaz (günlük büyümez), revizyon aynı kalır.
    std::expected<Revision, Error>
    write(const NotePath& path, std::string_view content, const Revision& expected,
          std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    // Dosya yolu isteyen araç (dış editör) için: notu özel geçici dosyaya (0600) çıkarır, tool'u çalıştırır,
    // baytlar değiştiyse çakışma denetimiyle geri yazar, dosyayı siler. Dönüş: not değişti mi. Kayıt
    // başarısızsa dosya kalır, hata yolunu taşır (error.edit_kept). tool hatası, dosya işlendikten sonra
    // döner.
    std::expected<bool, Error>
    edit_in_temp_file(const NotePath& path,
                      const std::function<std::expected<void, Error>(const std::filesystem::path&)>& tool);

    // Taşı/yeniden adlandır; hedef klasörler oluşur, uzantı değişimi tip değişimidir. Hedef varsa
    // AlreadyExists (asla ezilmez). Başarıda hedef yolu döner.
    std::expected<NotePath, Error> move(const NotePath& from, const NotePath& to);
    // Notu kalıcı siler; onay ön yüzün işidir.
    std::expected<void, Error> remove(const NotePath& path);

    // Çöp kutusu: Note.deleted_at, senkronla cihazlar arası taşınır. Baytlara dokunmaz. Çöpte yol doluysa
    // "-2"...
    std::expected<TrashItem, Error> trash(const NotePath& path);
    std::expected<std::vector<TrashItem>, Error> list_trash() const;  // atılma azalan, eşitlikte yol artan
    // item: TrashItem::path. Canlı ağaçta ad doluysa "-2", "-3"... (asla ezilmez). Başarıda canlı yol döner.
    std::expected<NotePath, Error> restore(const NotePath& item);
    std::expected<void, Error> purge(const NotePath& item);  // kalıcı siler
    std::expected<void, Error> empty_trash();

    // list() + üst veri; sync_status SyncOff'tur, sync::fill_sync_status doldurur.
    std::vector<NoteMeta> list_meta() const;

    // Canlı notu olan etiketler, ada göre; çöpteki notun etiketi sayılmaz.
    std::vector<TagCount> tags() const;

    // Notun komşuları. Tek okuma işlemi: gezinmede değil, önizleme gibi ertelenmiş yüklemede çağrılır.
    std::expected<NoteLinks, Error> links(const NotePath& path) const;
    // Tüm canlı notlar arası LINKS_TO ve RELATED_TO kenarları, tek okuma işleminde; sıralı, tekil. Çöp,
    // ulaşılamayan not, eksik hedef ve kendine kenar yoktur. Metinde olup kenarı henüz kurulmamış hedef de
    // yoktur (links()'ten farkı): harita kenar başına gövde okumaz. Depo yoksa boş.
    std::expected<std::vector<NoteEdge>, Error> edges() const;
    // [[hedef]] metnini yazmanın kuralıyla (yol, uzantısız yol, dosya adı, yalın ad) canlı nota çözer;
    // "|takma ad" ve "#başlık" atılır. Çözülemezse ya da depo açılamadıysa nullopt.
    std::optional<NotePath> resolve_link(std::string_view target) const;
    // Kullanıcının kurduğu RELATED_TO kenarı. Çözülen yönsüzdür: iki yönde de aynı ilişkidir. Kendine ilişki
    // InvalidPath, var olan AlreadyExists, not yoksa NotFound.
    std::expected<void, Error> add_relation(const NotePath& from, const NotePath& to);
    // İki yöndeki kenarları siler; ilişki yoksa NotFound.
    std::expected<void, Error> remove_relation(const NotePath& first, const NotePath& second);

    // Klasör adları NotePath kurallarıyla doğrulanır. Boş klasör bir Folder düğümüdür.
    std::expected<void, Error> create_folder(std::string_view folder);
    std::expected<void, Error> rename_folder(std::string_view from, std::string_view to);
    // Alt ağacı boş klasörleriyle kopyalar, baytlar aynen, uzantısız not da. Hedef doluysa AlreadyExists,
    // kendi içine ise InvalidPath, kaynak yoksa NotFound; ezme ve birleşme yok. Toplu işlem: not başına ayrı
    // yazma binlerce notta saniyeler sürerdi. Çerçeve sınırında bölünür, işlemler arası değişiklik yol
    // sırasıyla izlenir. İlk işlemdeki hata hiçbir şey yazmaz. Sonraki işlemdeki hata geri alınmaz:
    // yazılanlar yerinde kalır, kaynak değişmez, PartialCopy ({path}, {count} = kopyalanan not) döner.
    std::expected<void, Error> copy_folder(std::string_view from, std::string_view to);
    // Not içeren klasör force olmadan NotEmpty ({count} = not sayısı) verir ve hiçbir şey silinmez; force
    // özyinelemeli siler.
    std::expected<void, Error> remove_folder(std::string_view folder, bool force);

    // Toplu içe aktarma (geçiş, perf kurulumu): tek işlem. Kimlik yoldan türetildiğinden tekrar etkisizdir.
    std::expected<ImportOutcome, Error> import_notes(std::span<const ImportedNote> notes);
    std::expected<void, Error> import_folders(std::span<const std::string> folders);
    // Tüm notların [[bağlantı]] kenarlarını yeniden kurar: geçişte hedef, kaynaktan sonra gelmiş olabilir.
    std::expected<void, Error> relink_notes();
    // Bu not içe aktarıldı mı: gövde aynı, ya da içe aktarma tabanı aynı (sonradan düzenlenmiş), ya da
    // kopyası var.
    std::expected<bool, Error> verify_imported(const ImportedNote& note) const;

    // note_sync.cpp içindir: senkron, notu core'da bırakarak aynı bağlantıyı kullanır.
    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace noteapp::core
