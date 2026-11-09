#pragma once

#include "app/inspector.hpp"
#include "app/item_ops.hpp"
#include "app/listing.hpp"
#include "app/map_query.hpp"
#include "app/note_map.hpp"
#include "app/notices.hpp"
#include "app/tabs.hpp"
#include "core/file_icons.hpp"
#include "core/folder_tree.hpp"
#include "core/limits.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "edit/editor_buffer.hpp"
#include "edit/input_line.hpp"
#include "edit/key.hpp"
#include "edit/note_find.hpp"
#include "search/content_index.hpp"
#include "search/folder_order.hpp"
#include "search/global_search.hpp"
#include "search/query.hpp"
#include "tui/commands.hpp"
#include "tui/link_panel.hpp"
#include "tui/map_view.hpp"
#include "tui/pane_layout.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

using core::folder_of;
using core::FolderNode;
using core::FolderTree;
using core::in_folder;
using core::join_folder;
using core::last_component;
using core::parent_folder;

// Modelin dış dünyadan istediği iş; yan etkileri (editör, diyalog, çıkış) çağıran yürütür.
enum class Intent {
    None,
    Quit,
    OpenSelected,     // harici editör (`E`)
    CreateFromQuery,  // eşleşme yokken Enter: sorgu yeni notun ilk satırı olur
    NewNote,
    NewRow,        // `a`: listenin başında ad satırı
    NewFolderRow,  // `folder.new`: aynı ad satırı, klasör adı için hazır
    MoveSelected,  // Ctrl-R: ad/klasör/tip diyaloğu
    DeleteSelected,
    SyncNow,
    ShowHelp,
    ShowSettings,
    EnterEditor,       // `e`/Enter: seçili not sağ panelde düzenlenir
    EditorKey,         // Editör odağı: tuş EditorSession'a gider
    CopyNote,          // `c`: notun özgün baytları sistem panosuna
    TrashSelected,     // `d`: onaysız çöpe; `u` geri alır
    RestoreLast,       // `u`: bu oturumda son çöpe atılan
    RestoreSelected,   // çöp görünümünde `u`
    EmptyTrash,        // çöp görünümünde `E`: onaylı
    RenameSelected,    // `F2`: altta tek satırlık yol girdisi
    ToggleTrash,       // `T`, çöp görünümünde `q` `Esc`
    SavePrefs,         // sıralama ya da sağ sütun değişti
    Paste,             // `p`: bekleyen yank geçerli klasöre
    MoveTo,            // `m`: altta hedef klasör girdisi
    LinkNote,          // `L`: altta ilişkilendirilecek not girdisi
    UnlinkSelected,    // `L` ilişkili satırda: onaylı kaldırma
    RenameRelation,    // `n` ilişkili satırda: altta ad girdisi (o ilişkinin kimliğiyle)
    FlipRelation,      // `f` ilişkili satırda: yön çevrilir (yönsüz: bu nottan çıkan olur)
    UndirectRelation,  // `u` ilişkili satırda: yönsüz yapılır
    LinkMissing,       // bağlantı panelinde gidilecek notu olmayan satır: nedeni söylenir
    ToggleIcons,       // palet: `icons` ayarını çevirir, config.toml'a yazar
    CheckUpdates,      // palet `update`: güncelleme penceresi (app::UpdateController; main yürütür)
    ExportPdf,         // REVIEW `P`: altta çıktı yolu girdisi
    FollowLink,        // REVIEW'da seçili [[bağlantı]]: depo çözer, hedef not açılır
    // Sekmeler (SPEC-tui "Sekmeler"): yürütme main'de, sekmeler EditorTabs'tadır.
    TabNew,             // Ctrl+T: yeni sekmede boş taslak
    TabClose,           // Ctrl+W (editör): etkin sekme kaydedilmemiş akışıyla kapanır
    TabNext,            // Alt+N, Ctrl+PgDn
    TabPrevious,        // Alt+P, Ctrl+PgUp
    TabOpenSelected,    // satır menüsü / palet: seçili not yeni sekmede
    ParkEditor,         // Ctrl+K (editör): sekme açık kalır, arama girdisine geçilir
    OpenMapNode,        // haritada Tab ile seçilen not açılır (map_selected)
    MapEdit,            // haritada kenar kur ya da kaldır (take_map_edit; app::map_relations)
    MapNotice,          // haritanın sonucu alt satıra (take_map_notice): en kısa yol, bütün çizgede açma
    MapRelationFilter,  // haritada `N`: ilişki adı süzgeci; adlar depodan (begin_map_relation_filter)
    TrashMapNode,       // haritada `d`: hedef not seçilip onaysız çöpe (işaretler değil); `u` geri alır
    RunMapQuery,  // haritada `Q` satırında Enter: map_query_text() çalışır (app::MapQuery; yazmada onay)
    ImportFiles,  // palet `import`: altta dosya sistemi yolu girdisi, hedef app::new_note_folder
    SaveView,     // palet `view.save`, haritada `K`: gösterilen okumanın sorgusu adla kayıtlı görünüm olur
    RemoveView,   // palet `view.remove`: altta görünüm adı (Tab adlar), Enter iki kez: siler
    // Özellik denetçisi (F4; app::Inspector): satırlar depodan, yazma alt satırdaki girdiyle.
    InspectorLoad,    // `p`: konu seçildi, satırlar okunur
    InspectorAdd,     // denetçide `a`: anahtar satırı (ya da #etiket), sonra değer satırı
    InspectorEdit,    // denetçide `e` / Enter: seçili özelliğin değer satırı, düzenleme metniyle dolu
    InspectorDelete,  // denetçide `d`: onaylı silme (özellik ya da etiket)
};

// Çoklu seçimin ya da yank'in bir öğesi (GUI ile ortak: app/item_ops.hpp).
using ListItem = app::ItemRef;

// `y`: `p`ye dek bekler; kaynaklar o anda okunur, kaybolan öğe yapıştırmada raporlanır.
struct PendingYank {
    std::vector<ListItem> items;
};

using Mode = app::SearchMode;

// Tuşların kime gideceğini belirler: harfler aynı anda hem süzüp hem eylem yapamaz (SPEC-tui "Tuşlar").
enum class Focus { List, Search, Preview, PreviewFind, Editor, SortMenu, Links, Inspector };

