#include "tui/tui_state.hpp"

#include "app/map_relations.hpp"
#include "core/i18n.hpp"
#include "search/query.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>

namespace noteapp::tui {

namespace {

// UTF-8 devam baytı (10xxxxxx): Backspace bir kod noktasının tamamını silsin.
bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Kontrol karakterleri sorguya girmez (terminal olaylarından sızabilir).
bool is_printable(std::string_view text) {
    return !text.empty() && static_cast<unsigned char>(text.front()) >= 0x20;
}

// İçerik modunda önizleme ilk eşleşmeye kayar; üstte birkaç satır bağlam kalır.
constexpr std::size_t kHitContextLines = 2;

std::size_t initial_scroll(const Row& row) {
    if (row.hits.empty() || row.hits.front().line == 0)
        return 0;
    const std::size_t line_index = row.hits.front().line - 1;  // Hit::line 1 tabanlı
    return line_index > kHitContextLines ? line_index - kHitContextLines : 0;
}

constexpr int kListColumnCount = static_cast<int>(app::kListColumnNames.size());
// Bu aralıktan sık gelen aynı yön tuşu tekrardır; daha uzun duraklama hızlanmayı sıfırlar.
constexpr auto kKeyRepeatGap = std::chrono::milliseconds(120);
constexpr int kRepeatsPerStep = 8;  // adım her bu kadar tekrarda bir büyür
constexpr int kMaxStepPageDivisor = 3;

// Bu aralıkta aynı klasör satırına ikinci tıklama içeri girer.
constexpr auto kDoubleClickGap = std::chrono::milliseconds(400);

int page_direction(Key key) {
    return key == Key::PageUp ? -1 : 1;
}

// Her odakta çalışan uygulama tuşları: hiçbir panelin içeriğine bağlı değiller.
std::optional<Intent> app_intent(Key key) {
    switch (key) {
    case Key::CtrlC:
        return Intent::Quit;
    case Key::F1:
        return Intent::ShowHelp;
    case Key::F9:
        return Intent::ShowSettings;
    default:
        return std::nullopt;
    }
}

// Klasör satırı adını `/` ile gösterir; ağaçtaki not adı son parçadır, düz aramada etiket yok (tam yol).
Row to_row(app::ListRow&& row) {
    std::string label = row.is_folder ? row.name + "/" : std::move(row.name);
    return Row{.path = std::move(row.path),
               .positions = std::move(row.positions),
               .hit_count = row.hit_count,
               .hits = std::move(row.hits),
               .show_best_line = row.show_best_line,
               .is_folder = row.is_folder,
               .label = std::move(label),
               .depth = row.depth,
               .number = row.number,
               .reason = std::move(row.reason)};
}

}  // namespace

// Satırın seçim kimliği: not yolu `/` ile bitmez, klasör satırıyla karışmaz.
std::string TuiState::row_key(const Row& row) {
    return app::mark_key(row.path.string(), row.is_folder);
}

std::vector<ListItem> TuiState::action_items() const {
    const std::set<std::string>& active = marks();
    std::vector<ListItem> items;
    if (active.empty()) {
        if (const Row* row = selected_entry())
            items.push_back({row->path, row->is_folder});
        return items;
    }
    return app::marked_items(active);
}

void TuiState::prune_marks() {
    app::prune_marks(marks_, listing_);
}

void TuiState::toggle_mark() {
    const Row* row = selected_entry();
    if (!row)
        return;
    std::set<std::string>& active = active_marks();
    if (const std::string key = row_key(*row); !active.erase(key))
        active.insert(key);
    move_selection(1);
}

void TuiState::mark_all() {
    for (const Row& row : rows_)
        active_marks().insert(row_key(row));
}

void TuiState::begin_yank() {
    std::vector<ListItem> items = action_items();
    if (items.empty())
        return;
    yank_ = PendingYank{std::move(items)};
    marks_.clear();
}

// `g` öneki: süre yok; bilinmeyen tuş iptal eder ve başka iş yapmaz. `gg` `gh` `gt` tablodadır.
Intent TuiState::handle_prefix(Key key, std::string_view text) {
    const std::optional<unsigned> context = command_context();
    if (key != Key::Char || !context)
        return Intent::None;
    const Command* command = bound_command(*context, Key::Char, "g" + std::string(text));
    return command ? run_bound(*command) : Intent::None;
}

void TuiState::go_root() {
    if (trash_view_)
        leave_trash();
    search_line_.clear();
    mode_ = Mode::Name;
    go_to_folder({});
}

bool right_pane_focused(Focus focus) {
    return focus == Focus::Preview || focus == Focus::PreviewFind || focus == Focus::Editor ||
           focus == Focus::Links || focus == Focus::Inspector;
}

bool passes_to_terminal(Key key, Focus focus) {
    return key == Key::CtrlZ && focus != Focus::Editor;
}

bool quits_from_modal(Key key, Focus focus) {
    return key == Key::CtrlC && focus != Focus::Editor;
}

void TuiState::set_notes(std::vector<core::NoteInfo> notes) {
    if (listing_.set_notes(std::move(notes)))
        notes_changed();
    refilter(false);  // içerik araması aynı listede de yeniden koşar (indeks değişmiş olabilir)
}

void TuiState::set_relation_depth(std::optional<std::size_t> depth) {
    listing_.set_relation_depth(depth);
    refilter(false);
}

void TuiState::set_views(std::vector<std::string> names) {
    if (names == views_)
        return;
    views_ = std::move(names);
    input_sources_.reset();
}

void TuiState::set_folders(std::vector<std::string> folders) {
    if (!listing_.set_folders(std::move(folders)))
        return;
    prune_marks();
    refilter(false);
}

// Üst veri yeniden kuruldu: tamamlama kaynakları ve gitmiş öğelerin işaretleri eskidi.
void TuiState::notes_changed() {
    map_.edges_changed();  // harita görünürse sonraki çizimde bir kez okunur
    input_sources_.reset();
    relative_sources_.reset();
    prune_marks();
}

bool TuiState::set_sync_states(std::map<core::NotePath, core::SyncState> states) {
    if (!listing_.set_sync_states(std::move(states)))
        return false;
    notes_changed();
    refilter(false);
    return true;
}

const SharedInputSources& TuiState::input_sources() const {
    if (!input_sources_)
        input_sources_ = build_input_sources(listing_.metas(), views_);
    return input_sources_;
}

SharedInputSources TuiState::name_input_sources() const {
    const std::string base = name_base_folder();
    if (base.empty())
        return input_sources();
    if (relative_sources_ && relative_sources_folder_ == base)
        return relative_sources_;
    std::vector<core::NoteMeta> relative;
    for (const core::NoteMeta& meta : listing_.metas()) {
        const std::string path = meta.path.string();
        if (!in_folder(path, base))
            continue;
        core::NoteMeta copy = meta;
        if (auto parsed = core::NotePath::parse(std::string_view(path).substr(base.size() + 1))) {
            copy.path = std::move(*parsed);
            relative.push_back(std::move(copy));
        }
    }
    relative_sources_ = build_input_sources(relative);
    relative_sources_folder_ = base;
    return relative_sources_;
}

// Açık klasörün çocukları kendi satırının altına girer; numara her seviyede yeniden başlar. Liste yalnız
// değişimde kurulur, gezinme buraya gelmez.
std::vector<Row> TuiState::rows_in_folder() {
    current_folder_ = listing_.tree().nearest_existing(std::move(current_folder_));
    // Açık küme tercihlerdedir (`tui-state.toml`); liste yalnız kurarken okur.
    listing_.set_open_folders(prefs_.open_folders);
    std::vector<Row> rows;
    for (app::ListRow& row :
         details_view_ ? listing_.folder_rows(current_folder_) : listing_.tree_rows(current_folder_))
        rows.push_back(to_row(std::move(row)));
    return rows;
}

void TuiState::set_details_view(bool details) {
    details_view_ = details;
    set_map_view(false);
    refilter(false);
}

void TuiState::set_map_view(bool map) {
    map_view_ = map;
    map_link_.reset();
    map_input_kind_.reset();
    map_found_.clear();
    map_new_at_.reset();
}

namespace {

// Harita tuşları (SPEC-tui "Harita"). Adım hücre cinsindendir: kaydırma her boyutta aynı hissettirir.
constexpr float kMapPanCells = 4.0F;
constexpr float kMapZoomStep = 1.25F;
constexpr float kMapMinZoom = 0.0005F;  // 2.000 düğümlük çizge en küçük panelde de sığar
constexpr float kMapMaxZoom = 2.0F;     // birim başına nokta: ideal kenar 200 nokta (100 hücre)
constexpr int kMapStatusRows = 1;       // kapsam satırı
// Fare isabeti: braille noktası hücreden küçük; düğümün iki yanındaki hücre ve aynı satır sayılır, kenar
// çizgisine bu kadar nokta yakın.
constexpr int kMapHitCells = 2;
constexpr float kMapEdgeHitDots = 4.0F;  // bir hücre boyu: çizginin geçtiği hücrenin ortası

bool same_node(const app::MapNode& a, const app::MapNode& b) {
    return a.kind == b.kind && a.key == b.key;
}

std::optional<std::size_t> map_note_index(const app::NoteMap& map, const std::string& key) {
    const auto found = std::ranges::find_if(map.nodes, [&](const app::MapNode& node) {
        return node.kind == app::MapNodeKind::Note && node.key == key;
    });
    return found == map.nodes.end() ? std::nullopt
                                    : std::optional(static_cast<std::size_t>(found - map.nodes.begin()));
}

std::string trimmed(std::string_view text) {
    constexpr std::string_view kBlank = " \t";
    const std::size_t first = text.find_first_not_of(kBlank);
    return first == std::string_view::npos ? std::string()
                                           : std::string(text.substr(first, text.find_last_not_of(kBlank) - first + 1));
}

}  // namespace

void TuiState::update_map(const app::MapModel::EdgeReader& read,
                          const std::optional<core::NotePath>& center) {
    if (!map_view())
        return;  // görünmeyen harita depoyu okumaz
    map_.update(read, listing_.metas(), center);
    if (map_.generation() != map_fitted_generation_) {
        map_fitted_generation_ = map_.generation();
        map_camera_.reset();  // çizge değişti: yeni düğümler görünsün (GUI gibi)
    }
    // Bul açıkken harita değiştiyse eşleşmeler yeni düğüm sırasıyla yeniden kurulur (kamera yerinde kalır).
    if (map_input_kind_ == MapInputKind::Find && map_found_version_ != map_.version()) {
        map_found_version_ = map_.version();
        map_found_ = app::find_map_nodes(map_.map(), map_input_.text());
        map_found_at_ = std::min(map_found_at_, map_found_.empty() ? 0 : map_found_.size() - 1);
    }
    // Arama süzgeci listeyle aynıdır: süzgeçten geçen notlar yanar. Sürüm listenin her kuruluşunda artar.
    map_.refresh_matches(!query().empty(), work_.rebuilds, [this] {
        std::unordered_set<std::string> listed;
        for (const Row& row : rows_)
            if (!row.is_folder)
                listed.insert(row.path.string());
        return listed;
    });
}

bool TuiState::step_map() {
    return map_view() && map_.layout().step(kMapIterationsPerFrame);
}

std::pair<int, int> TuiState::map_cells() const {
    const int width = layout_.geometry(window_width_, false).list_width - kPaneChromeCells;
    return {std::max(width, 1), std::max(pane_rows() - kMapStatusRows, 1)};
}

MapCamera TuiState::map_camera() const {
    if (map_camera_)
        return *map_camera_;
    const auto [columns, rows] = map_cells();
    return fit_map_camera(map_.layout().positions(), columns * kMapDotsPerCellX, rows * kMapDotsPerCellY);
}

void TuiState::pan_map(float dx, float dy) {
    MapCamera camera = map_camera();
    camera.x -= dx * kMapPanCells * kMapDotsPerCellX;
    camera.y -= dy * kMapPanCells * kMapDotsPerCellY;
    map_camera_ = camera;
}

// Panelin ortası yerinde kalır.
void TuiState::zoom_map(float factor) {
    MapCamera camera = map_camera();
    const auto [columns, rows] = map_cells();
    camera.zoom_at(static_cast<float>(columns * kMapDotsPerCellX) / 2.0F,
                   static_cast<float>(rows * kMapDotsPerCellY) / 2.0F, factor, {kMapMinZoom, kMapMaxZoom});
    map_camera_ = camera;
}

std::optional<core::NotePath> TuiState::map_selected() const {
    if (map_selected_.empty())
        return std::nullopt;
    const auto& nodes = map_.map().nodes;
    const bool shown = std::ranges::any_of(nodes, [&](const app::MapNode& node) {
        return node.kind == app::MapNodeKind::Note && node.key == map_selected_;
    });
    if (!shown)
        return std::nullopt;
    auto path = core::NotePath::parse(map_selected_);
    return path ? std::optional(std::move(*path)) : std::nullopt;
}

// Tab yalnız notlar arasında gezer (etiket açılmaz); sıra haritanınkidir, sondan başa döner.
void TuiState::step_map_selection(int delta) {
    std::vector<const std::string*> notes;
    for (const app::MapNode& node : map_.map().nodes)
        if (node.kind == app::MapNodeKind::Note)
            notes.push_back(&node.key);
    if (notes.empty())
        return;
    const auto current =
        std::ranges::find_if(notes, [&](const std::string* key) { return *key == map_selected_; });
    std::size_t index = 0;
    if (current != notes.end())
        index = app::cycled_tab(static_cast<std::size_t>(current - notes.begin()), delta, notes.size());
    else if (delta < 0)
        index = notes.size() - 1;
    map_selected_ = *notes[index];
}

// Harita odaktayken (Liste): oklar kaydırır, `+` `-` yakınlaştırır, `0` sığdırır, Tab/Shift+Tab not seçer,
// Enter seçileni açar, `w` bütün çizge, `t` etiketler, `1`-`9` derinlik (en çok limits.app.map_depth). Diğer
// tuşlar tabloya düşer.
std::optional<Intent> TuiState::handle_map_key(Key key, std::string_view text) {
    if (map_input_kind_)
        return handle_map_input(key, text);
    if (map_link_)
        if (const auto intent = handle_map_link_key(key, text))
            return intent;
    if (map_query_table())
        if (const auto intent = handle_map_table_key(key))
            return intent;
    switch (key) {
    case Key::Escape:
        if (map_query_.outcome() || !map_query_.error().empty()) {  // F3: önce sorgunun sonucu kalkar
            clear_map_query();
            return Intent::None;
        }
        if (!map_.scope().path)
            return std::nullopt;  // liste: süzgeci temizler
        map_.clear_path();
        return Intent::None;
    case Key::Left:
        pan_map(-1.0F, 0.0F);
        return Intent::None;
    case Key::Right:
        pan_map(1.0F, 0.0F);
        return Intent::None;
    case Key::Up:
        pan_map(0.0F, -1.0F);
        return Intent::None;
    case Key::Down:
        pan_map(0.0F, 1.0F);
        return Intent::None;
    case Key::Tab:
        step_map_selection(1);
        return Intent::None;
    case Key::ShiftTab:
        step_map_selection(-1);
        return Intent::None;
    case Key::Enter:
        return map_selected() ? Intent::OpenMapNode : Intent::None;
    case Key::Char:
        break;
    default:
        return std::nullopt;
    }
    if (const auto intent = handle_map_explore_key(text))
        return intent;
    app::MapOptions options = map_.options();
    if (text == "+" || text == "=") {
        zoom_map(kMapZoomStep);
    } else if (text == "-") {
        zoom_map(1.0F / kMapZoomStep);
    } else if (text == "0") {
        map_camera_.reset();
    } else if (text == "w") {
        options.whole_graph = !options.whole_graph;
    } else if (text == "t") {
        options.show_tags = !options.show_tags;
    } else if (text == "C") {
        begin_map_link();
        return Intent::None;
    } else if (text == "p") {  // F4 (properties): liste görünmez, yapıştırma `p`si haritada gölgelenir
        return inspect_map();
    } else if (text.size() == 1 && text[0] >= '0' + app::MapOptions::kMinDepth && text[0] <= '9' &&
               text[0] - '0' <= map_.max_depth()) {
        options.depth = text[0] - '0';
    } else {
        return std::nullopt;
    }
    map_.set_options(options);
    return Intent::None;
}

// Kip açıkken: Esc, Tab, Enter ve `d` (seçimde `r` `l`) burada; başka tuş haritaya ve listeye düşer, seçim
// beklerken yutulur (yanlış harf liste komutu çalıştırmasın).
std::optional<Intent> TuiState::handle_map_link_key(Key key, std::string_view text) {
    MapLink& link = *map_link_;
    switch (key) {
    case Key::Escape:
        map_link_.reset();
        return Intent::None;
    case Key::Tab:
    case Key::ShiftTab:
        if (!link.choosing)
            step_map_target(key == Key::Tab ? 1 : -1);
        return Intent::None;
    case Key::Enter:
        if (link.path)
            return find_map_path();
        return link.choosing ? Intent::None : confirm_map_link();
    case Key::Char:
        break;
    default:
        return std::nullopt;
    }
    if (link.path) {  // yol kipi (F2): `d` yönü açıp kapar, başka harf haritaya düşer
        if (text != "d")
            return std::nullopt;
        link.directed = !link.directed;
        return Intent::None;
    }
    if (link.choosing) {
        if (text == "r")
            return request_map_edit(app::MapEdgeKind::Related, MapEditAction::Connect);
        if (text == "l")
            return request_map_edit(app::MapEdgeKind::Link, MapEditAction::Connect);
        return Intent::None;
    }
    if (text != "d" && text != "n" && text != "f" && text != "u")
        return std::nullopt;
    if (!link.target)
        return Intent::None;
    return handle_map_edge_key(text);
}

std::optional<TuiState::MapLinkEdge> TuiState::map_link_edge() const {
    if (!map_link_ || !map_link_->target)
        return std::nullopt;
    const MapLink& link = *map_link_;
    // Sağ tıklanan kenarın dizini o anki çizgeye aittir: uçlar kipin kopyalarıdır (kaynak = kenarın from'u).
    if (link.edge)
        return MapLinkEdge{*link.edge, link.source, *link.target};
    const app::NoteMap& map = map_.map();
    const app::MapEdge* edge = app::map_edge_between(map, link.source, *link.target);
    if (edge == nullptr)
        return std::nullopt;
    return MapLinkEdge{*edge, map.nodes[edge->from], map.nodes[edge->to]};
}

// `d` kenarı kaldırır; `n` `f` `u` yalnız ilişkide (adı, yönü), başka kenarda yutulur.
Intent TuiState::handle_map_edge_key(std::string_view text) {
    const std::optional<MapLinkEdge> found = map_link_edge();
    if (!found)
        return text == "d" ? request_map_edit(std::nullopt, MapEditAction::Remove) : Intent::None;
    const app::MapEdge& edge = found->edge;
    if (text == "d") {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- map_link_edge hedefi denetledi
        return request_map_edit({map_link_->source, *map_link_->target, edge.kind, MapEditAction::Remove,
                                 edge.relation});
    }
    if (edge.kind != app::MapEdgeKind::Related)
        return Intent::None;
    MapEditRequest request{found->from, found->to, edge.kind, MapEditAction::Direct, edge.relation};
    if (text == "n") {
        request.action = MapEditAction::Rename;
        request.name = edge.name;
    } else if (text == "f") {
        // Yönlü kenarın kaynağı from'dur: çevirince öbür uç; yönsüz kipin kaynağından hedefe yönlenir.
        request.source = edge.directed ? found->to : map_link_->source;
    }
    return request_map_edit(std::move(request));
}

// Kaynak Tab'la seçilen not, yoksa gösterilen not (çevrenin merkezi); haritada yoksa kip açılmaz.
void TuiState::begin_map_link() {
    if (auto source = map_target())
        map_link_ = MapLink{{app::MapNodeKind::Note, std::move(*source)}, std::nullopt, std::nullopt};
}

// Hedef Tab'la seçilen not; yoksa gösterilen not (çevrenin merkezi), haritadaysa.
std::optional<std::string> TuiState::map_target() const {
    if (const auto selected = map_selected())
        return selected->string();
    const std::optional<core::NotePath>& center = map_.center();
    if (!center || !map_note_index(map_.map(), center->string()))
        return std::nullopt;
    return center->string();
}

// Hedef kaynak dışındaki her düğümdür (not ve etiket; yol kipinde yalnız not), haritanın sırasıyla; sondan başa
// döner.
void TuiState::step_map_target(int delta) {
    MapLink& link = *map_link_;
    std::vector<const app::MapNode*> targets;
    for (const app::MapNode& node : map_.map().nodes)
        if (!same_node(node, link.source) && (!link.path || node.kind == app::MapNodeKind::Note))
            targets.push_back(&node);
    if (targets.empty())
        return;
    const auto current =
        link.target ? std::ranges::find_if(
                          targets, [&](const app::MapNode* node) { return same_node(*node, *link.target); })
                    : targets.end();
    std::size_t index = 0;
    if (current != targets.end())
        index = app::cycled_tab(static_cast<std::size_t>(current - targets.begin()), delta, targets.size());
    else if (delta < 0)
        index = targets.size() - 1;
    link.target = *targets[index];
    link.edge.reset();
}

Intent TuiState::confirm_map_link() {
    if (!map_link_ || !map_link_->target)
        return Intent::None;
    switch (app::map_drop(map_link_->source, *map_link_->target)) {
    case app::MapDrop::ChooseNoteEdge:
        map_link_->choosing = true;
        return Intent::None;
    case app::MapDrop::AddTag:
        return request_map_edit(app::MapEdgeKind::Tag, MapEditAction::Connect);
    case app::MapDrop::None:
        break;
    }
    return Intent::None;
}

Intent TuiState::request_map_edit(std::optional<app::MapEdgeKind> kind, MapEditAction action) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- çağıranlar hedefi denetledi
    return request_map_edit({map_link_->source, *map_link_->target, kind, action});
}