// Listede sağ sütunun gösterdiği üst veri (`v`; GUI Ayrıntılar ile aynı). Sıra `v` döngüsüdür.
using app::ListColumn;

// Kalıcı liste tercihleri (`tui-state.toml`): GUI ile ortak alanlar app::UiPrefs'te, ek olarak `<` `>` ya da
// sürüklemeyle seçilen liste genişliği (0..1).
struct ListPrefs : app::UiPrefs {
    std::optional<double> list_share = {};
};

// Listedeki bir satır. Ad modunda `positions` (yol içindeki bayt ofsetleri) vurgu içindir; içerik modunda
// `hit_count`/`hits` doludur.
struct Row {
    core::NotePath path;
    std::vector<std::size_t> positions = {};
    std::size_t hit_count = 0;
    std::vector<search::Hit> hits = {};
    bool show_best_line = false;  // `f`: içerik eşleşmesinin en iyi satırı yolun altında
    bool is_folder = false;       // klasör görünümü: `path` klasörün yoludur
    std::string label = {};       // klasör görünümünde gösterilen ad; boşsa tam yol
    std::size_t depth = 0;        // ağaçta girinti: geçerli klasörün çocukları 0
    std::size_t number = 0;       // gösterilen numara: kendi klasörü içinde, düz listede sıra
    std::optional<search::RelationReason> reason = std::nullopt;  // yalnız ilişkiyle bulunan satırın nedeni

    std::string shown_text() const { return label.empty() ? path.string() : label; }
};

// REVIEW'da fare seçiminin bir ucu: `preview_lines` satırı ve o satırdaki hücrenin bayt aralığı. İşaretçinin
// altındaki karakter seçime girer: yön ne olursa olsun bir uç `begin`, öbürü `end` kullanır.
struct PreviewPoint {
    std::size_t line = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};
// Sıralı aralık: [begin_line:begin_offset, end_line:end_offset).
struct PreviewRange {
    std::size_t begin_line = 0;
    std::size_t begin_offset = 0;
    std::size_t end_line = 0;
    std::size_t end_offset = 0;
};

// Tek tanım src/app'tedir (app/listing.hpp); bu adlar mevcut çağıranlar ve testler için kalır.
using app::ContentSearcher;
using app::GlobalSearcher;
using app::same_listing;
using app::same_scan;
using app::scan_store;
using app::StoreScan;
using app::view_names;

// Sağ panelde mi çalışılıyor (yakınlaştırmada hangi panelin gösterileceği buna bağlı).
bool right_pane_focused(Focus focus);

// Ctrl-Z editörde geri almadır; başka odakta terminale kalır (askıya alma).
bool passes_to_terminal(Key key, Focus focus);
// Modal açıkken Ctrl-C çıkar; yardım editörün üstünde açıksa çıkmaz (kaydedilmemiş metin).
bool quits_from_modal(Key key, Focus focus);

class TuiState {
public:
    // Önizleme sayfa kaydırma adımı (satır).
    static constexpr std::size_t kPreviewPageLines = 10;
    // Panel dışındaki satırlar: üst çubuk, panel çerçevesi (2), toplamlar ya da editör durumu, kısayollar.
    static constexpr int kChromeLines = 5;
    // Panelin satır sayısı: `PgUp` `PgDn` adımı, REVIEW'da yarım sayfa ve hızlanma sınırı buna göredir.
    std::size_t page_rows() const;

    void set_notes(std::vector<core::NoteInfo> notes);
    void set_content_searcher(ContentSearcher searcher) { listing_.set_content_searcher(std::move(searcher)); }
    void set_global_searcher(GlobalSearcher searcher) { listing_.set_global_searcher(std::move(searcher)); }
    // `view:` süzgecinin notları ve "Run view" listesi depodan gelir (model G/Ç yapmaz). Bağlanmazsa `view:`
    // hiçbir notu geçirmez.
    void set_view_lookup(search::ViewLookup views) { listing_.set_view_lookup(std::move(views)); }
    void set_fuzzy_candidate_chars(std::size_t chars) { listing_.set_fuzzy_candidate_chars(chars); }
    // İlişki araması (SPEC-search): kenarlar depodan arama başına bir kez, derinlik config'ten.
    void set_edge_source(app::EdgeSource source) { listing_.set_edge_source(std::move(source)); }
    // `prop:` süzgecinin notları (app::search_properties); bağlanmazsa `prop:` hiçbir notu geçirmez.
    void set_property_source(search::PropertyLookup source) {
        listing_.set_property_source(std::move(source));
    }
    // Listenin depo bağları GUI ile tek yoldan (app::connect_store); depo okumaları `store_mutex` altında.
    void connect_store(const core::NoteStore& store, const core::Config& config, app::ContentIndexSource index,
                       std::mutex& store_mutex) {
        app::connect_store(listing_, store, config, std::move(index), &store_mutex);
    }
    // Ayarlar kaydında da çağrılır: açık arama yeni derinlikle yeniden süzülür.
    void set_relation_depth(std::optional<std::size_t> depth);
    void set_views(std::vector<std::string> names);
    const std::vector<std::string>& views() const { return views_; }
    // İşçiden gelen senkron durumları (UI iş parçacığında): yeniden taramada da korunur. Değiştiyse true.
    bool set_sync_states(std::map<core::NotePath, core::SyncState> states);
    std::span<const core::NoteMeta> metas() const { return listing_.metas(); }
    // Boş klasörler de görünsün diye deponun klasör listesi; notlardan türeyenlerle birleşir.
    void set_folders(std::vector<std::string> folders);

    // Klasör görünümü: süzgeç ve çöp yokken liste geçerli klasörün çocuklarıdır (önce klasörler).
    bool in_folder_view() const { return !trash_view_ && query().empty() && mode_ != Mode::Global; }
    const std::string& current_folder() const { return current_folder_; }
    const FolderTree& folder_tree() const { return listing_.tree(); }
    const app::Listing& listing() const { return listing_; }
    // `view.details`: klasör görünümü yalnız geçerli klasörün çocuklarıdır (GUI Ayrıntılar); `view.tree`
    // ağaca döner. Oturumluktur.
    bool details_view() const { return details_view_; }
    void set_details_view(bool details);

    // --- Harita (`view.map`, SPEC-tui "Harita") ---
    // Liste paneli notları çizge olarak gösterir; aramada kalır (eşleşmeyen söner), çöpte liste görünür.
    // Model ve yerleşim GUI ile ortaktır (app::MapModel). Oturumluktur.
    bool map_view() const { return map_view_ && !trash_view_; }
    void set_map_view(bool map);
    const app::MapModel& map_model() const { return map_; }
    // limits.app.map_nodes: uygulama config'ten verir; verilmezse 0 = sınırsız.
    void set_map_node_limit(core::Limit limit) { map_.set_node_limit(std::move(limit)); }
    // limits.app.map_depth: derinlik tuşlarının üstü (tek hane: 1–9); verilmezse 0 = sınırsız.
    void set_map_depth_limit(core::Limit limit) { map_.set_depth_limit(std::move(limit)); }
    // Taramada değişiklik görüldü (yalnız ilişki değişse de): kenarlar sonraki çizimde okunur.
    void map_edges_changed() { map_.edges_changed(); }
    // Çizimden önce: kenarlar yalnız eskiyince okunur (taramada değişiklik), harita görünür değilken hiç.
    // `center` çevre kapsamının merkezi: etkin sekmenin notu, yoksa seçili not.
    void update_map(const app::MapModel::EdgeReader& read, const std::optional<core::NotePath>& center);
    // Yerleşimin kare başına adımı; hâlâ hareket ediyorsa true (çağıran kare ister). Durunca iş yapmaz.
    bool step_map();
    static constexpr int kMapIterationsPerFrame = 1;
    // Kamera: kaydırılmadıysa harita panele sığdırılmış (`0`).
    MapCamera map_camera() const;
    // Haritanın hücre boyu: liste panelinin içi, ilk satır kapsam satırıdır.
    std::pair<int, int> map_cells() const;
    // Tab ile seçilen not; yoksa ya da haritadan çıktıysa nullopt.
    std::optional<core::NotePath> map_selected() const;
    const std::string& map_selected_key() const { return map_selected_; }

    // --- Haritada ilişki kurma (MAP-R; kural app::map_relations, GUI'nin Shift+sürüklemesi ile aynı) ---
    // `C` (connect) seçili nottan (yoksa gösterilen nottan) başlar; fareyle sağ sürükleme (terminal geçirirse
    // Shift+sol da) düğümden düğüme. Tab/Shift+Tab hedefi seçer (not ya da etiket), Enter kurar: not → not
    // `r` ilişki (ad satırı açılır) / `l` bağlantı sorar, not ↔ etiket etiketi ekler. `d` aradaki kenarı
    // kaldırır (kenara sağ tık o kenarı seçer); ilişkide `n` adını değiştirir, `f` yönünü çevirir (yönsüzü
    // kaynaktan hedefe yönlü yapar), `u` yönsüz yapar. Esc vazgeçer. Diğer harita tuşları sürer.
    struct MapLink {
        app::MapNode source;
        std::optional<app::MapNode> target;
        std::optional<app::MapEdge> edge;                        // kenara sağ tık: kaldırılacak kenar
        bool choosing = false;                                   // not → not: `r` / `l` bekleniyor
        std::optional<std::pair<int, int>> drag = std::nullopt;  // fare sürüklüyor: işaretçinin tuval noktası
        bool path = false;      // `R` (route) kipi: Enter en kısa yolu arar, hedef yalnız not (F2)
        bool directed = false;  // yol kipinde `d`: kenarlar yönünde
    };
    const std::optional<MapLink>& map_link() const { return map_link_; }

    // --- Harita keşfi (F2, SPEC-tui "Harita"; kural app::MapModel, GUI ile ortak) ---
    // Hedef Tab'la seçilen not, yoksa gösterilen not: `x` komşuları aç/kapat (geçerli kenar süzgecinin türü ve
    // adıyla), `X` bir adım daha, `P` sabitle/çöz, `U` hepsini çöz, `R` yol kipi, `d` çöpe, `n` yeni not. `e`
    // kenar türü süzgeci, `#` etiket ve `N` ilişki adı satırı, `/` haritada bul.
    enum class MapInputKind { Find, Tag, Relation, Query };
    const std::optional<MapInputKind>& map_input_kind() const { return map_input_kind_; }