Intent TuiState::request_map_edit(MapEditRequest request) {
    map_edit_ = std::move(request);
    map_link_.reset();
    return Intent::MapEdit;
}

std::optional<app::LayoutPoint> TuiState::map_point(int column, int row) const {
    const MapCamera camera = map_camera();
    if (camera.zoom <= 0.0F)
        return std::nullopt;
    const float x = static_cast<float>(column * kMapDotsPerCellX) + kMapDotsPerCellX / 2.0F;
    const float y = static_cast<float>(row * kMapDotsPerCellY) + kMapDotsPerCellY / 2.0F;
    return app::LayoutPoint{(x - camera.x) / camera.zoom, (y - camera.y) / camera.zoom};
}

// Düğümün hücresi çizimdeki gibidir (map_pane: nokta yuvarlanır, hücreye bölünür).
std::optional<std::size_t> TuiState::map_node_at(int column, int row) const {
    const MapCamera camera = map_camera();
    const auto& positions = map_.layout().positions();
    std::optional<std::size_t> best;
    int best_distance = kMapHitCells + 1;
    for (std::size_t i = 0; i < positions.size() && i < map_.map().nodes.size(); ++i) {
        const float dot_x = std::round(camera.x + positions[i].x * camera.zoom);
        const float dot_y = std::round(camera.y + positions[i].y * camera.zoom);
        const auto cell_x = static_cast<int>(std::floor(dot_x / kMapDotsPerCellX));
        const auto cell_y = static_cast<int>(std::floor(dot_y / kMapDotsPerCellY));
        const int distance = std::abs(cell_x - column);
        if (cell_y == row && distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

bool TuiState::begin_map_drag(int column, int row) {
    const auto node = map_node_at(column, row);
    if (!node)
        return false;
    leave_preview();  // tıklama haritaya odaklanır (click_row gibi): seçim tuşları haritaya gelsin
    focus_ = Focus::List;
    map_link_ = MapLink{map_.map().nodes[*node], std::nullopt, std::nullopt};
    map_link_->drag = std::pair(column * kMapDotsPerCellX + 1, row * kMapDotsPerCellY + 2);
    return true;
}

void TuiState::move_map_drag(int column, int row) {
    if (!map_dragging())
        return;
    map_link_->drag = std::pair(column * kMapDotsPerCellX + 1, row * kMapDotsPerCellY + 2);
    const auto node = map_node_at(column, row);
    if (node && !same_node(map_.map().nodes[*node], map_link_->source))
        map_link_->target = map_.map().nodes[*node];
    else
        map_link_->target.reset();
}

// Bırakış: not → not seçim sorar (kip `r` `l` bekler), not ↔ etiket hemen ekler; boşluk ya da aynı düğüm
// vazgeçmektir.
Intent TuiState::end_map_drag(int column, int row) {
    if (!map_link_)
        return Intent::None;
    map_link_->drag.reset();
    const auto node = map_node_at(column, row);
    if (!node || same_node(map_.map().nodes[*node], map_link_->source)) {
        map_link_.reset();
        return Intent::None;
    }
    map_link_->target = map_.map().nodes[*node];
    const Intent intent = confirm_map_link();
    if (map_link_ && !map_link_->choosing)
        map_link_.reset();
    return intent;
}

void TuiState::open_map_edge(int column, int row) {
    map_link_.reset();
    const auto point = map_point(column, row);
    if (!point)
        return;
    const app::NoteMap& map = map_.map();
    const auto edge =
        app::map_edge_at(map, map_.layout().positions(), *point, kMapEdgeHitDots / map_camera().zoom);
    if (!edge)
        return;
    leave_preview();
    focus_ = Focus::List;
    const app::MapEdge& hit = map.edges[*edge];
    map_link_ = MapLink{map.nodes[hit.from], map.nodes[hit.to], hit};
}

// Keşif tuşları (F2, SPEC-tui "Harita"). Harfler İngilizce adlardan: (e)X(pand), e(dges), N(ame), P(in),
// U(npin), R(oute), n(ew), d(elete → çöp); F3 Q(uery), O(utput: tablo), K(eep: sorguyu görünüm olarak).
// Listenin `x` `X` `e` `/` tuşları haritada gölgelenir; liste görünmez.
std::optional<Intent> TuiState::handle_map_explore_key(std::string_view text) {
    if (text == "Q") {  // vazgeçilen ya da hatalı sorgunun metniyle açılır
        begin_map_input(MapInputKind::Query, views_, map_query_text_);
        return Intent::None;
    }
    if (text == "K")
        return Intent::SaveView;
    if (text == "O") {
        if (map_query_.outcome()) {
            map_query_.toggle_table();
            map_query_row_ = 0;
        }
        return Intent::None;
    }
    if (text == "/") {
        begin_map_input(MapInputKind::Find, {}, {});
        return Intent::None;
    }
    if (text == "#") {
        begin_map_input(MapInputKind::Tag, input_sources()->tags, map_.options().tag);
        return Intent::None;
    }
    if (text == "N")
        return Intent::MapRelationFilter;
    if (text == "e") {
        cycle_map_edges();
        return Intent::None;
    }
    if (text == "U") {
        map_.layout().unpin_all();
        return map_notice(app::NoticeKind::Info, core::tr("map.unpinned_all"));
    }
    if (text == "R") {
        begin_map_link();
        if (map_link_)
            map_link_->path = true;
        return Intent::None;
    }
    if (text == "n") {  // var olan ad satırı (quick.add); not, panelin ortasına sabitlenir
        const auto [columns, rows] = map_cells();
        map_new_at_ = map_point(columns / 2, rows / 2);
        return Intent::NewRow;
    }
    if (text == "P")
        return toggle_map_pin();
    if (text == "d") {
        // Yalnız Tab'la açıkça seçilen not çöpe gider: seçim yoksa (süzgeç sildi, ya da `C` kipindeki `d` kenarı
        // kaldırdıktan sonraki ikinci `d`) gösterilen notu sessizce atmak yerine ileti. Seçilen not liste
        // seçimi olur: var olan yol (trash_selected) ve `u`.
        const std::optional<core::NotePath> selected = map_selected();
        if (!selected)
            return map_notice(app::NoticeKind::Info, core::tr("map.trash_needs_selection"));
        return select_note(*selected) ? Intent::TrashMapNode : Intent::None;
    }
    if (text != "x" && text != "X")
        return std::nullopt;
    const std::optional<std::string> target = map_target();
    if (!target)
        return Intent::None;
    if (map_.options().whole_graph)
        return map_notice(app::NoticeKind::Info, core::tr("map.whole_graph_open"));
    const app::MapExpansion spec = app::expansion_for(map_.options());
    if (text == "x")
        map_.toggle_neighbours(*target, spec);
    else
        map_.expand(*target, spec);
    return Intent::None;
}

// Bağlantı ve ilişki → yalnız bağlantı → yalnız ilişki → ikisi (ad süzgeci ayrı: `N`).
void TuiState::cycle_map_edges() {
    app::MapOptions options = map_.options();
    const bool both = options.show_links && options.show_related;
    const bool links_only = options.show_links && !options.show_related;
    options.show_related = !both;
    options.show_links = !links_only;
    map_.set_options(options);
}

Intent TuiState::toggle_map_pin() {
    const std::optional<std::string> target = map_target();
    const auto node = target ? map_note_index(map_.map(), *target) : std::nullopt;
    if (!node)
        return Intent::None;
    app::GraphLayout& layout = map_.layout();
    if (layout.pinned(*node)) {
        layout.unpin(*node);
        return map_notice(app::NoticeKind::Info, core::tr("map.unpinned", {{"path", *target}}));
    }
    layout.pin(*node, layout.positions()[*node]);
    return map_notice(app::NoticeKind::Info, core::tr("map.pinned", {{"path", *target}}));
}

Intent TuiState::find_map_path() {
    const MapLink link = *std::exchange(map_link_, std::nullopt);
    if (!link.target)
        return Intent::None;
    const std::string& from = link.source.key;
    const std::string& to = link.target->key;
    const app::MapPathResult result = map_.find_path(listing_.metas(), from, to, link.directed);
    return map_notice(result.status == app::MapPathResult::Status::Found ? app::NoticeKind::Success
                                                                          : app::NoticeKind::Info,
                      app::map_path_text(result, from, to, link.directed));
}

Intent TuiState::map_notice(app::NoticeKind kind, std::string text) {
    map_notice_ = MapNotice{kind, std::move(text)};
    return Intent::MapNotice;
}

void TuiState::begin_map_relation_filter(std::span<const core::RelationName> names) {
    std::vector<std::string> displays;
    displays.reserve(names.size());
    for (const core::RelationName& name : names)
        displays.push_back(name.display);
    const std::optional<std::string>& current = map_.options().relation;
    // Adsız ilişki adıyla yazılamaz: satırda "(adsız)" metni onu seçer.
    begin_map_input(MapInputKind::Relation, std::move(displays),
                    !current ? std::string() : current->empty() ? core::tr("tui.relation_unnamed") : *current);
}

void TuiState::begin_map_input(MapInputKind kind, std::vector<std::string> candidates, std::string text) {
    map_link_.reset();
    map_input_kind_ = kind;
    map_input_ = InputLine(kind == MapInputKind::Query ? InputKind::Query : InputKind::Find);
    map_input_.set_sources(word_input_sources(std::move(candidates)));
    map_input_.set_text(std::move(text));
    map_found_.clear();
}

// Girdi açıkken her tuş onundur. Bul satırında Enter/Tab sonraki, Shift+Tab önceki eşleşme; süzgeç satırında
// Enter uygular, Tab adları listeler.
Intent TuiState::handle_map_input(Key key, std::string_view text) {
    const bool find = map_input_kind_ == MapInputKind::Find;
    const bool listing = map_input_.popup() != nullptr;
    if (key == Key::Escape && !listing) {
        map_input_kind_.reset();
        map_found_.clear();  // seçim eşleşmede kalır: Enter onu açar
        return Intent::None;
    }
    if (map_input_kind_ == MapInputKind::Query && !listing)
        return handle_map_query_key(key, text);
    if (find && (key == Key::Enter || key == Key::Tab || key == Key::ShiftTab)) {
        step_map_found(key == Key::ShiftTab ? -1 : 1);
        return Intent::None;
    }
    if (!find && key == Key::Enter && !listing) {
        submit_map_input();
        return Intent::None;
    }
    if (map_input_.handle(key, text) == InputLine::Result::Edited && find)
        refresh_map_found();
    return Intent::None;
}

// Sorgu satırı (F3): ↑ ↓ ve Ctrl+P/N geçmişte gezer (arama geçmişinin yardımcısı), Enter çalıştırır. Geçmiş
// oturumluktur; sır benzeri metin girmez.
Intent TuiState::handle_map_query_key(Key key, std::string_view text) {
    if (key == Key::Up || key == Key::Down)
        key = key == Key::Up ? Key::CtrlP : Key::CtrlN;
    if (key != Key::Enter) {
        map_input_.handle(key, text, map_query_history_);
        return Intent::None;
    }
    const std::string typed = trimmed(map_input_.text());
    map_input_kind_.reset();
    if (typed.empty())
        return Intent::None;
    remember_search(map_query_history_, typed, nullptr, search_history_limits_);
    map_query_text_ = typed;
    return Intent::RunMapQuery;
}

void TuiState::submit_map_input() {
    const std::string value = trimmed(map_input_.text());
    app::MapOptions options = map_.options();
    if (map_input_kind_ == MapInputKind::Tag)
        options.tag = value;
    else if (value.empty())
        options.relation.reset();
    else
        options.relation = value == core::tr("tui.relation_unnamed") ? std::string() : value;
    map_input_kind_.reset();
    map_.set_options(options);
}

void TuiState::refresh_map_found() {
    map_found_version_ = map_.version();
    map_found_ = app::find_map_nodes(map_.map(), map_input_.text());
    map_found_at_ = 0;
    if (!map_found_.empty())
        center_map_on(map_found_.front());
}

void TuiState::step_map_found(int delta) {
    if (map_found_.empty())
        return;
    map_found_at_ = app::cycled_tab(map_found_at_, delta, map_found_.size());
    center_map_on(map_found_[map_found_at_]);
}

// Eşleşme panelin ortasına gelir (yakınlık kalır); not ise Tab seçimi olur (Enter açar, `x` `P` `R` onu alır).
void TuiState::center_map_on(std::size_t node) {
    const app::NoteMap& map = map_.map();
    if (node >= map.nodes.size())
        return;
    if (map.nodes[node].kind == app::MapNodeKind::Note)
        map_selected_ = map.nodes[node].key;
    const auto [columns, rows] = map_cells();
    MapCamera camera = map_camera();
    const app::LayoutPoint at = map_.layout().positions()[node];
    camera.x = static_cast<float>(columns * kMapDotsPerCellX) / 2.0F - at.x * camera.zoom;
    camera.y = static_cast<float>(rows * kMapDotsPerCellY) / 2.0F - at.y * camera.zoom;
    map_camera_ = camera;
}

void TuiState::map_query_finished() {
    if (const auto& outcome = map_query_.outcome())
        map_.set_highlight(outcome->highlight);
    map_query_text_.clear();
    map_query_row_ = 0;
}

void TuiState::map_query_failed() {
    begin_map_input(MapInputKind::Query, views_, map_query_text_);
}

void TuiState::clear_map_query() {
    map_query_.clear();
    map_.set_highlight(std::nullopt);
    map_query_row_ = 0;
}

// Tablo görünümü (`O`): ↑↓ PgUp PgDn satır seçer, Enter satırın ilk notunu açar, Esc sonucu kaldırır. Başka tuş
// haritanın tuşlarına düşer (`O` haritaya döner, `Q` sorgu).
std::optional<Intent> TuiState::handle_map_table_key(Key key) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- map_query_table() sonucu gerektirir
    const auto& rows = map_query_.outcome()->table.rows;
    const std::size_t last = rows.empty() ? 0 : rows.size() - 1;
    const auto page = static_cast<std::size_t>(std::max(map_cells().second - 2, 1));
    switch (key) {
    case Key::Up:
        map_query_row_ -= map_query_row_ > 0 ? 1 : 0;
        return Intent::None;
    case Key::Down:
        map_query_row_ = std::min(map_query_row_ + 1, last);
        return Intent::None;
    case Key::PageUp:
        map_query_row_ -= std::min(map_query_row_, page);
        return Intent::None;
    case Key::PageDown:
        map_query_row_ = std::min(map_query_row_ + page, last);
        return Intent::None;
    case Key::Enter: {
        if (map_query_row_ >= rows.size())
            return Intent::None;
        const auto& cells = rows[map_query_row_];
        const auto note = std::ranges::find(cells, app::QueryCell::Kind::Note, &app::QueryCell::kind);
        if (note == cells.end())
            return Intent::None;
        map_selected_ = note->note;
        return map_selected() ? Intent::OpenMapNode : Intent::None;
    }
    case Key::Escape:
        clear_map_query();
        return Intent::None;
    default:
        return std::nullopt;
    }
}

void TuiState::map_note_created(const core::NotePath& path) {
    if (const auto at = std::exchange(map_new_at_, std::nullopt); at && map_view())
        map_.keep(path.string(), *at);
}

std::string TuiState::name_prefix_for(std::string_view folder) const {
    const std::string base = name_base_folder();
    if (folder.empty() || folder == base || !in_folder(folder, base))
        return {};
    return std::string(folder.substr(base.empty() ? 0 : base.size() + 1)) + "/";
}

void TuiState::enter_folder(std::string folder) {
    current_folder_ = std::move(folder);
    refilter(true);
    select_index(0);
}

void TuiState::go_to_folder(std::string folder) {
    const std::string from = std::exchange(current_folder_, std::move(folder));
    leave_preview();
    refilter(true);
    if (!in_folder(from, current_folder_) || from == current_folder_)
        return;
    const std::size_t begin = current_folder_.empty() ? 0 : current_folder_.size() + 1;
    const std::size_t end = from.find('/', begin);
    select_key(from.substr(0, end) + "/");
}

void TuiState::select_key(const std::string& key) {
    const auto found = std::ranges::find_if(rows_, [&](const Row& row) { return row_key(row) == key; });
    if (found != rows_.end())
        select_index(static_cast<std::size_t>(found - rows_.begin()));
}

bool TuiState::select_folder(const std::string& folder) {
    search_line_.clear();
    mode_ = Mode::Name;
    current_folder_ = parent_folder(folder);
    focus_ = Focus::List;
    refilter(true);
    select_key(folder + "/");
    const Row* row = selected_folder_row();
    return row && row->path.string() == folder;
}

bool TuiState::open_selected_folder() {
    const Row* row = selected_folder_row();
    if (!row)
        return false;
    enter_folder(row->path.string());
    return true;
}

// Düz sonuçta notun klasörüne gider; klasör görünümünde üst klasör, kökte etkisiz.
void TuiState::go_left() {
    if (!in_folder_view()) {
        if (const Row* row = selected_row()) {
            const core::NotePath path = row->path;
            reveal(path);
        }
        return;
    }
    if (!current_folder_.empty())
        go_to_folder(parent_folder(current_folder_));
}

std::vector<Row>::const_iterator TuiState::find_note_row(const core::NotePath& path) const {
    return std::ranges::find_if(rows_, [&](const Row& row) { return !row.is_folder && row.path == path; });
}

const core::NoteMeta* TuiState::meta_of(const core::NotePath& path) const {
    if (trash_view_) {
        const auto found = std::ranges::find(trash_metas_, path, &core::NoteMeta::path);
        return found == trash_metas_.end() ? nullptr : &*found;
    }
    return listing_.meta_of(path);
}

void TuiState::enter_trash(std::vector<core::TrashItem> items) {
    trash_view_ = true;
    search_line_.clear();
    mode_ = Mode::Name;
    query_error_.reset();
    selected_ = 0;
    preview_find_.clear();
    focus_ = Focus::List;  // fareyle önizlemeden de gelinir: tuşlar çöp listesine gitsin
    set_trash(std::move(items));
}

void TuiState::set_trash(std::vector<core::TrashItem> items) {
    trash_metas_.clear();
    for (core::TrashItem& item : items)
        trash_metas_.push_back({.path = std::move(item.path),
                                .size = item.size,
                                .updated_at = item.trashed_at,
                                .created_at = item.created_at,
                                .origin = std::move(item.origin)});
    std::set<std::string> present;
    for (const core::NoteMeta& meta : trash_metas_)
        present.insert(meta.path.string());
    std::erase_if(trash_marks_, [&](const std::string& key) { return !present.contains(key); });
    refilter(false);
}

void TuiState::leave_trash() {
    trash_view_ = false;
    trash_metas_.clear();
    trash_marks_.clear();
    selected_ = 0;
    saved_spot_.reset();  // çöpten dönüş ilk satıra iner (arama dönüşünden farklı)
    refilter(true);
}

// Elle düzenlenmiş dosyadaki uzun son komut listesi de sınıra iner.
void TuiState::set_saved_tabs(app::SavedTabs tabs) {
    if (tabs == prefs_.tabs)
        return;
    prefs_.tabs = std::move(tabs);
    prefs_changed_ = true;
}

void TuiState::set_list_prefs(const ListPrefs& prefs) {
    prefs_ = prefs;
    if (prefs_.recent_commands.size() > recent_commands_limit_.cap())
        prefs_.recent_commands.resize(recent_commands_limit_.cap());
    if (prefs_.list_share)  // tuning.tui_list_share'i geçer; sütun sınırları okurken uygulanır
        layout_.set_share(*prefs_.list_share);
    map_.set_options(prefs_.map.options);  // derinlik limits.app.map_depth'e çekilir
    set_map_view(prefs_.map.shown);
    refilter(false);
}

bool TuiState::take_prefs_changed() {
    app::MapSession map{.shown = map_view_, .view = map_query_.view(), .options = map_.options()};
    if (map != prefs_.map) {
        prefs_.map = std::move(map);
        prefs_changed_ = true;
    }
    return std::exchange(prefs_changed_, false);
}

void TuiState::store_list_share() {
    const std::optional<double> share = layout_.chosen_share(window_width_);
    if (!share || share == prefs_.list_share)
        return;
    prefs_.list_share = share;
    prefs_changed_ = true;
}

std::size_t TuiState::row_height(std::size_t row) const {
    ++work_.row_visits;
    const Row& shown = rows_[row];
    const std::size_t details = shown.is_folder ? kFolderMetaLines : kMetaLines;
    return 1 + (shown.show_best_line ? 1 : 0) + (is_expanded(shown) ? details : 0);
}

bool TuiState::is_expanded(const Row& row) const {
    return (row.is_folder ? expanded_folders_ : expanded_).contains(row.path);
}

bool TuiState::line_starts_row(std::size_t visible, std::size_t line) const {
    std::size_t walked = 0;
    for (std::size_t row = list_top(visible); row < rows_.size() && walked <= line; ++row) {
        if (walked == line)
            return true;
        walked += row_height(row);
    }
    return false;
}

void TuiState::toggle_tree(std::size_t row) {
    last_click_row_.reset();
    if (row >= rows_.size() || !rows_[row].is_folder)
        return;
    const std::string folder = rows_[row].path.string();
    if (!prefs_.open_folders.erase(folder))
        prefs_.open_folders.insert(folder);
    refilter(false);
}

// Ağaç boşken (ilk tarama, açılamayan depo) süzgeç yoktur: açık klasörler kaybolmaz (app::open_folder_filter).
void TuiState::prune_open_folders() {
    if (const auto keep = app::open_folder_filter(listing_.tree()))
        std::erase_if(prefs_.open_folders, [&keep](const std::string& folder) { return !keep(folder); });
}

void TuiState::toggle_expanded(std::size_t row) {
    last_click_row_.reset();  // işaret tıklaması çift tıklamanın ilk yarısı sayılmaz
    if (row >= rows_.size())
        return;
    const core::NotePath& path = rows_[row].path;
    std::set<core::NotePath>& open = rows_[row].is_folder ? expanded_folders_ : expanded_;
    if (!open.erase(path))
        open.insert(path);
}

std::size_t TuiState::row_at(std::size_t visible, std::size_t line) const {
    std::size_t walked = 0;
    for (std::size_t row = list_top(visible); row < rows_.size(); ++row) {
        walked += row_height(row);
        if (line < walked)
            return row;
    }
    return rows_.size();
}

Intent TuiState::handle_trash(Key key, std::string_view text) {
    switch (key) {
    case Key::Up:
    case Key::CtrlP:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
    case Key::CtrlN:
        move_selection(1);
        return Intent::None;
    case Key::AltRight:
    case Key::AltLeft:
        set_selected_expanded(key == Key::AltRight);
        return Intent::None;
    case Key::Escape:
        return Intent::ToggleTrash;
    case Key::Char:
        if (text == "g")
            awaiting_prefix_ = true;
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::intent_if_selected(Intent intent) const {
    return selected_row() ? intent : Intent::None;
}

Intent TuiState::intent_if_any_row(Intent intent) const {
    return selected_entry() || marked_count() > 0 ? intent : Intent::None;
}

void TuiState::set_selected_expanded(bool expanded) {
    const Row* row = selected_entry();
    if (!row)
        return;
    std::set<core::NotePath>& open = row->is_folder ? expanded_folders_ : expanded_;
    if (expanded)
        open.insert(row->path);
    else
        open.erase(row->path);
}

void TuiState::toggle_all_expanded() {
    const bool all_open = std::ranges::all_of(rows_, [this](const Row& row) { return is_expanded(row); });
    for (const Row& row : rows_) {
        std::set<core::NotePath>& open = row.is_folder ? expanded_folders_ : expanded_;
        if (all_open)
            open.erase(row.path);
        else
            open.insert(row.path);
    }
}

// `Alt+ok` her terminalde gelmez: Space ve `i` seçili satırı, `I` tümünü açar/kapatır.
void TuiState::toggle_selected_details() {
    toggle_expanded(selected_);
}

// Space: klasör yerinde açılır (açık küme `tui-state.toml`da kalır), notta ayrıntı.
Intent TuiState::toggle_row() {
    if (!selected_folder_row()) {
        toggle_selected_details();
        return Intent::None;
    }
    toggle_tree(selected_);
    return Intent::SavePrefs;
}

// Tümü açıksa kapatır, değilse hepsini açar (`I` gibi).
Intent TuiState::toggle_tree_all() {
    std::set<std::string> all;
    for (std::string& folder : listing_.tree().folders_under({}))
        if (!folder.empty())
            all.insert(std::move(folder));
    const bool all_open = std::ranges::includes(prefs_.open_folders, all);
    prefs_.open_folders = all_open ? std::set<std::string>{} : std::move(all);
    refilter(false);
    return Intent::SavePrefs;
}

Intent TuiState::cycle_column() {
    return set_column(static_cast<ListColumn>((static_cast<int>(prefs_.column) + 1) % kListColumnCount));
}

Intent TuiState::set_column(ListColumn column) {
    prefs_.column = column;
    return Intent::SavePrefs;
}

Intent TuiState::handle_sort_menu(std::string_view text) {
    constexpr std::array<std::pair<std::string_view, search::SortField>, 4> kFields = {{
        {"n", search::SortField::Name},
        {"u", search::SortField::Updated},
        {"c", search::SortField::Created},
        {"s", search::SortField::Size},
    }};
    if (text == "r") {
        prefs_.sort.descending = !prefs_.sort.descending;
    } else {
        const auto* found = std::ranges::find(kFields, text, [](const auto& entry) { return entry.first; });
        if (found == kFields.end())
            return Intent::None;
        prefs_.sort = app::sort_for(found->second);
    }
    focus_ = Focus::List;
    refilter(false);
    return Intent::SavePrefs;
}

void TuiState::set_preview_line_count(std::size_t lines) {
    preview_lines_ = lines;
    scroll_preview(0);  // yeni sınıra kenetle
}

// Çöp kendi listesidir; klasör görünümü ve arama ortak listeleme kuralından gelir. Bozuk argümanda
// query_error_ dolar, satırlar son geçerli sorgunun bu listedeki sonucudur. Klasör ve çöp görünümü aramayı
// bitirir: sonraki bozuk sorgu önceki aramanın satırlarına dönmez.
std::vector<Row> TuiState::filter_rows() {
    std::vector<Row> rows;
    if (trash_view_) {
        listing_.forget_last_query();
        for (const core::NoteMeta& meta : trash_metas_)
            rows.push_back({meta.path});
        return rows;
    }
    listing_.set_sort(prefs_.sort);
    if (in_folder_view()) {
        listing_.forget_last_query();
        query_error_.reset();
        return rows_in_folder();
    }
    app::SearchResult result = listing_.search(query(), mode_);
    query_error_ = std::move(result.error);
    for (app::ListRow& row : result.rows)
        rows.push_back(to_row(std::move(row)));
    return rows;
}

void TuiState::number_rows() {
    ++work_.rebuilds;
    max_row_number_ = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].number == 0)
            rows_[i].number = i + 1;  // düz liste: sonuç sırası
        max_row_number_ = std::max(max_row_number_, rows_[i].number);
    }
}

void TuiState::refilter(bool query_changed) {
    std::optional<std::string> previous;
    if (const Row* row = selected_entry())
        previous = row_key(*row);
    const bool now_folders = in_folder_view();
    if (showing_folders_ && !now_folders)
        saved_spot_ = previous;

    rows_ = filter_rows();
    number_rows();
    if (!showing_folders_ && now_folders)
        previous = std::exchange(saved_spot_, std::nullopt);
    showing_folders_ = now_folders;

    selected_ = 0;
    if (previous) {
        const auto it =
            std::ranges::find_if(rows_, [&](const Row& row) { return row_key(row) == *previous; });
        if (it != rows_.end())
            selected_ = static_cast<std::size_t>(it - rows_.begin());
    }
    const Row* now = selected_entry();
    const bool same_row = previous && now && row_key(*now) == *previous;
    // Bozuk argümanda satırlar son geçerli sorgudandır: aynı satırda önizleme kaydırması korunur.
    if (!same_row || (query_changed && !query_error_))
        scroll_to_source_line(now ? initial_scroll(*now) : 0);
}

bool TuiState::reveal(const core::NotePath& path) {
    search_line_.clear();
    mode_ = Mode::Name;
    if (listing_.meta_of(path))
        current_folder_ = folder_of(path);
    refilter(true);
    const auto it = find_note_row(path);
    focus_ = Focus::List;  // yeni/taşınan not üzerinde hemen harf kısayolları çalışsın
    if (it == rows_.end())
        return false;
    selected_ = static_cast<std::size_t>(it - rows_.begin());
    scroll_to_source_line(0);
    return true;
}

bool TuiState::select_note(const core::NotePath& path) {
    auto it = find_note_row(path);
    if (it == rows_.end() && (!query().empty() || in_folder_view())) {
        search_line_.clear();
        mode_ = Mode::Name;
        if (listing_.meta_of(path))
            current_folder_ = folder_of(path);  // panelde düzenlenen not listede de görünsün
        refilter(true);
        it = find_note_row(path);
    }
    if (it == rows_.end())
        return false;
    selected_ = static_cast<std::size_t>(it - rows_.begin());
    return true;
}

// Kullanıcının kendi yakınlaştırması kalır; yalnız bu görünümün açtığı yakınlaştırma çıkışta geri alınır.
void TuiState::open_new_note_view() {
    new_note_view_ = true;
    auto_zoomed_ = !layout_.zoomed();
    layout_.set_zoomed(true);
}

void TuiState::leave_editor(std::optional<std::size_t> preview_line) {
    if (new_note_view_) {
        new_note_view_ = false;
        if (std::exchange(auto_zoomed_, false))
            layout_.set_zoomed(false);
        preview_line.reset();  // not listede seçili kalır (on_saved), önizleme baştan
        scroll_to_source_line(0);
    }
    if (!preview_line) {
        focus_ = Focus::List;
        return;
    }
    focus_ = Focus::Preview;
    scroll_to_source_line(*preview_line);
    scroll_preview(0);
}

std::size_t TuiState::source_line_at(std::size_t display_line) const {
    if (view_source_lines_.empty())
        return display_line;
    return view_source_lines_[std::min(display_line, view_source_lines_.size() - 1)];
}

// Satır bir bloğun ortasına düşebilir: o bloğun ilk görüntü satırı.
std::size_t TuiState::display_line_of(std::size_t source_line) const {
    if (view_source_lines_.empty())
        return source_line;
    const auto after = std::ranges::upper_bound(view_source_lines_, source_line);
    if (after == view_source_lines_.begin())
        return 0;
    const auto block = std::ranges::lower_bound(view_source_lines_, *std::prev(after));
    return static_cast<std::size_t>(block - view_source_lines_.begin());
}

void TuiState::scroll_to_source_line(std::size_t line) {
    preview_scroll_ = display_line_of(line);
    pending_source_line_ = line;
}

void TuiState::set_preview_view(const std::vector<std::size_t>* source_lines, std::size_t generation) {
    // Parça parça uzayan görünüm aynı üretimdir: yalnız eklenen satırların eşlemesi kopyalanır.
    if (generation == view_generation_ && source_lines && source_lines->size() > view_source_lines_.size())
        view_source_lines_.insert(view_source_lines_.end(),
                                  source_lines->begin() + static_cast<std::ptrdiff_t>(view_source_lines_.size()),
                                  source_lines->end());
    if (generation == view_generation_ && !pending_source_line_)
        return;
    const std::size_t top = pending_source_line_ ? *pending_source_line_ : source_line_at(preview_scroll_);
    pending_source_line_.reset();
    if (generation != view_generation_) {
        view_generation_ = generation;
        if (source_lines)
            view_source_lines_ = *source_lines;
        else
            view_source_lines_.clear();
    }
    preview_scroll_ = display_line_of(top);
}

void TuiState::set_preview_lines(const std::vector<std::string>* lines, std::size_t generation, bool partial,
                                 const std::vector<std::vector<StyledSpan>>* styled,
                                 const std::vector<std::string>* wiki_targets) {
    const bool grew = generation == preview_generation_ && preview_lines_ != lines->size();
    preview_text_ = lines;
    preview_styled_ = styled && wiki_targets ? styled : nullptr;
    preview_wiki_targets_ = wiki_targets;
    preview_partial_ = partial;
    set_preview_line_count(lines->size());
    if (generation != preview_generation_) {
        preview_generation_ = generation;
        preview_selection_.reset();  // uçlar eski metne ait
        wiki_link_.reset();
        preview_column_ = 0;
        preview_find_.recompute(*lines);
    } else if (grew && !preview_find_.query().empty()) {
        preview_find_.recompute(*lines);
    }
}

void TuiState::set_links(const core::NoteLinks& links, std::size_t generation,
                         const std::optional<core::NotePath>& note) {
    if (generation == links_generation_)
        return;
    links_generation_ = generation;
    const bool same_note = links_note_ == note;
    links_note_ = note;
    links_.set(links, same_note);
    if (links_.empty() && focus_ == Focus::Links)
        focus_ = Focus::Preview;  // son ilişki kalktı: odak görünmez bölümde kalmasın
}

int TuiState::links_reserved_rows(int pane_rows) const {
    return trash_view_ || focus_ == Focus::Editor || links_hidden_ ? 0 : links_.reserved_rows(pane_rows);
}

void TuiState::click_link(std::size_t row) {
    const auto visible = static_cast<std::size_t>(links_.visible_rows(pane_rows()));
    const std::size_t line = links_.top(visible) + row;
    if (line >= links_.lines().size())
        return;
    focus_ = Focus::Links;
    if (links_.lines()[line].is_header)
        return;
    links_.select(line);
    if (links_.selected_path())
        jump_to_selected_link();
}

void TuiState::scroll_links(int delta) {
    links_.move(delta);
}

// Hedef süzgeçte ya da başka klasörde olabilir: select_note gevşetir, boş genel aramada reveal.
Intent TuiState::jump_to_selected_link() {
    const core::NotePath* selected = links_.selected_path();
    if (!selected)
        return Intent::LinkMissing;
    const core::NotePath target = *selected;
    return open_note_in_review(target) ? Intent::None : Intent::LinkMissing;
}

bool TuiState::open_note_in_review(const core::NotePath& path) {
    if (!select_note(path) && !reveal(path))
        return false;
    focus_ = Focus::Preview;
    wiki_link_.reset();
    scroll_to_source_line(0);
    return true;
}

std::optional<std::string> TuiState::selected_wiki_target() const {
    if (!wiki_link_ || !preview_text_ || wiki_link_->line >= preview_text_->size())
        return std::nullopt;
    for (const WikiSpan& span : wiki_spans_on(wiki_link_->line))
        if (span.begin == wiki_link_->begin)
            return std::string(span.target);
    return std::nullopt;
}

std::optional<ByteRange> TuiState::wiki_selection_on_line(std::size_t line) const {
    if (!wiki_link_ || wiki_link_->line != line)
        return std::nullopt;
    return ByteRange{wiki_link_->begin, wiki_link_->end};
}

std::vector<WikiSpan> TuiState::wiki_spans_on(std::size_t line) const {
    if (!preview_text_ || line >= preview_text_->size())
        return {};
    if (!preview_styled_)
        return wiki_spans((*preview_text_)[line]);
    // set_preview_lines ikisini birlikte kurar.
    return line < preview_styled_->size() ? wiki_spans((*preview_styled_)[line], *preview_wiki_targets_)
                                          : std::vector<WikiSpan>{};
}

bool TuiState::select_wiki_link_at(std::size_t line, std::size_t offset) {
    for (const WikiSpan& span : wiki_spans_on(line))
        if (offset >= span.begin && offset < span.end) {
            wiki_link_ = PreviewPoint{line, span.begin, span.end};
            return true;
        }
    return false;
}

// Seçim yoksa ileri görünen ilk satırdan, geri görünen son satırdan başlar; uçta durur (sarmaz).
void TuiState::step_wiki_link(int direction) {
    if (!preview_text_ || preview_text_->empty())
        return;
    const std::vector<std::string>& lines = *preview_text_;
    const std::size_t last_visible = std::min(preview_scroll_ + page_rows(), lines.size()) - 1;
    std::size_t line = wiki_link_ ? wiki_link_->line : (direction > 0 ? preview_scroll_ : last_visible);
    if (line >= lines.size())
        return;
    const std::size_t from = wiki_link_ ? (direction > 0 ? wiki_link_->end : wiki_link_->begin)
                                        : (direction > 0 ? 0 : lines[line].size() + 1);
    std::optional<PreviewPoint> found;
    for (bool first = true;; first = false) {
        const std::vector<WikiSpan> spans = wiki_spans_on(line);
        if (direction > 0) {
            const auto next = std::ranges::find_if(
                spans, [&](const WikiSpan& span) { return !first || span.begin >= from; });
            if (next != spans.end())
                found = PreviewPoint{line, next->begin, next->end};
        } else {
            for (const WikiSpan& span : spans)
                if (!first || span.end <= from)
                    found = PreviewPoint{line, span.begin, span.end};
        }
        if (found || (direction > 0 ? line + 1 >= lines.size() : line == 0))
            break;
        line = direction > 0 ? line + 1 : line - 1;
    }
    if (!found)
        return;
    wiki_link_ = found;
    if (found->line < preview_scroll_ || found->line >= preview_scroll_ + page_rows())
        preview_scroll_ = std::min(found->line, max_preview_scroll());
}

void TuiState::begin_preview_selection(PreviewPoint point, int x, int y) {
    preview_selection_ = PreviewSelection{point, point, x, y};
}

// cppcheck-suppress passedByValue ; üç size_t, kopyası ucuz
void TuiState::extend_preview_selection(PreviewPoint point, int x, int y) {
    if (!preview_selection_)
        return;
    PreviewSelection& selection = *preview_selection_;
    selection.moved = selection.moved || x != selection.x || y != selection.y;
    selection.head = point;
    selection.x = x;
    selection.y = y;
}

bool TuiState::finish_preview_selection() {
    if (!preview_selection_)
        return false;
    preview_selection_->dragging = false;
    if (preview_selection_->moved)
        return true;
    preview_selection_.reset();  // yalnız tıklama: seçim yok
    return false;
}

std::optional<PreviewRange> TuiState::preview_selection_range() const {
    if (!preview_selection_ || !preview_selection_->moved)
        return std::nullopt;
    const PreviewPoint& a = preview_selection_->anchor;
    const PreviewPoint& b = preview_selection_->head;
    const bool forward = std::pair(a.line, a.begin) <= std::pair(b.line, b.begin);
    const PreviewPoint& first = forward ? a : b;
    const PreviewPoint& last = forward ? b : a;
    return PreviewRange{first.line, first.begin, last.line, last.end};
}

std::optional<ByteRange> TuiState::preview_selection_on_line(std::size_t line, std::size_t line_size) const {
    const auto range = preview_selection_range();
    if (!range || line < range->begin_line || line > range->end_line)
        return std::nullopt;
    return ByteRange{line == range->begin_line ? range->begin_offset : 0,
                     line == range->end_line ? range->end_offset : line_size};
}

void TuiState::set_window(int width, int height) {
    window_width_ = width;
    window_height_ = height;
}

// Seçili satır (ek satırlarıyla) pencerenin altına oturacak en üstteki satır.
std::size_t TuiState::list_top(std::size_t visible) const {
    if (rows_.empty())
        return 0;
    std::size_t top = selected_;
    std::size_t used = row_height(selected_);
    while (top > 0 && used + row_height(top - 1) <= visible) {
        used += row_height(top - 1);
        --top;
    }
    return top;
}

void TuiState::click_row(std::size_t row) {
    leave_preview();
    focus_ = Focus::List;
    if (row < rows_.size())
        select_index(row);
}

void TuiState::note_row_click(std::size_t row) {
    const auto now = clock_();
    const bool again = last_click_row_ == row && now - last_click_at_ < kDoubleClickGap;
    last_click_row_ = row;
    last_click_at_ = now;
    if (again && row < rows_.size() && rows_[row].is_folder) {
        last_click_row_.reset();
        open_selected_folder();
    }
}

void TuiState::focus_preview() {
    if (focus_ != Focus::PreviewFind)
        focus_ = Focus::Preview;
}

void TuiState::leave_preview() {
    preview_find_.clear();
    wiki_link_.reset();
    focus_ = Focus::List;
}

void TuiState::scroll_to_find_match() {
    if (const auto line = preview_find_.current_line()) {
        preview_scroll_ = *line > kHitContextLines ? *line - kHitContextLines : 0;
        scroll_preview(0);
    }
}

bool TuiState::handle_layout_letter(std::string_view text) {
    if (text == "<")
        resize_list(-1);
    else if (text == ">")
        resize_list(1);
    else if (text == "z")
        toggle_zoom();
    else
        return false;
    return true;
}

void TuiState::select_index(std::size_t index) {
    if (rows_.empty())
        return;
    selected_ = std::min(index, rows_.size() - 1);
    scroll_to_source_line(initial_scroll(rows_[selected_]));
}

void TuiState::move_selection(int delta) {
    if (rows_.empty())
        return;
    const auto last = static_cast<int>(rows_.size()) - 1;
    const int next = std::clamp(static_cast<int>(selected_) + delta, 0, last);
    if (static_cast<std::size_t>(next) != selected_) {
        selected_ = static_cast<std::size_t>(next);
        scroll_to_source_line(initial_scroll(rows_[selected_]));
    }
}

std::size_t TuiState::page_rows() const {
    return static_cast<std::size_t>(std::max(window_height_ - kChromeLines, 1));
}

// Tuş tekrarında adım büyür, en çok sayfanın üçte biri; duraklama ya da yön değişimi sıfırlar.
void TuiState::scroll_accelerated(int direction) {
    const auto now = clock_();
    const bool repeating = direction == scroll_direction_ && now - last_scroll_key_ < kKeyRepeatGap;
    scroll_repeats_ = repeating ? scroll_repeats_ + 1 : 0;
    scroll_direction_ = direction;
    last_scroll_key_ = now;
    const int max_step = std::max(static_cast<int>(page_rows()) / kMaxStepPageDivisor, 1);
    scroll_preview(direction * std::min(1 + scroll_repeats_ / kRepeatsPerStep, max_step));
}

std::size_t TuiState::max_preview_scroll() const {
    // Devamı üretilmemiş görünümde sınır yok: istenen satıra dek üretilir, sonra kenetlenir. Toplamada taşmasın.
    if (preview_partial_)
        return std::numeric_limits<std::size_t>::max() / 2;
    return preview_lines_ == 0 ? 0 : preview_lines_ - 1;
}

std::size_t TuiState::preview_skip(std::string_view line) const {
    std::size_t skip = std::min(preview_column(), line.size());
    while (skip > 0 && skip < line.size() && (static_cast<unsigned char>(line[skip]) & 0xC0) == 0x80)
        --skip;
    return skip;
}

// Ham görünümün satırı panele sarılır; kaydırma birimi satırdır. Ekrandan uzun satırın devamına yana kaydırarak
// gelinir (GUI ham önizlemesi uzun satırı 4 KiB'lık satırlara böler): adım bir panel genişliği, sarılan dev
// satırda bir görüntü satırı. Bayt cinsindendir: ofset kod noktası başına çekilir, iş satır boyuyla büyümez.
void TuiState::scroll_preview_columns(int direction) {
    if (!preview_text_ || !view_source_lines_.empty())
        return;
    const auto step = static_cast<std::size_t>(
        std::max(layout_.geometry(window_width_, true).right_width - kPaneChromeCells, 1));
    const std::size_t end = std::min(preview_text_->size(), preview_scroll_ + page_rows());
    std::size_t longest = 0;
    for (std::size_t line = std::min(preview_scroll_, end); line < end; ++line)
        longest = std::max(longest, (*preview_text_)[line].size());
    const std::size_t last = longest == 0 ? 0 : longest - 1;
    preview_column_ = direction < 0 ? preview_column_ - std::min(preview_column_, step)
                                    : std::min(preview_column_ + step, std::max(last, preview_column_));
}

void TuiState::scroll_preview(int delta) {
    const std::size_t step = static_cast<std::size_t>(delta < 0 ? -delta : delta);
    const std::size_t next =
        delta < 0 ? (preview_scroll_ > step ? preview_scroll_ - step : 0) : preview_scroll_ + step;
    preview_scroll_ = std::min(next, max_preview_scroll());
}

Intent TuiState::handle(Key key, std::string_view text) {
    preview_selection_.reset();
    // Haritadan `n`in yeri yalnız açtığı ad satırınındır: satır açıkken tuşlar buraya gelmez (main satıra verir),
    // gelen tuş satırın kapandığını (Esc ya da kayıt) söyler. Sonraki ilgisiz not o yere sabitlenmesin.
    map_new_at_.reset();
    // Alt+Z ve F11 her odakta (editör, bağlantılar, özellikler, girdi satırları): kendileri metin üretmez.
    if (key == Key::AltZ || key == Key::F11) {
        toggle_zoom();
        return Intent::None;
    }
    // Editörde her tuş metne aittir (Ctrl-C kopyalar, F2 kaydedilmemiş metni sorar); yalnız yardım ve
    // tablonun editör satırları (sekmeler, Ctrl+K) araya girer. Harf hiçbir zaman: metne gider.
    if (focus_ == Focus::Editor) {
        if (key == Key::F1)
            return Intent::ShowHelp;
        const Command* command = key == Key::Char ? nullptr : bound_command(kInEditor, key, {});
        return command ? command->run(*this) : Intent::EditorKey;
    }
    const bool prefixed = std::exchange(awaiting_prefix_, false);
    if (const auto intent = app_intent(key)) {
        palette_.reset();
        return *intent;
    }
    if (palette_)
        return handle_palette(key, text);
    if (!summary_.empty()) {
        summary_.clear();
        return Intent::None;
    }
    if (prefixed)
        return handle_prefix(key, text);
    return handle_focused(key, text);
}

Intent TuiState::handle_focused(Key key, std::string_view text) {
    if (focus_ == Focus::Inspector)  // harfler komut tablosundan önce: `a` `e` `d` denetçinindir
        return handle_inspector(key, text);
    if ((focus_ == Focus::Preview || focus_ == Focus::Links) && key == Key::Char && text == "p" &&
        !trash_view_)
        return inspect_review();
    if (focus_ == Focus::List && map_view())
        if (const auto intent = handle_map_key(key, text))
            return *intent;
    if (const auto intent = handle_command_key(key, text))
        return *intent;
    if (trash_view_)
        return handle_trash(key, text);
    if (focus_ == Focus::SortMenu) {
        if (key == Key::Escape)
            focus_ = Focus::List;
        return key == Key::Char ? handle_sort_menu(text) : Intent::None;
    }
    if (focus_ == Focus::Preview)
        return handle_preview(key);
    if (focus_ == Focus::PreviewFind)
        return handle_preview_find(key, text);
    if (focus_ == Focus::Links)
        return handle_links(key, text);
    if (focus_ == Focus::Search)
        if (const auto intent = handle_search_line(key, text))
            return *intent;
    if (const auto intent = handle_shared(key))
        return *intent;
    return focus_ == Focus::Search ? handle_search(key) : handle_list(key, text);
}

void TuiState::toggle_content_mode() {
    mode_ = mode_ == Mode::Name ? Mode::Content : Mode::Name;
    refilter(true);
}

// Metin, imleç, öneri, Tab listesi ve geçmiş girdinindir; geri kalan tuşlar aramanın ortak yoluna düşer.
std::optional<Intent> TuiState::handle_search_line(Key key, std::string_view text) {
    search_line_.set_sources(input_sources());
    const InputLine::Result result = search_line_.handle(key, text, prefs_.search_history);
    if (result == InputLine::Result::Edited)
        refilter(true);
    return result == InputLine::Result::Ignored ? std::nullopt : std::optional(Intent::None);
}

std::optional<Intent> TuiState::handle_shared(Key key) {
    switch (key) {
    case Key::CtrlP:
        move_selection(-1);
        return Intent::None;
    case Key::CtrlN:
        move_selection(1);
        return Intent::None;
    case Key::PageUp:
    case Key::PageDown:
        move_selection(page_direction(key) * static_cast<int>(page_rows()));
        return Intent::None;
    case Key::Tab:
        toggle_content_mode();
        return Intent::None;
    case Key::CtrlO:
        return Intent::NewNote;
    case Key::CtrlR:
        return move_dialog_intent();
    case Key::CtrlD:
        return intent_if_any_row(Intent::TrashSelected);
    case Key::AltRight:
    case Key::AltLeft:
        set_selected_expanded(key == Key::AltRight);
        return Intent::None;
    case Key::CtrlS:
        return Intent::SyncNow;
    default:
        return std::nullopt;
    }
}

Intent TuiState::handle_list(Key key, std::string_view text) {
    switch (key) {
    case Key::Char:
        if (text == "g")
            awaiting_prefix_ = true;
        return Intent::None;
    case Key::Up:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
        move_selection(1);
        return Intent::None;
    case Key::Left:
        go_left();
        return Intent::None;
    case Key::Right:
        return right_or_open();
    case Key::Enter:
        return enter_selected();
    case Key::Escape:
        escape_list();
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::right_or_open() {
    if (!open_selected_folder())
        focus_ = Focus::Preview;
    return Intent::None;
}

Intent TuiState::enter_selected() {
    if (open_selected_folder())
        return Intent::None;
    return selected_row() ? Intent::EnterEditor : Intent::None;
}

// Klasörün tipi yok: diyalog değil, yol girdisi.
Intent TuiState::move_dialog_intent() const {
    if (selected_folder_row())
        return Intent::RenameSelected;
    return selected_row() ? Intent::MoveSelected : Intent::None;
}

void TuiState::page_list(int direction) {
    move_selection(direction * static_cast<int>(page_rows()));
}

void TuiState::begin_sort_menu() {
    focus_ = Focus::SortMenu;
}

void TuiState::select_last() {
    select_index(rows_.size());  // son satıra kenetlenir
}

void TuiState::resize_list(int direction) {
    layout_.resize(direction * PaneLayout::kKeyStep, window_width_);
    store_list_share();
}

void TuiState::toggle_zoom() {
    layout_.toggle_zoom();
    auto_zoomed_ = false;  // elle verilen karar: editörden çıkış geri almaz
}

void TuiState::focus_links() {
    focus_ = Focus::Links;
}

void TuiState::toggle_links() {
    links_hidden_ = !links_hidden_;
    if (links_hidden_ && focus_ == Focus::Links)
        focus_ = Focus::Preview;
}

void TuiState::toggle_review_view() {
    if (preview_pdf_)
        pdf_pages_ = !pdf_pages_;
    else
        review_rendered_ = !review_rendered_;
    wiki_link_.reset();  // görünüm değişti: satır ve bayt eski metne ait
}

void TuiState::scroll_preview_top() {
    preview_scroll_ = 0;
}

void TuiState::scroll_preview_bottom() {
    preview_scroll_ = max_preview_scroll();
}

void TuiState::scroll_half_page(int direction) {
    scroll_preview(direction * (static_cast<int>(page_rows()) / 2));
}

Intent TuiState::begin_preview_find() {
    if (pdf_page_view())
        return Intent::PdfPageNoText;
    preview_find_.clear();
    focus_ = Focus::PreviewFind;
    return Intent::None;
}

void TuiState::find_step(bool forward) {
    if (forward)
        preview_find_.next();
    else
        preview_find_.previous();
    scroll_to_find_match();
}

// Önce bekleyen yank, süzgeç ikinci Esc'te. Kullanıcı kararı: çıkış `q`; boş süzgeçte tekrarlanan Esc
// uygulamayı kapatmamalı.
void TuiState::escape_list() {
    if (yank_) {
        yank_.reset();
        return;
    }
    if (query().empty() && mode_ != Mode::Global)
        return;
    search_line_.clear();
    if (mode_ == Mode::Global)
        mode_ = Mode::Name;
    refilter(true);
}

void TuiState::begin_search(Mode mode) {
    focus_ = Focus::Search;
    if (mode == Mode::Global) {
        search_line_.clear();
        mode_ = Mode::Global;
        refilter(true);
    } else if (mode_ == Mode::Global) {
        mode_ = Mode::Name;
        refilter(true);
    }
}

// Palet: ad araması `tag:` ile açılır, Tab listesi notların etiketleridir; tek etiket varsa tamamlanır.
void TuiState::begin_tag_search() {
    begin_search(Mode::Name);
    search_line_.set_text(std::string(kTagArgument));
    refilter(true);
    handle_search_line(Key::Tab, {});
}

// Palet "Run view": `tag:` gibi; Tab listesi kayıtlı görünümlerdir, seçilen `view:AD` süzgecini kurar.
void TuiState::begin_view_search() {
    begin_search(Mode::Name);
    search_line_.set_text(std::string(kViewArgument));
    refilter(true);
    handle_search_line(Key::Tab, {});
}

// Karar GUI ile ortak (app::search_enter_action); odak bu arayüzün.
Intent TuiState::search_enter() {
    const bool global = mode_ == Mode::Global;
    const bool selected = global ? selected_row() != nullptr : selected_entry() != nullptr;
    switch (app::search_enter_action(mode_, selected, query_error_.has_value(), query())) {
    case app::SearchEnter::Nothing:
        return Intent::None;
    case app::SearchEnter::Select:
        focus_ = Focus::List;
        return Intent::None;
    case app::SearchEnter::Remember:
        // Genel aramada önizleme eşleşmeye kaydı; düz aramada seçim kalır, açmak `e` ya da Enter ile Listeden.
        focus_ = global ? Focus::Preview : Focus::List;
        return remember_query();
    case app::SearchEnter::CreateNote:
        break;
    }
    return Intent::CreateFromQuery;
}

// Enter'la kullanılan arama geçmişe girer; eşleşmesiz sorgu notun ilk satırı olur, oraya girmez.
Intent TuiState::remember_query() {
    return remember_search(prefs_.search_history, query(), input_sources().get(), search_history_limits_)
               ? Intent::SavePrefs
               : Intent::None;
}

Intent TuiState::handle_search(Key key) {
    switch (key) {
    case Key::CtrlT:
        toggle_content_mode();
        return Intent::None;
    case Key::Up:
        move_selection(-1);
        return Intent::None;
    case Key::Down:
        move_selection(1);
        return Intent::None;
    case Key::Enter:
        return search_enter();
    case Key::Escape:
        focus_ = Focus::List;  // süzgeç kalır: filtrelenmiş listede hemen `d` çalışsın
        return Intent::None;
    default:
        return Intent::None;
    }
}

// Harfler ve sayfa tuşları tablodadır. Liste komutları burada bilerek yok: içerik panelinde çalışmazlar
// (Ctrl-P/N ve Tab dahil).
Intent TuiState::handle_preview(Key key) {
    switch (key) {
    case Key::Up:
        scroll_accelerated(-1);
        return Intent::None;
    case Key::Down:
        scroll_accelerated(1);
        return Intent::None;
    case Key::Left:
        leave_preview();
        return Intent::None;
    case Key::ShiftLeft:
    case Key::ShiftRight:
        scroll_preview_columns(key == Key::ShiftLeft ? -1 : 1);
        return Intent::None;
    case Key::Tab:
    case Key::ShiftTab:
        step_wiki_link(key == Key::Tab ? 1 : -1);
        return Intent::None;
    case Key::Escape:
        // İlk Esc seçili bağlantıyı ya da vurguyu temizler, sonraki Listeye döner: arama yanlışlıkla panelden
        // çıkarmasın.
        if (wiki_link_)
            wiki_link_.reset();
        else if (preview_find_.query().empty())
            leave_preview();
        else
            preview_find_.clear();
        return Intent::None;
    case Key::Enter:
        if (wiki_link_)
            return Intent::FollowLink;
        return selected_row() ? Intent::EnterEditor : Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::handle_links(Key key, std::string_view text) {
    const int page = std::max(links_.visible_rows(pane_rows()), 1);
    switch (key) {
    case Key::Char:
        return handle_links_letter(text);
    case Key::Up:
        links_.move(-1);
        return Intent::None;
    case Key::Down:
        links_.move(1);
        return Intent::None;
    case Key::PageUp:
    case Key::PageDown:
        links_.move(page_direction(key) * page);
        return Intent::None;
    case Key::Enter:
        return jump_to_selected_link();
    case Key::Escape:
        focus_ = Focus::Preview;
        return Intent::None;
    default:
        return Intent::None;
    }
}

Intent TuiState::handle_links_letter(std::string_view text) {
    if (handle_layout_letter(text))
        return Intent::None;
    if (text == "b") {
        focus_ = Focus::Preview;
    } else if (text == "j") {
        links_.move(1);
    } else if (text == "k") {
        links_.move(-1);
    } else if (text == "L") {
        const core::LinkLine* line = links_.selected_line();
        return line && line->group == core::LinkGroup::Related ? Intent::UnlinkSelected
                                                               : intent_if_selected(Intent::LinkNote);
    } else if (text == "n" || text == "f" || text == "u") {
        // İlişkili satırın adı (name), yönü (flip) ya da yönsüzlüğü (undirected); başka satırda bir şey olmaz.
        const core::LinkLine* line = links_.selected_line();
        if (!line || line->is_header || line->group != core::LinkGroup::Related || !selected_row())
            return Intent::None;
        return text == "n" ? Intent::RenameRelation
               : text == "f" ? Intent::FlipRelation
                             : Intent::UndirectRelation;
    }
    return Intent::None;
}

Intent TuiState::handle_preview_find(Key key, std::string_view text) {
    static const std::vector<std::string> kNoLines;
    const std::vector<std::string>& lines = preview_text_ ? *preview_text_ : kNoLines;
    std::string query = preview_find_.query();
    switch (key) {
    case Key::Char:
        if (!is_printable(text))
            return Intent::None;
        query.append(text);
        break;
    case Key::Backspace:
        if (query.empty())
            return Intent::None;
        while (is_continuation(query.back()) && query.size() > 1)
            query.pop_back();
        query.pop_back();
        break;
    case Key::Enter:
        focus_ = Focus::Preview;  // vurgular kalır; `n` `N` gezer
        return Intent::None;
    case Key::Escape:
        preview_find_.clear();
        focus_ = Focus::Preview;
        return Intent::None;
    case Key::Down:
        preview_find_.next();
        scroll_to_find_match();
        return Intent::None;
    case Key::Up:
        preview_find_.previous();
        scroll_to_find_match();
        return Intent::None;
    default:
        return Intent::None;
    }
    preview_find_.set_query(std::move(query), lines);
    scroll_to_find_match();
    return Intent::None;
}

}  // namespace noteapp::tui