    // --- Sorgu (F3, SPEC-tui "Harita" → "Sorgu"; akış app::MapQuery, GUI çubuğuyla ortak) ---
    // `Q` tek satır açar: ↑ ↓ (Ctrl+P/N) oturumluk geçmiş, Tab kayıtlı görünüm adları, Enter Intent::RunMapQuery.
    // Sonuç haritada vurgudur, `O` liste panelinde tablodur, `Esc` sonucu kaldırır. Depo işi ListActions'tadır.
    app::MapQuery& map_query() { return map_query_; }
    const app::MapQuery& map_query() const { return map_query_; }
    void set_query_limits(const graph::QueryLimits& limits) { map_query_.set_limits(limits); }
    // limits.app.query_cell_bytes (SIZE_MAX: sınırsız): tablo hücresi bundan uzunsa `…` ile kısalır.
    void set_query_cell_bytes(std::size_t bytes) { map_query_.set_cell_bytes(bytes); }
    const std::string& map_query_text() const { return map_query_text_; }  // çalışacak (ya da vazgeçilen) metin
    // ListActions'ın sonucu: Ran → vurgu haritaya, metin boşalır; Failed → satır metin ve hatayla yeniden açılır.
    void map_query_finished();
    void clear_map_query();  // sonuç, hata, vurgu ve etkin görünüm düşer (Esc; notes_dir değişimi)
    void map_query_failed();
    bool map_query_table() const { return map_view() && map_query_.table_shown(); }
    std::size_t map_query_row() const { return map_query_row_; }  // tablonun seçili satırı
    const InputLine& map_input() const { return map_input_; }
    // Intent::MapRelationFilter'ın işi: `names` (NoteStore::relation_names) Tab listesi olur.
    void begin_map_relation_filter(std::span<const core::RelationName> names);
    const std::vector<std::size_t>& map_found() const { return map_found_; }  // bul satırının eşleşmeleri
    std::size_t map_found_at() const { return map_found_at_; }                // gösterilen eşleşme
    struct MapNotice {
        app::NoticeKind kind = app::NoticeKind::Info;
        std::string text;  // yollar güvenilmez: alt satır terminal_safe'ten geçirir
    };
    std::optional<MapNotice> take_map_notice() { return std::exchange(map_notice_, std::nullopt); }
    // `n`'nin satırından not oluştu: haritada kalır, panelin ortasına sabitlenir (GUI'de sağ tıklanan yer).
    void map_note_created(const core::NotePath& path);
    // Intent::MapEdit'in işi; `kind` boşsa iki düğüm arasında kenar yok (ileti). Connect Related'da ad satırı
    // açılır (F1); Rename, Direct yalnız ilişkide (kimliğiyle).
    enum class MapEditAction { Connect, Remove, Rename, Direct };
    struct MapEditRequest {
        app::MapNode from;
        app::MapNode to;
        std::optional<app::MapEdgeKind> kind;
        MapEditAction action = MapEditAction::Connect;
        core::RelationId relation = {};           // kaldırılacak, adı ya da yönü değişecek ilişki (Related)
        std::string name = {};                    // Rename: şimdiki ad, girdinin ön değeri (güvenilmez)
        std::optional<app::MapNode> source = {};  // Direct: yeni kaynak uç; boş = yönsüz
    };
    std::optional<MapEditRequest> take_map_edit() { return std::exchange(map_edit_, std::nullopt); }
    // Kipte hedefle kaynak arasındaki kenar (kenara sağ tıkta o kenar) ve uçları: `d` `n` `f` `u` onu hedefler,
    // harita ipucu ilişkinin adını söyler. Yoksa nullopt.
    struct MapLinkEdge {
        app::MapEdge edge;
        app::MapNode from;  // yönlüde kaynak
        app::MapNode to;
    };
    std::optional<MapLinkEdge> map_link_edge() const;
    // Fare: tuvalin hücresi (liste panelinin içi, kapsam satırının altı). Düğümün satırında iki yanındaki
    // ikişer hücre de sayılır: braille noktası tek hücreden küçüktür, en yakın düğüm kazanır.
    std::optional<std::size_t> map_node_at(int column, int row) const;
    bool begin_map_drag(int column, int row);  // düğümdeyse sürükleme başlar
    void move_map_drag(int column, int row);
    Intent end_map_drag(int column, int row);
    bool map_dragging() const { return map_link_ && map_link_->drag; }
    void open_map_edge(int column, int row);  // kenara sağ tık: o kenar seçilir, `d` kaldırır
    void enter_folder(std::string folder);
    // Ata klasöre dönüşte çıkılan yolun çocuğu seçili kalır; kök üstü yoktur.
    void go_to_folder(std::string folder);
    // Seçimi klasöre taşır: klasörün üstüne gider, o satırı seçer. Süzgeç varsa temizlenir.
    bool select_folder(const std::string& folder);
    // `a` ve `F2` adayları klasöre göreli; kökte ve düz listede tam yol.
    SharedInputSources name_input_sources() const;
    // `a`, `F2`: yazılan ad bu klasöre göredir (düz listede kök).
    std::string name_base_folder() const { return in_folder_view() ? current_folder_ : std::string{}; }
    // `folder` ad satırında name_base_folder'a göre nasıl yazılır: `alt/`; kendisi ya da dışındaysa boş (ad
    // buraya göredir).
    std::string name_prefix_for(std::string_view folder) const;
    // Görünümdeki notun üst verisi (çöpte çöpün); yoksa nullptr.
    const core::NoteMeta* meta_of(const core::NotePath& path) const;
    // Sorgunun bozuk argümanı: metin sayılmaz, satırlar son geçerli sorgunun güncel listedeki sonucudur.
    const std::optional<search::QueryError>& query_error() const { return query_error_; }

    // Çöp görünümü (`T`): liste çöpü gösterir, yalnız gezinme, `u`, `D`, `E`, `T`, `q`, `Esc` çalışır.
    void enter_trash(std::vector<core::TrashItem> items);
    void set_trash(std::vector<core::TrashItem> items);
    void leave_trash();
    // Üst satırdaki gösterge için; değeri çağıran diskten okur (model G/Ç yapmaz).
    void set_trash_count(std::size_t count) { trash_count_ = count; }
    std::size_t trash_count() const { return trash_count_; }
    bool trash_view() const { return trash_view_; }

    // Satır yüksekliği: not + içerik eşleşmesinin en iyi satırı + açık üst veri satırları.
    std::size_t row_height(std::size_t row) const;
    bool is_expanded(const core::NotePath& path) const { return expanded_.contains(path); }
    bool is_expanded(const Row& row) const;
    // Pencerenin `line`. görsel satırındaki not satırı; liste bittiyse rows().size().
    std::size_t row_at(std::size_t visible, std::size_t line) const;
    // Pencerenin `line`. görsel satırı bir notun ilk satırı mı (ayrıntı değil): işaret yalnız orada.
    bool line_starts_row(std::size_t visible, std::size_t line) const;
    void toggle_expanded(std::size_t row);
    // Klasör satırını ağaçta yerinde açar/kapatır; açık küme tercihlerdedir (`tui-state.toml`).
    void toggle_tree(std::size_t row);
    bool is_folder_open(const std::string& folder) const { return prefs_.open_folders.contains(folder); }
    // Yazmadan önce: kalmayan klasörler dosyada birikmesin. Taramada değil, senkron geri getirebilir.
    void prune_open_folders();
    // Numara sütununun genişliği için: yeniden süzmede hesaplanır, kare başına satır yürünmez.
    std::size_t max_row_number() const { return max_row_number_; }
    // İş sayacı: kare başına görünene orantılı kalsın diye test eder, duvar saati yerine.
    struct ListWork {
        std::size_t rebuilds = 0;    // liste kaç kez kuruldu
        std::size_t row_visits = 0;  // çizim ve kaydırma kaç satıra baktı
    };
    const ListWork& list_work() const { return work_; }
    void note_row_drawn() const { ++work_.row_visits; }
    static constexpr std::size_t kMetaLines = 7;  // app::kNoteFields
    static constexpr std::size_t kFolderMetaLines = 4;

    const search::SortSpec& list_sort() const { return prefs_.sort; }
    ListColumn list_column() const { return prefs_.column; }
    const ListPrefs& list_prefs() const { return prefs_; }
    core::IconStyle icon_style() const { return icon_style_; }
    void set_icon_style(core::IconStyle style) { icon_style_ = style; }
    void set_list_prefs(const ListPrefs& prefs);
    // Önizlemenin satır sayısı: kaydırma ofseti son satırın ötesine geçemez.
    void set_preview_line_count(std::size_t lines);

    Intent handle(Key key, std::string_view text = {});
    // REVIEW'da tuş tekrarını ayırt eder; testler kendi saatini verir.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    void set_clock(Clock clock) { clock_ = std::move(clock); }

    // Sorguyu temizleyip notu seçer (yeni kaydedilen not süzgeçte kalmasın); yoksa false.
    bool reveal(const core::NotePath& path);

    // Seçimi nota taşır, odak değişmez (editörde kaydedilen not). Süzgeç notu gizliyorsa süzgeç temizlenir:
    // panelde düzenlenen not listede de görünsün. Yoksa false.
    bool select_note(const core::NotePath& path);

    void enter_editor() { focus_ = Focus::Editor; }
    // Ctrl+K: editör sekmesi açık kalır, odak arama girdisine geçer (GUI Ctrl+K gibi).
    void search_from_editor() { begin_search(Mode::Name); }
    // Açık sekmeler değişti: tercihlerle birlikte yazılır (take_prefs_changed).
    void set_saved_tabs(app::SavedTabs tabs);
    // `n`: boş editör tüm terminali alır (panel yakınlaştırması); liste ve üst çubuk gizlenir. Editör
    // kapanınca normal iki panele Liste odağıyla dönülür.
    void open_new_note_view();
    bool new_note_view() const { return new_note_view_; }
    // Editör kapandı: satır varsa Önizleme o satırdan sürer, yoksa (kaydedilmemiş taslak, silinen not) Liste.
    void leave_editor(std::optional<std::size_t> preview_line);
    // `e`/Enter ile açılan editörün ilk satırı: Önizlemede görünen satır, Listede baş.
    // Biçimli görünümde kaydırma görüntü satırıdır: üstteki bloğun kaynak satırına çevrilir.
    std::size_t editor_start_line() const {
        return focus_ == Focus::Preview ? source_line_at(preview_scroll_) : 0;
    }
    // REVIEW'un görünümü (`R`, oturum boyunca): biçimli mi, ham mı.
    bool review_rendered() const { return review_rendered_; }
    // Gösterilen görünümün görüntü satırı -> kaynak satırı eşlemesi (ham metinde nullptr: ikisi aynıdır).
    // Üretim değiştiyse kaydırma aynı kaynak bloğunda kalacak şekilde çevrilir; önbellekten kopyalanır, ömrü
    // ona bağlanmaz.
    void set_preview_view(const std::vector<std::size_t>* source_lines, std::size_t generation);
    // Önizleme metni (PreviewCache'e ait; önbellekten uzun yaşamaz). Üretim değişince not içi eşleşmeler
    // yenilenir. `partial`: biçimli görünüm parça parça üretiliyor, devamı var. O sürece kaydırma son üretilen
    // satıra kenetlenmez (`G` sona gider); aynı üretimde uzayan metinde eşleşmeler yenilenir, seçim kalır.
    // Biçimli görünümde `styled` ve `wiki_targets` de verilir (Preview'un, aynı ömür): [[bağlantı]] ayraçsız
    // çizildiğinden yeri ve hedefi onlardan okunur. Ham metinde nullptr: bağlantı satırda aranır.
    void set_preview_lines(const std::vector<std::string>* lines, std::size_t generation,
                           bool partial = false, const std::vector<std::vector<StyledSpan>>* styled = nullptr,
                           const std::vector<std::string>* wiki_targets = nullptr);
    // Parça parça üretimin istekleri (PreviewCache okur): gösterilmesi beklenen kaynak satırı (EDIT dönüşü,
    // içerik araması) ve tüm görünümü isteyen not içi arama.
    std::optional<std::size_t> pending_source_line() const { return pending_source_line_; }
    bool wants_whole_preview() const { return !preview_find_.query().empty(); }

    // Seçili notun bağlantıları, önizlemeyle aynı ertelenmiş okumadan; üretim değişmedikçe kopyalanmaz.
    void set_links(const core::NoteLinks& links, std::size_t generation,
                   const std::optional<core::NotePath>& note);
    const LinkPanel& link_panel() const { return links_; }
    // Önizlemeden alınan satır: editörde ve çöpte bölüm yoktur.
    int links_reserved_rows(int pane_rows) const;
    // Bölümün `row`. görünen satırına tıklama: nota gider; eksik hedef ve başlıkta yalnız odaklar.
    void click_link(std::size_t row);

    // --- Özellik denetçisi (F4; model app::Inspector, GUI ile ortak; SPEC-tui "Denetçi") ---
    // `p` REVIEW'da gösterilen notu, bağlantı bölümünün ilişkili satırında o ilişkiyi, haritada `C` kipinin
    // ilişki kenarını ya da Tab'la seçilen notu açar; bölüm sağ panelde bağlantıların yerindedir. Satırları
    // ListActions okur ve yazar (Intent::Inspector*).
    app::Inspector& inspector() { return inspector_; }
    const app::Inspector& inspector() const { return inspector_; }
    void set_inspector_cell_bytes(std::size_t bytes) { inspector_.set_cell_bytes(bytes); }
    int inspector_reserved_rows(int pane_rows) const;
    std::size_t inspector_top(std::size_t visible) const;  // seçili satır görünür kalır
    void scroll_links(int delta);
    // REVIEW'da seçili [[bağlantı]] (Tab/Shift+Tab, tıklama): hedef anahtarı (core::link_target); yoksa
    // nullopt. Seçim not, görünüm ya da metin değişince düşer.
    std::optional<std::string> selected_wiki_target() const;
    std::optional<ByteRange> wiki_selection_on_line(std::size_t line) const;
    // Gösterilen metnin `line` satırındaki bağlantılar (ham ya da biçimli görünüm).
    std::vector<WikiSpan> wiki_spans_on(std::size_t line) const;
    // Gösterilen metnin `line` satırında `offset` baytı bir bağlantının içindeyse onu seçer.
    bool select_wiki_link_at(std::size_t line, std::size_t offset);
    // Bağlantıyla gidilen not seçilir, REVIEW başından sürer (süzgeç gizliyorsa temizlenir). Yoksa false.
    bool open_note_in_review(const core::NotePath& path);

    // Fare.
    void scroll_preview(int delta);
    void move_selection(int delta);
    void click_row(std::size_t row);
    // Tuşlar listeye gider, seçim değişmez (click_row gibi): üst satırdaki çöp göstergesine tıklama.
    void focus_list() { leave_preview(); }
    // Tıklama sonrası: aynı klasör satırına kısa aralıkla ikinci tıklama içeri girer.
    void note_row_click(std::size_t row);
    void focus_preview();
    // Pencerenin ilk liste satırı: seçili satır görünür kalacak şekilde.
    std::size_t list_top(std::size_t visible) const;

    // REVIEW fare seçimi: uçlar mantıksal konumdur, sarma ya da kaydırma seçimi bozmaz.
    void begin_preview_selection(PreviewPoint point, int x, int y);
    void extend_preview_selection(PreviewPoint point, int x, int y);
    // Sürükleme bitti; işaretçi başka hücreye gittiyse true (kopyalanacak metin var).
    bool finish_preview_selection();
    void clear_preview_selection() { preview_selection_.reset(); }
    // Sürüklemeden biten tıklamanın konumu (finish_preview_selection'dan önce okunur); yoksa nullopt.
    std::optional<PreviewPoint> preview_click_point() const {
        return preview_selection_ && !preview_selection_->moved ? std::optional(preview_selection_->anchor)
                                                                : std::nullopt;
    }
    bool preview_selection_dragging() const { return preview_selection_ && preview_selection_->dragging; }
    int preview_selection_x() const { return preview_selection_ ? preview_selection_->x : 0; }
    int preview_selection_y() const { return preview_selection_ ? preview_selection_->y : 0; }
    std::optional<PreviewRange> preview_selection_range() const;
    // Seçimin `line` satırına düşen kısmı; yoksa nullopt.
    std::optional<ByteRange> preview_selection_on_line(std::size_t line, std::size_t line_size) const;
    const std::vector<std::string>* preview_text() const { return preview_text_; }

    // Son çizilen pencere: `<` `>` sınırları ve fare konumları buna göre hesaplanır.
    void set_window(int width, int height);
    int window_width() const { return window_width_; }
    int window_height() const { return window_height_; }
    PaneLayout& layout() { return layout_; }
    const PaneLayout& layout() const { return layout_; }
    // Seçilen liste genişliğini tercihlere alır (değiştiyse yazılır): `<` `>` her basışta, sürükleme bitince.
    void store_list_share();
    const PreviewFind& preview_find() const { return preview_find_; }

    // Seçimi satır numarasına koyar (son satıra kenetlenir): silmeden sonra komşu satırda kalmak için.
    void select_index(std::size_t index);

    const std::string& query() const { return search_line_.text(); }
    const InputLine& search_line() const { return search_line_; }
    // Bellekteki notlardan aday listeleri; notlar değişince ilk istekte yeniden kurulur.
    const SharedInputSources& input_sources() const;
    std::string search_ghost() const { return search_line_.ghost(prefs_.search_history); }
    Mode mode() const { return mode_; }
    Focus focus() const { return focus_; }
    const std::vector<Row>& rows() const { return rows_; }
    std::size_t total() const { return trash_view_ ? trash_metas_.size() : listing_.notes().size(); }
    std::size_t selected() const { return selected_; }
    // Seçili not; klasör satırında nullptr: not isteyen her eylem klasörde kendiliğinden etkisizdir.
    const Row* selected_row() const {
        return rows_.empty() || rows_[selected_].is_folder ? nullptr : &rows_[selected_];
    }
    const Row* selected_folder_row() const {
        return rows_.empty() || !rows_[selected_].is_folder ? nullptr : &rows_[selected_];
    }
    const Row* selected_entry() const { return rows_.empty() ? nullptr : &rows_[selected_]; }
    // Önizlemenin ilk görünen satırı (0 tabanlı).
    std::size_t preview_scroll() const { return preview_scroll_; }
    // Ham REVIEW'da satırların baştan atlanan baytı (yana kaydırma); biçimli görünümde 0.
    std::size_t preview_column() const { return view_source_lines_.empty() ? preview_column_ : 0; }
    // `line`ın çizimde atlanan baytı: preview_column, kod noktası başına geri çekilmiş, satır boyunu geçmez.
    std::size_t preview_skip(std::string_view line) const;
    // Shift+←/→, Shift+tekerlek: panel genişliği kadar bayt; görünen en uzun satırın son baytını geçmez.
    void scroll_preview_columns(int direction);
    // Gösterilen önizleme ham metin mi (yana kaydırılır); biçimli görünüm satırları panele sığar.
    bool preview_scrolls_sideways() const { return preview_text_ && view_source_lines_.empty(); }

    // Çoklu seçim (`x`): klasörler arasında kalır, yeniden taramada kaybolan öğe düşer. Çöpün ayrı kümesi
    // vardır: çöp ve canlı ağaç ayrı ad uzayıdır.
    bool is_marked(const Row& row) const { return marks().contains(row_key(row)); }
    std::size_t marked_count() const { return marks().size(); }
    void clear_marks() { active_marks().clear(); }
    // Eylemin öğeleri: seçim varsa o (seçili klasörün içindekiler tek sayılır), yoksa geçerli satır.
    std::vector<ListItem> action_items() const;
    // `y` ile bekleyen kopya.
    const std::optional<PendingYank>& yank() const { return yank_; }
    void clear_yank() { yank_.reset(); }
    // Liste `g` öneki: sıradaki tuşu bekliyor, alt satır ne beklediğini söyler.
    bool awaiting_prefix() const { return awaiting_prefix_; }
    // Toplu işlemin öğe öğe sonucu: küçük pencere, sıradaki tuş kapatır.
    const std::vector<std::string>& summary() const { return summary_; }
    void show_summary(std::vector<std::string> lines) { summary_ = std::move(lines); }

    // Komut paleti (`:`) ve tek dağıtım tablosu (tui/commands.cpp). Palet odak değiştirmez: açıkken kip
    // etiketi, yakınlaştırma ve komutların çalışacağı görünüm aynı kalır.
    static std::span<const Command> commands();
    static constexpr std::string_view kPaletteKey = ":";
    // limits.app.recent_commands: son kullanılanlar bununla kısalır, yüklenen tercihler de (set_list_prefs).
    void set_recent_commands_limit(core::Limit limit) { recent_commands_limit_ = std::move(limit); }
    const core::Limit& recent_commands_limit() const { return recent_commands_limit_; }
    // limits.tui.search_history ve search_entry_bytes: Enter ile hatırlanan aramalar ve yüklenen geçmiş.
    void set_search_history_limits(SearchHistoryLimits limits) { search_history_limits_ = limits; }
    const SearchHistoryLimits& search_history_limits() const { return search_history_limits_; }
    bool palette_open() const { return palette_.has_value(); }
    // Satırın bağlam menüsü (sağ tık): GUI'nin satır menüsündeki komutlarla sınırlı palet, seçili satır için.
    // Satır seçimde değilse seçim bırakılır: eylem tıklanan satıra uygulanır.
    void open_row_menu();
    // Yalnız palette_open() iken.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- çağıran palette_open() ile denetler
    const InputLine& palette_line() const { return palette_->line; }
    // Süzülmüş sıra: çalışanlar önce, sonra puan, sonra son kullanılan. Her çağrıda durumdan hesaplanır:
    // açıkken gelen yeniden tarama eski kararı bırakmaz.
    std::vector<PaletteRow> palette_rows() const;
    // Seçili komut: sıra değişse de aynı komut; yoksa ilk satır, liste boşsa nullptr.
    const Command* palette_selected() const;
    // Komut şu an çalışamıyorsa nedenin i18n anahtarı; çalışırsa nullptr.
    const char* command_block(const Command& command) const;
    // Çağıran bilir: senkron kapalıyken `sync.now` soluktur.
    void set_sync_enabled(bool enabled) { sync_enabled_ = enabled; }
    // Palet son kullanılanları değiştirdi: çağıran tercihleri yazar.
    // Haritanın oturumu (UiPrefs::map) burada tercihlere alınır: değiştiyse dosya yazılır.
    bool take_prefs_changed();

private:
    struct PaletteState {
        InputLine line{InputKind::Find};
        const Command* selected = nullptr;
        std::span<const std::string_view> only = {};  // boş: tüm komutlar; dolu: bağlam menüsü
    };
    std::optional<Intent> handle_command_key(Key key, std::string_view text);
    std::optional<Intent> handle_map_key(Key key, std::string_view text);
    std::optional<Intent> handle_map_link_key(Key key, std::string_view text);
    void begin_map_link();
    void step_map_target(int delta);
    Intent confirm_map_link();
    Intent request_map_edit(std::optional<app::MapEdgeKind> kind, MapEditAction action);
    Intent request_map_edit(MapEditRequest request);
    Intent handle_map_edge_key(std::string_view text);
    // Hücrenin ortası, yerleşim biriminde.
    std::optional<app::LayoutPoint> map_point(int column, int row) const;
    void pan_map(float dx, float dy);
    void zoom_map(float factor);
    void step_map_selection(int delta);
    // F2 keşif
    std::optional<std::string> map_target() const;  // Tab'la seçilen, yoksa haritadaki gösterilen not
    std::optional<Intent> handle_map_explore_key(std::string_view text);
    std::optional<Intent> handle_map_table_key(Key key);
    Intent handle_map_input(Key key, std::string_view text);
    Intent handle_map_query_key(Key key, std::string_view text);
    void begin_map_input(MapInputKind kind, std::vector<std::string> candidates, std::string text);
    void submit_map_input();
    void refresh_map_found();
    void step_map_found(int delta);
    void center_map_on(std::size_t node);
    void cycle_map_edges();
    Intent toggle_map_pin();
    Intent find_map_path();
    Intent map_notice(app::NoticeKind kind, std::string text);
    std::optional<unsigned> command_context() const;
    const char* needs_block(CommandNeeds needs) const;
    const Command* bound_command(unsigned context, Key key, std::string_view text) const;
    Intent run_bound(const Command& command);
    bool shown_in_palette(const Command& command, unsigned context) const;
    void step_palette(int delta);
    Intent run_palette_selection();
    Intent handle_palette(Key key, std::string_view text);
    void remember_command(std::string_view id);
    // Tablonun çalıştırdığı, başka yerden de çağrılan eylemler.
    Intent enter_selected();
    Intent right_or_open();
    Intent move_dialog_intent() const;
    Intent toggle_row();
    Intent cycle_column();
    Intent set_column(ListColumn column);
    Intent toggle_tree_all();
    void page_list(int direction);
    void begin_sort_menu();
    void select_last();
    void resize_list(int direction);
    void toggle_zoom();
    void focus_links();
    void toggle_links();
    void step_wiki_link(int direction);
    void toggle_review_view();
    void scroll_preview_top();
    void scroll_preview_bottom();
    void scroll_half_page(int direction);
    void go_root();
    void begin_preview_find();
    void find_step(bool forward);

    static std::string row_key(const Row& row);
    const std::set<std::string>& marks() const { return trash_view_ ? trash_marks_ : marks_; }
    std::set<std::string>& active_marks() { return trash_view_ ? trash_marks_ : marks_; }
    void prune_marks();
    void toggle_mark();
    void mark_all();
    void begin_yank();
    Intent handle_prefix(Key key, std::string_view text);
    Intent handle_focused(Key key, std::string_view text);
    void escape_list();

    Intent handle_trash(Key key, std::string_view text);
    Intent intent_if_selected(Intent intent) const;
    Intent intent_if_any_row(Intent intent) const;
    std::vector<Row> rows_in_folder();
    bool open_selected_folder();
    void go_left();
    void select_key(const std::string& key);
    std::vector<Row>::const_iterator find_note_row(const core::NotePath& path) const;
    void set_selected_expanded(bool expanded);
    void toggle_all_expanded();
    void toggle_selected_details();
    void scroll_accelerated(int direction);
    Intent handle_sort_menu(std::string_view text);
    std::vector<Row> filter_rows();
    void notes_changed();
    void begin_search(Mode mode);
    void begin_tag_search();
    static constexpr std::string_view kTagArgument = "tag:";
    void begin_view_search();
    static constexpr std::string_view kViewArgument = "view:";
    Intent search_enter();
    // Liste ve Arama'da ortak tuşlar; ortak olmayan tuşta nullopt.
    std::optional<Intent> handle_shared(Key key);
    Intent handle_list(Key key, std::string_view text);
    Intent handle_preview(Key key);
    Intent handle_preview_find(Key key, std::string_view text);
    Intent handle_links(Key key, std::string_view text);
    Intent open_inspector(app::InspectTarget target);
    Intent inspect_review();  // REVIEW ve bağlantı bölümünde `p`
    Intent inspect_map();     // haritada `p`
    Intent handle_inspector(Key key, std::string_view text);
    Intent handle_links_letter(std::string_view text);
    Intent jump_to_selected_link();
    int pane_rows() const { return window_height_ - kChromeLines; }
    bool handle_layout_letter(std::string_view text);
    void leave_preview();
    void scroll_to_find_match();
    std::optional<Intent> handle_search_line(Key key, std::string_view text);
    Intent remember_query();
    void toggle_content_mode();
    Intent handle_search(Key key);
    void refilter(bool query_changed);
    void number_rows();
    std::size_t max_preview_scroll() const;
    std::size_t source_line_at(std::size_t display_line) const;
    std::size_t display_line_of(std::size_t source_line) const;
    // Kaydırma kaynak satırı olarak geldi (eşleşme, editör dönüşü): görüntü satırına sıradaki eşitlemede
    // iner.
    void scroll_to_source_line(std::size_t line);

    // Notlar, klasörler, senkron durumları ve arama: satırlar buradan kurulur (TUI ve GUI tek kural).
    app::Listing listing_;
    std::vector<std::string> views_;  // kayıtlı görünüm adları, ada göre
    std::optional<search::QueryError> query_error_;
    std::vector<core::NoteMeta> trash_metas_;
    std::size_t trash_count_ = 0;
    bool trash_view_ = false;
    std::set<core::NotePath> expanded_;
    std::set<core::NotePath> expanded_folders_;  // aynı adlı not ve klasör karışmasın
    std::string current_folder_;
    bool showing_folders_ = true;
    bool details_view_ = false;
    bool map_view_ = false;
    app::MapModel map_;
    std::optional<MapCamera> map_camera_;  // boş: sığdırılmış; kaydırma ya da yakınlaştırma doldurur
    std::size_t map_fitted_generation_ = 0;
    std::string map_selected_;                // Tab ile seçilen notun yolu
    std::optional<MapLink> map_link_;         // `C` kipi ya da fareyle sürükleme
    std::optional<MapEditRequest> map_edit_;  // Intent::MapEdit bekleyen iş
    std::optional<MapInputKind> map_input_kind_;    // açık süzgeç ya da bul satırı
    InputLine map_input_{InputKind::Find};          // metnin tamamı tek değerdir (sözcüklere bölünmez)
    std::vector<std::size_t> map_found_;            // bul: eşleşen düğümler, harita sırasıyla
    std::size_t map_found_at_ = 0;                  // bul: gösterilen eşleşme
    std::size_t map_found_version_ = 0;             // eşleşmelerin kurulduğu harita sürümü (MapModel::version)
    std::optional<MapNotice> map_notice_;           // Intent::MapNotice bekleyen ileti
    app::MapQuery map_query_;                       // limitler: set_query_limits (limits.query.*)
    std::string map_query_text_;
    std::vector<std::string> map_query_history_;    // en yeni başta, oturumluk
    std::size_t map_query_row_ = 0;
    std::optional<app::LayoutPoint> map_new_at_;    // `n`: oluşturulacak notun yeri
    std::optional<std::string> saved_spot_;   // düz listeye geçerken klasördeki seçim; dönüşte geri gelir
    mutable SharedInputSources relative_sources_;
    mutable std::string relative_sources_folder_;
    std::optional<std::size_t> last_click_row_;
    std::chrono::steady_clock::time_point last_click_at_{};
    ListPrefs prefs_;
    core::Limit recent_commands_limit_;  // uygulama config'ten verir; verilmezse 0 = sınırsız
    SearchHistoryLimits search_history_limits_;
    InputLine search_line_{InputKind::Search};
    mutable SharedInputSources input_sources_;
    core::IconStyle icon_style_ = core::IconStyle::Off;  // testler ve çizim için tarafsız varsayılan
    Mode mode_ = Mode::Name;
    Focus focus_ = Focus::List;
    std::vector<Row> rows_;
    std::size_t selected_ = 0;
    std::size_t preview_scroll_ = 0;
    std::size_t preview_column_ = 0;
    std::size_t preview_lines_ = static_cast<std::size_t>(-1);  // bilinmiyor: üst sınır yok
    bool preview_partial_ = false;
    const std::vector<std::string>* preview_text_ = nullptr;
    const std::vector<std::vector<StyledSpan>>* preview_styled_ = nullptr;  // yalnız biçimli görünümde
    const std::vector<std::string>* preview_wiki_targets_ = nullptr;
    std::size_t preview_generation_ = 0;
    PreviewFind preview_find_;
    std::optional<PreviewPoint> wiki_link_;  // REVIEW'da seçili [[bağlantı]]: satır ve bayt aralığı
    LinkPanel links_;
    bool links_hidden_ = false;  // `links.toggle`: bu oturumda bölüm gizli
    app::Inspector inspector_;
    Focus inspector_back_ = Focus::Preview;  // Esc dönüşü: REVIEW ya da harita (Liste)
    std::size_t links_generation_ = 0;
    std::optional<core::NotePath> links_note_;
    bool review_rendered_ = true;
    std::vector<std::size_t> view_source_lines_;  // boş: ham metin
    std::size_t view_generation_ = 0;
    std::optional<std::size_t> pending_source_line_;
    Clock clock_ = [] { return std::chrono::steady_clock::now(); };
    std::chrono::steady_clock::time_point last_scroll_key_{};
    int scroll_direction_ = 0;
    int scroll_repeats_ = 0;
    struct PreviewSelection {
        PreviewPoint anchor;
        PreviewPoint head;
        int x = 0;
        int y = 0;
        bool dragging = true;
        bool moved = false;
    };
    std::optional<PreviewSelection> preview_selection_;
    PaneLayout layout_;
    bool new_note_view_ = false;
    bool auto_zoomed_ = false;  // yakınlaştırmayı yeni not görünümü açtı (kullanıcı değil)
    std::set<std::string> marks_;
    std::set<std::string> trash_marks_;
    std::optional<PendingYank> yank_;
    bool awaiting_prefix_ = false;
    std::size_t max_row_number_ = 0;
    mutable ListWork work_;
    std::vector<std::string> summary_;
    std::optional<PaletteState> palette_;
    bool sync_enabled_ = false;
    bool prefs_changed_ = false;
    int window_width_ = 80;
    int window_height_ = 24;
};

}  // namespace noteapp::tui
