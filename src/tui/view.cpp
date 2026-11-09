#include "tui/view.hpp"

#include "app/listing.hpp"
#include "app/map_relations.hpp"
#include "app/update_controller.hpp"
#include "core/file_stat.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "edit/display_text.hpp"
#include "edit/editor_session.hpp"
#include "tui/editor_view.hpp"
#include "tui/input_view.hpp"
#include "tui/modals.hpp"
#include "tui/styled_view.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace noteapp::tui {

namespace {

using Range = std::pair<std::size_t, std::size_t>;  // bayt aralığı [başlangıç, bitiş)

constexpr int kFrameCells = 2;     // modal çerçevesinin iki kenarı (paneller: kPaneChromeCells)
constexpr int kMinPathCells = 16;  // sağ sütun yolu bundan fazla sıkıştırmaz
constexpr std::string_view kColumnGap = "  ";
constexpr std::string_view kEllipsis = "…";  // ContentIndex kesitlerinin kenar işareti
constexpr int kFrameLabelOffset = 1;         // etiket köşeyi örtmesin
// Listede süzgeç yüzünden düşmeyen ilk ipuçları; sağda F1 yardım da yer tutar, 60 sütunda dördü sığar.
constexpr int kPriorityHints = 4;
constexpr std::string_view kIconGap = "  ";  // taşma hücresi ve boşluk
constexpr std::string_view kBom = "\xEF\xBB\xBF";

// UTF-8 kod noktasının bayt uzunluğu (baş bayttan).
std::size_t cp_length(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    return 2;
}

// Aralıkları vurgulu, kalanı düz metin olarak dizer. Aralıklar sıralı ve örtüşmesiz varsayılır; taşanlar
// metne kırpılır.
ftxui::Element highlighted(std::string_view s, const std::vector<Range>& ranges) {
    using namespace ftxui;
    Elements parts;
    std::size_t at = 0;
    for (const auto& [begin, end] : ranges) {
        const std::size_t b = std::min(begin, s.size());
        const std::size_t e = std::min(end, s.size());
        if (b < at || b >= e)
            continue;
        if (b > at)
            parts.push_back(text(terminal_safe(s.substr(at, b - at))));
        parts.push_back(text(terminal_safe(s.substr(b, e - b))) | bold | color(Color::Yellow));
        at = e;
    }
    if (at < s.size())
        parts.push_back(text(terminal_safe(s.substr(at))));
    return hbox(std::move(parts));
}

// fuzzy konumları (bayt ofseti) -> her biri bir kod noktası uzunluğunda aralık.
std::vector<Range> ranges_from_positions(std::string_view path, const std::vector<std::size_t>& positions) {
    std::vector<Range> ranges;
    ranges.reserve(positions.size());
    for (const std::size_t p : positions)
        if (p < path.size())
            ranges.emplace_back(p, p + cp_length(static_cast<unsigned char>(path[p])));
    return ranges;
}

// Kesit satırın bir parçasıdır (kenarlarda "…" olabilir); satırdaki yerini bulup aralıkları oraya kaydırır.
// ponytail: yalnız ilk 3 eşleşen satır vurgulanır (ContentIndex'in verdiği kadar); satırda kesit bulunamazsa
// vurgusuz kalır. Tüm eşleşmeler gerekirse terimleri görünüm tarafında yeniden aramak gerekir.
std::vector<Range> hit_ranges_in_line(const search::Hit& hit, std::string_view line) {
    std::string_view core = hit.snippet;
    std::size_t prefix = 0;
    if (core.starts_with(kEllipsis)) {
        core.remove_prefix(kEllipsis.size());
        prefix = kEllipsis.size();
    }
    if (core.ends_with(kEllipsis))
        core.remove_suffix(kEllipsis.size());
    const std::size_t found = line.find(core);
    if (found == std::string_view::npos)
        return {};
    std::vector<Range> ranges;
    for (const auto& [begin, end] : hit.ranges)
        if (begin >= prefix && begin - prefix + found < line.size())
            ranges.emplace_back(begin - prefix + found, end - prefix + found);
    return ranges;
}

std::string mode_label(const TuiState& state) {
    if (state.trash_view())
        return core::tr("tui.mode_trash");
    switch (state.mode()) {
    case Mode::Content:
        return core::tr("tui.mode_content");
    case Mode::Global:
        return core::tr("tui.mode_global");
    case Mode::Name:
        break;
    }
    return core::tr("tui.mode_name");
}

std::string sort_label(const TuiState& state) {
    const search::SortSpec& sort = state.list_sort();
    const char* key = "tui.sort_updated";
    if (sort.field == search::SortField::Name)
        key = "tui.sort_name";
    else if (sort.field == search::SortField::Size)
        key = "tui.sort_size";
    else if (sort.field == search::SortField::Created)
        key = "tui.sort_created";
    return core::tr(key) + (sort.descending ? "↓" : "↑");
}

std::string trash_prefix(const TuiState& state) {
    if (state.icon_style() == core::IconStyle::Off)
        return {};
    return core::icon_text(core::trash_icon(), state.icon_style()) + std::string(kIconGap);
}

// Klasör yolunun bir parçası. `folder` boşsa (kısaltma) tıklanmaz; kök "" olduğu için optional.
struct Crumb {
    std::string text;
    std::optional<std::string> folder;
};

constexpr std::string_view kCrumbSeparator = "/";
constexpr std::string_view kTopBarGap = "  ";
constexpr std::string_view kCrumbEllipsis = "…";
// Üst satırın sağ yarısı (sıra, mod, sayaç, senkron) için ayrılan hücre; dar pencerede yol kısalır.
constexpr int kTopBarRightCells = 44;
constexpr int kMinCrumbCells = 12;

std::string breadcrumb_lead(const TuiState& state) {
    std::string lead = trash_indicator(state);
    if (!lead.empty())
        lead += kTopBarGap;
    if (state.icon_style() != core::IconStyle::Off)
        lead += core::icon_text(core::folder_icon(true), state.icon_style()) + std::string(kIconGap);
    return lead;
}

int crumbs_cells(const std::vector<Crumb>& crumbs) {
    int cells = 0;
    for (const Crumb& crumb : crumbs)
        cells += ftxui::string_width(crumb.text) + static_cast<int>(kCrumbSeparator.size());
    return cells - static_cast<int>(kCrumbSeparator.size());
}

// Yol sığmazsa baştaki parçalar düşer: bulunulan klasör görünür kalır. Adlar güvenilmez: terminal_safe.
std::vector<Crumb> breadcrumb(const TuiState& state) {
    std::vector<Crumb> crumbs;
    if (!state.in_folder_view())
        return crumbs;
    crumbs.push_back({core::tr("tui.breadcrumb_root"), std::string{}});
    const std::string& current = state.current_folder();
    for (std::size_t begin = 0; begin < current.size();) {
        const std::size_t slash = std::min(current.find('/', begin), current.size());
        crumbs.push_back({terminal_safe(std::string_view(current).substr(begin, slash - begin)),
                          current.substr(0, slash)});
        begin = slash + 1;
    }
    const int room =
        std::max(state.window_width() - kTopBarRightCells - ftxui::string_width(breadcrumb_lead(state)),
                 kMinCrumbCells);
    bool cut = false;
    while (crumbs.size() > 1 && crumbs_cells(crumbs) + (cut ? 2 : 0) > room) {
        crumbs.erase(crumbs.begin());
        cut = true;
    }
    if (cut)
        crumbs.insert(crumbs.begin(), Crumb{std::string(kCrumbEllipsis), std::nullopt});
    const int fixed = cut ? 2 : 0;
    Crumb& last = crumbs.back();
    if (crumbs_cells(crumbs) > room)
        last.text = tail_cells(last.text, static_cast<std::size_t>(std::max(room - fixed - 1, 1)));
    return crumbs;
}

// Klasör adları kalın, ayırıcı soluk: yol bir bakışta okunur. Hücreler `breadcrumb_folder_at` ile aynıdır.
ftxui::Element breadcrumb_element(const TuiState& state) {
    using namespace ftxui;
    const std::vector<Crumb> crumbs = breadcrumb(state);
    if (crumbs.empty())
        return text(trash_indicator(state)) | bold;
    Elements parts = {text(breadcrumb_lead(state)) | bold};
    for (std::size_t i = 0; i < crumbs.size(); ++i) {
        if (i > 0)
            parts.push_back(text(std::string(kCrumbSeparator)) | dim);
        parts.push_back(text(crumbs[i].text) | bold);
    }
    return hbox(std::move(parts));
}

constexpr std::string_view kGroupSeparator = " · ";

// Senkron durumu rengiyle: ✓ yeşil, ⟳ mavi, ⚠ kırmızı, kapalı soluk. Renk tek sinyal değildir, ikon da
// söyler. `shown_text` arınmış ve sığdırılmış metindir.
ftxui::Element sync_element(const std::string& shown_text, SyncIndicator::State state) {
    using namespace ftxui;
    Element shown = text(shown_text);
    switch (state) {
    case SyncIndicator::State::Running:
        return shown | color(Color::Blue);
    case SyncIndicator::State::Ok:
        return shown | color(Color::Green);
    case SyncIndicator::State::Failed:
        return shown | color(Color::Red);
    case SyncIndicator::State::Disabled:
        break;
    }
    return shown | dim;
}

ftxui::Element top_bar(const TuiState& state, const StatusLine& status, int width) {
    using namespace ftxui;
    // Satır klasör de sayar: oran yalnız süzgeçte anlamlıdır (eşleşme / not), başka yerde not sayısı.
    const bool filtered = !state.query().empty() || state.mode() == Mode::Global;
    const std::string counts = filtered
                                   ? std::to_string(state.rows().size()) + "/" + std::to_string(state.total())
                                   : count_text("tui.count_notes", state.total());
    Element left = state.trash_view() ? text(trash_prefix(state) + core::tr("tui.trash_title")) | bold
                                      : breadcrumb_element(state);
    // Sağ grup ikincil bilgidir: soluk, ` · ` ayraçlı; yalnız senkron durumu kendi rengindedir.
    std::string right = sort_label(state) + std::string(kGroupSeparator) + mode_label(state) +
                        std::string(kGroupSeparator) + counts;
    // Dar pencerede yalnız senkron metni kısalır ("…"): sıra, mod ve sayaç tam kalır, satır kenardan taşmaz.
    // Metin uzak hata ayrıntısı taşıyabilir: önce arınır, sonra hücreye göre kesilir.
    left->ComputeRequirement();
    const int room = width - left->requirement().min_x - ftxui::string_width(std::string(kTopBarGap)) -
                     ftxui::string_width(right + std::string(kGroupSeparator));
    const std::string sync = head_ellipsis(terminal_safe(status.sync), room);
    if (!sync.empty())
        right += kGroupSeparator;
    // Yakınlaştırma işareti ve sürüm senkron metninden sonra gelir: yalnız senkron tam sığdıktan sonra kalan
    // yerde görünürler, dar pencerede senkron kısalmadan önce düşerler. İşaret sürümden önce yer alır.
    int spare = room - ftxui::string_width(sync);
    Element zoom_element = text("");
    if (state.layout().zoomed()) {
        const std::string zoom = std::string(kGroupSeparator) + core::tr("tui.zoom_marker");
        if (spare >= ftxui::string_width(zoom)) {
            zoom_element = text(zoom) | bold;
            spare -= ftxui::string_width(zoom);
        }
    }
    const std::string version = std::string(kGroupSeparator) + app::version_label();
    Element version_element = text("");
    if (spare >= ftxui::string_width(version))
        version_element = text(version) | dim;
    return hbox({std::move(left), filler(), text(right) | dim, sync_element(sync, status.sync_state),
                 std::move(zoom_element), std::move(version_element)});
}

// `n` görünümünün üst satırı: liste ve sayaçlar gizli, kaydedildiyse notun yolu.
ftxui::Element new_note_header(const EditorSession& editor) {
    using namespace ftxui;
    Elements cells = {text(core::tr("tui.new_note_header")) | bold};
    if (editor.path())
        cells.push_back(text(std::string(kHintSeparator) + editor.path()->string()));
    return hbox(std::move(cells));
}

// Odaklı panelin kipi: 16 ANSI rengi (açık ve koyu terminalde okunur), rengi görmeyen için metin etiketi.
struct PaneMode {
    ftxui::Color color;
    std::string label;
};

PaneMode list_mode() {
    return {ftxui::Color::Cyan, core::tr("tui.frame_list")};
}

PaneMode review_mode() {
    return {ftxui::Color::Green, core::tr("tui.frame_review")};
}

// Biçimli görünümü olan notta etiket hangisinin gösterildiğini söyler (`R`).
PaneMode review_mode(const Preview& preview) {
    PaneMode mode = review_mode();
    if (preview.view_kind != ViewKind::None)
        mode.label += " · " + core::tr(preview.rendered ? "tui.review_rendered" : "tui.review_raw");
    return mode;
}

// Salt-okunur notta yazılamaz: etiket EDIT dese kullanıcı tuşların neden etkisiz olduğunu anlamazdı.
PaneMode editor_mode(const EditorSession& editor) {
    if (editor.read_only() != EditorSession::ReadOnly::No)
        return review_mode();
    return {ftxui::Color::Magenta,
            core::tr("tui.frame_edit") + (editor.dirty() ? std::string(kDirtyMark) : "")};
}

// Alt satırların kipi: editör açıkken satırlar editörü anlatır, yoksa odaktaki panel. Hap ve ipucu tuşları
// bu rengi alır.
PaneMode status_mode(const TuiState& state, const EditorSession* editing) {
    if (editing && editing->is_open())
        return editor_mode(*editing);
    return right_pane_focused(state.focus()) ? review_mode() : list_mode();
}

std::string pill_text(const PaneMode& mode) {
    return " " + mode.label + " ";
}

// Kip hapı: kip renginde zemin, siyah kalın metin (açık ve koyu temada okunur). Renksiz terminalde (NO_COLOR,
// FTXUI Palette1) zemin rengi düşer; hap ters çizilir ki kip yine bir bakışta okunsun.
ftxui::Element mode_pill(const PaneMode& mode) {
    using namespace ftxui;
    Element pill = text(pill_text(mode)) | bold;
    if (Terminal::ColorSupport() == Terminal::Color::Palette1)
        return pill | inverted;
    return pill | bgcolor(mode.color) | color(Color::Black);
}

// Çocuğu çizdikten sonra kutusunun kenar hücrelerine `apply` uygular; içerik hücrelerine dokunmaz.
class EdgeCells : public ftxui::Node {
public:
    EdgeCells(ftxui::Element child, void (*apply)(ftxui::Cell&)) : Node({std::move(child)}), apply_(apply) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }
    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }
    void Render(ftxui::Screen& screen) override {
        Node::Render(screen);
        for (int x = box_.x_min; x <= box_.x_max; ++x) {
            apply_(screen.CellAt(x, box_.y_min));
            apply_(screen.CellAt(x, box_.y_max));
        }
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            apply_(screen.CellAt(box_.x_min, y));
            apply_(screen.CellAt(box_.x_max, y));
        }
    }

private:
    void (*apply_)(ftxui::Cell&);
};

ftxui::Decorator edge_cells(void (*apply)(ftxui::Cell&)) {
    return [apply](ftxui::Element child) { return std::make_shared<EdgeCells>(std::move(child), apply); };
}

// İki panel de yuvarlak çerçeveli (yan yana kalın/ince dikişi olmaz). Odaklı panel kip renginde çerçeve ve
// kalın etiket alır, odaksız çerçeve soluktur: renksiz terminalde de kalınlık ve solukluk odağı anlatır.
// Etiket çerçevenin üzerine ayrı katmandır: `color` süsleyicisi içeriğe uygulanırsa sözdizimi renklerini
// ezerdi. Sekme satırı da kenardadır (`tabs`): panel içeriği kaymaz, editör imleci ve fare eşlemesi değişmez.
ftxui::Element framed_pane(ftxui::Element content, const std::optional<PaneMode>& mode,
                           const std::string& tabs = {}) {
    using namespace ftxui;
    // İçerik sol kenara yapışmaz (§5.5): bir sütunluk boşluk (kPaneContentX) panel boyunca gerçek boşluk
    // hücresidir. İçeriğin dışındadır: seçim tersi ve satır zemini onu boyamaz.
    content = hbox({separatorEmpty(), std::move(content) | flex});
    Element framed = mode ? std::move(content) | borderStyled(ROUNDED, mode->color)
                          : std::move(content) | borderStyled(ROUNDED) |
                                edge_cells([](Cell& cell) { cell.dim = true; });
    if (!mode && tabs.empty())
        return framed;
    Elements label = {emptyElement() | size(WIDTH, EQUAL, kFrameLabelOffset)};
    if (mode)
        label.push_back(text(" " + mode->label + " ") | bold | color(mode->color));
    if (!tabs.empty())
        label.push_back(text(tabs));
    // Dolgu şart: katman tüm panel boyunu alırsa `color` kutusu etiketin altındaki sütunları da boyar.
    return dbox({std::move(framed), vbox({hbox(std::move(label)), filler()})});
}


// Sütun tüm çizim için aynı: saat satır başına okunmaz.
struct ListCells {
    int digits = 1;  // en büyük satır numarasının hane sayısı
    int inner_width = 0;
    std::chrono::system_clock::time_point now;
    bool name_row_open = false;  // odak ad satırında: seçili satır işaretlenmez
    int icon_cells = 0;          // ikon ve boşluğu; ikon kapalıysa 0
};

int digit_count(std::size_t value) {
    return static_cast<int>(std::to_string(value).size());
}

// Girinti seviye başına iki hücre; panelin en çok yarısı: derinde ad görünür kalsın.
int row_indent_cells(const Row& row, int inner_width) {
    constexpr int kIndentCells = 2;
    return std::min(static_cast<int>(row.depth) * kIndentCells, std::max(inner_width, 0) / 2);
}

// Satır başı: seçim işareti, numara, boşluk, ok, boşluk, varsa ikon ve boşluğu (girinti hariç).
int row_prefix_cells(const ListCells& cells) {
    constexpr int kFixedPrefixCells = 4;
    return kFixedPrefixCells + cells.digits + cells.icon_cells;
}

}  // namespace

ftxui::Color icon_color(core::IconColor color) {
    switch (color) {
    case core::IconColor::Blue:
        return ftxui::Color::Blue;
    case core::IconColor::Cyan:
        return ftxui::Color::Cyan;
    case core::IconColor::Green:
        return ftxui::Color::Green;
    case core::IconColor::Yellow:
        return ftxui::Color::Yellow;
    case core::IconColor::Red:
        return ftxui::Color::Red;
    case core::IconColor::Magenta:
        return ftxui::Color::Magenta;
    case core::IconColor::Default:
        break;
    }
    return ftxui::Color::Default;
}

namespace {

// Glif tek hücre sayılır; ardındaki boş hücreye Mono olmayan Nerd Font (MesloLGS: genişlik 1,45 hücre) taşar
// ve tam boyutunda çizilir. Boyutu terminal fontu belirler, bu yalnız ezilmesini önler.
void push_row_icon(ftxui::Elements& cells, const TuiState& state, const Row& row) {
    using namespace ftxui;
    if (state.icon_style() == core::IconStyle::Off)
        return;
    const core::FileIcon icon = row.is_folder ? core::folder_icon(state.is_folder_open(row.path.string()))
                                              : core::file_icon(row.path.string());
    cells.push_back(text(core::icon_text(icon, state.icon_style())) | color(icon_color(icon.color)));
    cells.push_back(text(std::string(kIconGap)));
}

// Sığmayan değer yarım kesilmez: önce tarih, sonra boyut düşer. `free` yolu kesmeden kalan yer; yol uzunsa
// boyut `squeezed` yerde de gösterilir (yol en az kMinPathCells kalır), tarih yol kesmeye değmez.
std::string fitted_size_and_date(const core::NoteMeta& meta, const ListCells& cells, int free, int squeezed) {
    const std::string size = human_size(meta.size);
    std::string both = size + std::string(kColumnGap) + short_time_text(meta.updated_at, cells.now);
    if (ftxui::string_width(both) <= free)
        return both;
    return ftxui::string_width(size) <= std::max(free, squeezed) ? size : std::string{};
}

// İçerik modunda adın ardındaki eşleşme sayısı; sütun hesabı da onu sayar, yoksa ad kısalırdı.
std::string hit_count_text(const TuiState& state, const Row& row) {
    if (state.mode() == Mode::Name || row.hit_count == 0)
        return {};
    return "  (" + std::to_string(row.hit_count) + ")";
}

// Yalnız ilişkiyle bulunan satırın nedeni (SPEC-tui "İlişki araması"); ad ve yol güvenilmez.
std::string reason_suffix(const Row& row) {
    return row.reason ? "  " + terminal_safe(app::reason_text(*row.reason)) : std::string{};
}

// Yolun ve eşleşme sayısının arkasında kalan hücre (sağ sütun ve neden bunu paylaşır).
int cells_after_name(const TuiState& state, const Row& row, const ListCells& cells) {
    return cells.inner_width - row_prefix_cells(cells) - row_indent_cells(row, cells.inner_width) - 1 -
           ftxui::string_width(row.shown_text()) - ftxui::string_width(hit_count_text(state, row));
}

// Boş dönerse sütun çizilmez; yol en az kMinPathCells hücre tutar.
std::string fitted_column(const TuiState& state, const Row& row, const ListCells& cells) {
    const ListColumn column = state.list_column();
    if (column == ListColumn::None)
        return {};
    const int after_prefix =
        cells.inner_width - row_prefix_cells(cells) - row_indent_cells(row, cells.inner_width) - 1;
    const int free = cells_after_name(state, row, cells) - ftxui::string_width(reason_suffix(row));
    // Nedenli satırda sütun yalnız boş yere girer: yol ve neden sütun için kısalmaz.
    const int squeezed = row.reason ? free : after_prefix - kMinPathCells;
    if (row.is_folder) {
        const FolderNode* node = state.folder_tree().find(row.path.string());
        const std::string items = count_text("tui.count_items", node ? node->item_count() : 0);
        return ftxui::string_width(items) <= std::max(free, squeezed) ? items : std::string{};
    }
    const core::NoteMeta* meta = state.meta_of(row.path);
    if (!meta)
        return {};
    if (column == ListColumn::SizeAndDate)
        return fitted_size_and_date(*meta, cells, free, squeezed);
    // Değer GUI Ayrıntılar'la aynı (app::column_text); terminal tam tarihi gösterir. Kaynak cihaz güvenilmez.
    std::string shown = terminal_safe(app::column_text(*meta, column));
    return ftxui::string_width(shown) <= std::max(free, squeezed) ? shown : std::string{};
}

// Yalnız klasörde ok; not satırında aynı iki hücre boş kalır, adlar hizalı.
std::string_view row_arrow(const TuiState& state, const Row& row) {
    if (!row.is_folder)
        return "  ";
    return state.is_folder_open(row.path.string()) ? "▾ " : "▸ ";
}

ftxui::Element list_row(const TuiState& state, const Row& row, std::size_t index, const ListCells& list) {
    using namespace ftxui;
    const bool is_selected = index == state.selected() && !list.name_row_open;
    const std::string path = row.shown_text();
    std::string number = std::to_string(row.number);
    number.insert(0, static_cast<std::size_t>(list.digits) - number.size(), ' ');
    const bool marked = state.is_marked(row);
    Element mark = text(marked ? "*" : " ");
    Elements cells = {
        // cppcheck-suppress accessMoved ; araç yanılıyor: üçlünün yalnız bir kolu çalışır, tek taşıma olur
        marked ? std::move(mark) | bold | color(Color::Yellow) : std::move(mark),
        text(std::string(static_cast<std::size_t>(row_indent_cells(row, list.inner_width)), ' ')),
        text(number) | dim, text(" "), text(row_arrow(state, row))};
    push_row_icon(cells, state, row);
    Element name = highlighted(path, ranges_from_positions(path, row.positions)) | flex_shrink;
    // cppcheck-suppress accessMoved ; araç yanılıyor: üçlünün yalnız bir kolu çalışır, tek taşıma olur
    cells.push_back(marked ? std::move(name) | color(Color::Yellow) : std::move(name));
    if (std::string hits = hit_count_text(state, row); !hits.empty())
        cells.push_back(text(std::move(hits)) | dim);
    const std::string column = fitted_column(state, row, list);
    // Neden yoldan sonra gelir: dar listede yol değil neden "…" ile kısalır.
    if (std::string reason =
            head_ellipsis(reason_suffix(row), cells_after_name(state, row, list) -
                                                  (column.empty() ? 0 : ftxui::string_width(column) + 1));
        !reason.empty())
        cells.push_back(text(std::move(reason)) | dim);
    cells.push_back(filler());  // seçili satırın tersi tüm genişliği kaplasın
    if (!column.empty())
        cells.push_back(text(" " + column) | dim);
    Element line = hbox(std::move(cells));
    if (!is_selected)
        return line;
    // Seçim yalnız satır vurgusudur; odak sağdayken soluk arka plan, ters renk odağı söyler.
    return right_pane_focused(state.focus()) ? line | bgcolor(Color::GrayDark) : line | inverted;
}

// Klasör ayrıntısı: yol, sayılar, toplam boyut, son güncelleme.
void push_folder_details(ftxui::Elements& lines, const TuiState& state, const Row& row,
                         std::string_view indent) {
    using namespace ftxui;
    const std::string path = row.path.string();
    const auto summary = app::folder_summary(state.listing(), path, std::chrono::system_clock::now());
    const std::string fields[TuiState::kFolderMetaLines] = {
        core::tr("tui.meta_path", {{"value", terminal_safe(path)}}),
        summary ? summary->counts : std::string{},
        core::tr("tui.meta_size", {{"value", summary ? summary->size : human_size(0)}}),
        core::tr("tui.meta_updated", {{"value", summary ? summary->updated : std::string{}}}),
    };
    for (const std::string& field : fields)
        lines.push_back(text(std::string(indent) + field) | dim);
}

// Satırın ek görsel satırları; adedi TuiState::row_height ile aynı olmalı (fare ve kaydırma ona güvenir).
void push_row_details(ftxui::Elements& lines, const TuiState& state, const Row& row, int inner_width) {
    using namespace ftxui;
    const std::string kDetailIndent =
        std::string(static_cast<std::size_t>(row_indent_cells(row, inner_width)), ' ') + "    ";
    if (row.show_best_line && !row.hits.empty()) {
        const search::Hit& hit = row.hits.front();
        std::vector<Range> ranges(hit.ranges.begin(), hit.ranges.end());
        lines.push_back(hbox({text(kDetailIndent), highlighted(hit.snippet, ranges) | dim}));
    }
    if (!state.is_expanded(row))
        return;
    if (row.is_folder) {
        push_folder_details(lines, state, row, kDetailIndent);
        return;
    }
    // Alanlar ve değerleri GUI Bilgi paneliyle aynı (app::note_fields); kaynak cihaz ve etiket güvenilmez.
    const core::NoteMeta* meta = state.meta_of(row.path);
    static constexpr std::array<const char*, app::kNoteFields.size()> kLabels = {
        "tui.meta_size", "tui.meta_updated", "tui.meta_created", "tui.meta_sync",
        "tui.meta_origin", "tui.meta_type",  "tui.meta_tags"};
    static_assert(TuiState::kMetaLines == app::kNoteFields.size());
    for (std::size_t i = 0; i < app::kNoteFields.size(); ++i) {
        const std::string value =
            meta ? terminal_safe(app::note_field_text(*meta, app::kNoteFields[i])) : core::tr("app.note_unknown");
        lines.push_back(text(kDetailIndent + core::tr(kLabels[i], {{"value", value}})) | dim);
    }
}

// `a`: listenin ilk satırı. Numara sütunu boş kalır, yol diğer satırlarla aynı hücrede başlar.
ftxui::Element name_row(const NewRowState& row, const ListCells& cells) {
    using namespace ftxui;
    constexpr int kCursorCells = 1;
    const int room = std::max(cells.inner_width - row_prefix_cells(cells) - kCursorCells, 1);
    InputCells content = input_text_cells(row.line(), {}, row.ghost(), room);
    Elements shown = {text(" "), text(std::string(static_cast<std::size_t>(cells.digits), ' ')), text(" "),
                      text("+ "), text(std::string(static_cast<std::size_t>(cells.icon_cells), ' '))};
    std::ranges::move(content.cells, std::back_inserter(shown));
    shown.push_back(filler());
    return hbox(std::move(shown)) | inverted;
}

// Ad satırının kapladığı satır: kendisi ve panele sarılan hatası.
int name_row_lines(const NewRowState& row, int inner_width) {
    if (row.error().empty())
        return 1;
    return 1 + (ftxui::string_width(row.error()) + inner_width - 1) / std::max(inner_width, 1);
}

ftxui::Element note_list(const TuiState& state, int height, int width, const NewRowState* new_row) {
    using namespace ftxui;
    const auto& rows = state.rows();
    const int inner_width = width - kPaneChromeCells;
    const int reserved = new_row ? name_row_lines(*new_row, inner_width) : 0;
    const auto visible = static_cast<std::size_t>(std::max(height - reserved, 1));
    constexpr int kIconCells = 3;  // ikon, taşma hücresi, boşluk
    const ListCells cells{digit_count(state.max_row_number()), inner_width, std::chrono::system_clock::now(),
                          new_row != nullptr, state.icon_style() == core::IconStyle::Off ? 0 : kIconCells};
    Elements lines;
    if (new_row) {
        lines.push_back(name_row(*new_row, cells));
        if (!new_row->error().empty())
            lines.push_back(paragraph(terminal_safe(new_row->error())) | color(Color::Red));
    }
    const std::size_t room = visible + lines.size();
    for (std::size_t i = state.list_top(visible); i < rows.size() && lines.size() < room; ++i) {
        state.note_row_drawn();
        lines.push_back(list_row(state, rows[i], i, cells));
        push_row_details(lines, state, rows[i], inner_width);
    }
    return vbox(std::move(lines));
}

// Tüm eşleşmeler (içerik araması + not içi arama) tek vurgu listesi.
std::vector<ByteRange> line_highlights(const TuiState& state, const Row* row, std::size_t index,
                                       std::string_view line) {
    std::vector<ByteRange> ranges = state.preview_find().ranges_on_line(index);
    if (row)
        for (const auto& hit : row->hits)
            if (hit.line == index + 1)
                for (const auto& [begin, end] : hit_ranges_in_line(hit, line))
                    ranges.push_back({begin, end});
    return ranges;
}

ftxui::Element look_styled(ftxui::Element element, const TextLook& look) {
    using namespace ftxui;
    if (look.bold)
        element = element | bold;
    if (look.italic)
        element = element | italic;
    if (look.strike)
        element = element | strikethrough;
    if (look.underline)
        element = element | underlined;
    if (look.dim)
        element = element | dim;
    return token_styled(std::move(element), look.token);
}

// REVIEW'daki hedefi olmayan [[bağlantı]]lar: GUI gibi soluk. Eksik listesi önizlemeyle aynı okumadan gelir.
std::vector<ByteRange> missing_links(const TuiState& state, const std::vector<WikiSpan>& links) {
    std::vector<ByteRange> ranges;
    const std::vector<std::string>& missing = state.link_panel().links().missing;
    if (missing.empty())
        return ranges;
    for (const WikiSpan& span : links)
        if (std::ranges::find(missing, span.target) != missing.end())
            ranges.push_back({span.begin, span.end});
    return ranges;
}

// Ters çizilen aralık: fare seçimi, yoksa seçili bağlantı, yoksa geçerli arama eşleşmesi.
std::optional<ByteRange> preview_inverted(const TuiState& state, std::size_t line, std::size_t line_size) {
    if (const auto selected = state.preview_selection_on_line(line, line_size))
        return selected;
    if (const auto link = state.wiki_selection_on_line(line))
        return link;
    return state.preview_find().current_range_on_line(line);
}

// Biçimli görüntü satırı: seçim ve arama vurgusu görünüşün önüne geçer (ham satırdaki gibi).
ftxui::Element paint_styled_line(std::string_view line, std::span<const StyledSpan> spans,
                                 std::optional<ByteRange> inverted_range,
                                 const std::vector<ByteRange>& highlights,
                                 const std::vector<ByteRange>& dimmed = {}) {
    using namespace ftxui;
    std::vector<std::size_t> cuts = {0, line.size()};
    for (const StyledSpan& span : spans) {
        cuts.push_back(span.begin);
        cuts.push_back(span.end);
    }
    const auto add_range = [&](const ByteRange& range) {
        cuts.push_back(std::min(range.begin, line.size()));
        cuts.push_back(std::min(range.end, line.size()));
    };
    if (inverted_range)
        add_range(*inverted_range);
    for (const ByteRange& range : highlights)
        add_range(range);
    for (const ByteRange& range : dimmed)
        add_range(range);
    std::ranges::sort(cuts);
    const auto [last, end] = std::ranges::unique(cuts);
    cuts.erase(last, end);
    Elements parts;
    std::size_t span_index = 0;
    const auto inside = [](const ByteRange& range, std::size_t at) {
        return at >= range.begin && at < range.end;
    };
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const std::size_t at = cuts[i];
        Element run = text(std::string(line.substr(at, cuts[i + 1] - at)));
        while (span_index < spans.size() && spans[span_index].end <= at)
            ++span_index;
        if (inverted_range && inside(*inverted_range, at))
            run = run | inverted;
        else if (std::ranges::any_of(highlights, [&](const ByteRange& range) { return inside(range, at); }))
            run = run | bold | color(Color::Yellow);
        else if (span_index < spans.size() && spans[span_index].begin <= at)
            run = look_styled(std::move(run), spans[span_index].look);
        if (std::ranges::any_of(dimmed, [&](const ByteRange& range) { return inside(range, at); }))
            run = run | dim;
        parts.push_back(std::move(run));
    }
    return parts.empty() ? text("") : hbox(std::move(parts));
}

// Biçimli görünüm: satırlar panele sığacak biçimde üretilmiştir, sarma yok. Satır numaraları kaynağa değil
// görüntüye ait: içerik aramasının satır vurguları burada geçerli değildir, not içi arama geçerlidir.
ftxui::Element rendered_pane(const TuiState& state, const Preview& preview, int height) {
    using namespace ftxui;
    const auto wanted = static_cast<std::size_t>(std::max(height, 1));
    Elements lines;
    for (std::size_t i = state.preview_scroll(); i < preview.lines.size() && lines.size() < wanted; ++i) {
        lines.push_back(paint_styled_line(
            preview.lines[i], preview.styled[i], preview_inverted(state, i, preview.lines[i].size()),
            state.preview_find().ranges_on_line(i), missing_links(state, state.wiki_spans_on(i))));
    }
    return vbox(std::move(lines));
}

// Görüntü satırlarını sona ekler; kaynak satırları `first_line` kadar kayar (parçanın notta başladığı satır).
// Seçim kopyası `selected_text` ile ham metindeki yoldan geçer.
void append_rendered(Preview& out, RenderedView view, std::size_t first_line) {
    out.line_starts.reserve(out.line_starts.size() + view.lines.size());
    // Parçanın bağlantı sıraları öncekilerin ardına kayar: tek geçişteki numaralamayla aynı.
    const std::size_t links_before = out.wiki_targets.size();
    for (std::vector<StyledSpan>& spans : view.spans)
        for (StyledSpan& span : spans)
            if (span.look.wiki_link != 0)
                span.look.wiki_link += links_before;
    std::ranges::move(view.wiki_targets, std::back_inserter(out.wiki_targets));
    for (std::size_t i = 0; i < view.lines.size(); ++i) {
        out.line_starts.push_back(out.text.size());
        out.text += view.lines[i];
        out.text += '\n';
        out.lines.push_back(std::move(view.lines[i]));
        out.styled.push_back(std::move(view.spans[i]));
        out.source_lines.push_back(first_line + view.source_lines[i]);
    }
}

Preview rendered_preview(RenderedView view, ViewKind kind) {
    Preview out;
    out.kind = Preview::Kind::Text;
    out.view_kind = kind;
    out.rendered = true;
    append_rendered(out, std::move(view), 0);
    return out;
}

// Klasör seçiliyken REVIEW: depo okunmaz, her şey bellekteki ağaçtan.
ftxui::Element folder_summary(const TuiState& state, const Row& row) {
    using namespace ftxui;
    const auto summary =
        app::folder_summary(state.listing(), row.path.string(), std::chrono::system_clock::now());
    if (!summary)
        return emptyElement();
    Elements lines = {text(terminal_safe(summary->path)) | bold, text(summary->counts),
                      text(core::tr("tui.folder_total_size", {{"value", summary->size}})),
                      text(core::tr("tui.meta_updated", {{"value", summary->updated}}))};
    if (!summary->recent.empty()) {
        lines.push_back(text(""));
        lines.push_back(text(core::tr("tui.folder_recent")) | dim);
    }
    for (const app::FolderSummary::Recent& note : summary->recent)
        lines.push_back(
            hbox({text("  " + terminal_safe(note.name)), filler(), text(" " + note.updated) | dim}));
    return vbox(std::move(lines));
}

// `cat` gibi: uzun satır kırpılmaz, panele sarılır. Kaydırma birimi mantıksal satırdır; sarma, satır kaçış
// baytları ve sekmeler editörle aynı yoldan geçer.
// Bir hücrenin en çok baytı: 4 baytlık kod noktası ve ardından sıfır genişlikli birleşen işaretler. Daha
// yoğun satırda (binlerce birleşen işaret) önek ekrandan erken biter: aşağıda boş satır kalır, metin bozulmaz.
constexpr std::size_t kMaxBytesPerCell = 16;

// `cells` hücreye sığabilecek önek, kod noktası sınırında.
std::string_view visible_prefix(std::string_view line, std::size_t cells) {
    std::size_t end = std::min(line.size(), cells * kMaxBytesPerCell);
    while (end < line.size() && end > 0 && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80)
        --end;
    return line.substr(0, end);
}

// Satır başına göre aralıklar yana kaydırmada atlanan bayt kadar sola kayar; tamamen atlananlar düşer.
std::vector<ByteRange> shift_ranges(std::vector<ByteRange> ranges, std::size_t skip) {
    if (skip == 0)
        return ranges;
    std::erase_if(ranges, [skip](const ByteRange& range) { return range.end <= skip; });
    for (ByteRange& range : ranges) {
        range.begin = range.begin > skip ? range.begin - skip : 0;
        range.end -= skip;
    }
    return ranges;
}

// Önekin dışındaki vurgu düşer, sınırı aşan kısa kesilir.
std::vector<ByteRange> clamp_ranges(std::vector<ByteRange> ranges, std::size_t size) {
    std::erase_if(ranges, [size](const ByteRange& range) { return range.begin >= size; });
    for (ByteRange& range : ranges)
        range.end = std::min(range.end, size);
    return ranges;
}

ftxui::Element preview_pane(const TuiState& state, const Preview& preview, int height, int columns,
                            SpellChecker* spell) {
    using namespace ftxui;
    if (const Row* folder = state.selected_folder_row())
        return folder_summary(state, *folder);
    switch (preview.kind) {
    case Preview::Kind::Empty:
        return emptyElement();
    case Preview::Kind::Binary:
        return text(core::tr("app.binary_preview")) | dim;
    case Preview::Kind::Error:
        return text(preview.error) | dim;
    case Preview::Kind::Text:
        break;
    }
    if (preview.rendered)
        return rendered_pane(state, preview, height);
    const Row* row = state.selected_row();
    const auto wanted = static_cast<std::size_t>(std::max(height, 1));
    Elements lines;
    for (std::size_t i = state.preview_scroll(); i < preview.lines.size() && lines.size() < wanted; ++i) {
        // Ham REVIEW satır içinde dikey kaydırmaz: satırın yana kaydırma ofsetinden (Shift+←/→) sonraki, kalan
        // ekran satırlarına sığan kısmı işlenir. Kare başına iş satır boyuyla değil görünen hücreyle orantılı
        // (20 MiB'lık tek satır 76 ms'ti). Durumun aralıkları satır başına göredir, atlanan bayt kadar kayar.
        const std::size_t room = (wanted - lines.size()) * static_cast<std::size_t>(std::max(columns, 1));
        const std::string_view full = preview.lines[i];
        const std::size_t skip = state.preview_skip(full);
        const std::string_view line = visible_prefix(full.substr(skip), room);
        const EditorBuffer buffer(std::string(line), terminal_cell_width);
        const std::vector<ByteRange> ranges = clamp_ranges(
            shift_ranges(line_highlights(state, row, i, full.substr(0, skip + line.size())), skip), line.size());
        std::vector<SyntaxSpan> spans;
        if (preview.language != SyntaxLanguage::Plain) {
            const SyntaxState entry = preview.syntax.entry_state(
                preview.language, i,
                [&](std::size_t index) { return std::string_view(preview.lines[index]); },
                preview.highlight_line_bytes);
            highlight_line(preview.language, line, entry, &spans, preview.highlight_line_bytes);
        }
        const std::vector<ByteRange> misspelled =
            spell && preview.spell_checked ? spell->misspelled(line) : std::vector<ByteRange>{};
        const std::vector<ByteRange> missing = missing_links(state, wiki_spans(line));
        std::optional<ByteRange> inverted;
        if (const auto range = preview_inverted(state, i, full.size()))
            if (const auto kept = shift_ranges({*range}, skip); !kept.empty())
                inverted = kept.front();
        if (inverted && inverted->begin >= line.size())
            inverted.reset();
        else if (inverted)
            inverted->end = std::min(inverted->end, line.size());
        const RowPaint paint{.cursor = std::nullopt,
                             .selection = inverted,
                             .highlights = ranges,
                             .syntax = spans,
                             .syntax_origin = 0,
                             .misspelled = misspelled,
                             .dimmed = missing};
        for (const VisualRow& visual : buffer.rows_of_line(0, columns))
            if (lines.size() < wanted)
                lines.push_back(paint_row(buffer, visual, paint));
    }
    return vbox(std::move(lines));
}

// Kaydet diyaloğunun iç genişliği; en küçük pencerede (60) kenarlara sığar.
constexpr int kDialogWidth = 52;
constexpr int kLabelWidth = 9;     // "Klasör:" + boşluk
constexpr int kPaletteWidth = 58;  // en küçük pencerede (60) kenarlara sığar

ftxui::Element dialog_field(std::string label, const std::string& value, bool focused,
                            ftxui::Element extra = ftxui::emptyElement()) {
    using namespace ftxui;
    Element name = text(std::move(label) + ":") | size(WIDTH, EQUAL, kLabelWidth);
    if (focused)
        name = name | bold;
    Elements cells = {std::move(name), text(value)};
    if (focused)
        cells.push_back(text("▏"));
    cells.push_back(text("  "));
    cells.push_back(std::move(extra));
    return hbox(std::move(cells));
}

ftxui::Element discard_choice(const std::string& label, bool selected) {
    using namespace ftxui;
    Element button = text("[ " + label + " ]");
    return selected ? button | inverted : button;
}

// Onay penceresinin gövdesi: soru + [Vazgeç] [Evet] düğmeleri. Silme ve not atma onayı aynı görünümü
// paylaşır; "Vazgeç" (tui.discard_no) her ikisinde de varsayılan.
ftxui::Elements confirm_body(const std::string& question, const std::string& yes_label, bool yes_selected) {
    using namespace ftxui;
    return {paragraph(question) | bold, text(""),
            hbox({discard_choice(core::tr("tui.discard_no"), !yes_selected), text("   "),
                  discard_choice(yes_label, yes_selected)}) |
                center};
}

ftxui::Element save_dialog_box(const SaveDialogState& dialog) {
    using namespace ftxui;
    Elements body;
    if (dialog.confirming_discard()) {
        body = confirm_body(core::tr("tui.confirm_discard"), core::tr("tui.discard_yes"),
                            dialog.discard_selected());
    } else {
        using Field = SaveDialogState::Field;
        body.push_back(
            dialog_field(core::tr("app.field_name"), dialog.name(), dialog.focus() == Field::Name));
        body.push_back(
            dialog_field(core::tr("app.field_folder"), dialog.folder(), dialog.focus() == Field::Folder));
        if (dialog.focus() == Field::Folder) {
            const auto& suggestions = dialog.suggestions();
            for (std::size_t i = 0; i < suggestions.size(); ++i) {
                const bool picked = static_cast<int>(i) == dialog.selected_suggestion();
                Element line = hbox(
                    {text(std::string(kLabelWidth, ' ') + (picked ? "▸ " : "  ")), text(suggestions[i])});
                body.push_back(picked ? line | inverted : line | dim);
            }
        }
        body.push_back(dialog_field(core::tr("app.field_type"), dialog.type(), dialog.focus() == Field::Type,
                                    text(dialog.type_label()) | dim));
        if (!dialog.error().empty())
            body.push_back(paragraph(dialog.error()) |
                           color(Color::Red));  // temp yolu uzun olabilir: sarılır
        body.push_back(text(""));
        body.push_back(text(core::tr("tui.save_hints")) | dim);
    }
    return window(text(" " + dialog.title() + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

ftxui::Element confirm_box(const ConfirmState& confirm) {
    using namespace ftxui;
    const std::string title = confirm.title().empty() ? core::tr("tui.delete_title") : confirm.title();
    return window(text(" " + title + " ") | bold,
                  vbox(confirm_body(confirm.message(),
                                    confirm.yes().empty() ? core::tr("tui.delete_yes") : confirm.yes(),
                                    confirm.yes_selected()))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

// Yardım: katalogdaki satırlar kaydırılır; görünür satır sayısı pencere yüksekliğinden (kenarlık 2 + boşluk
// ve ipucu 2) çıkar, böylece 60x15'te de ipucu satırı görünür kalır.
ftxui::Element help_box(const HelpState& help, int window_height) {
    using namespace ftxui;
    const std::vector<std::string> lines = help_lines();
    const auto visible = static_cast<std::size_t>(std::max(window_height - 4, 1));
    const std::size_t first = std::min(help.scroll(), lines.size() > visible ? lines.size() - visible : 0);
    Elements body;
    for (std::size_t i = first; i < lines.size() && i < first + visible; ++i)
        body.push_back(text(lines[i]));
    body.push_back(text(""));
    body.push_back(text(core::tr("tui.help_hints")) | dim);
    return window(text(" " + core::tr("tui.help_title") + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

// Toplu işlemin sonucu: sığmayan satırlar sayıyla söylenir, ayrıntı kaybolmaz: başarısız öğe yerindedir.
ftxui::Element summary_box(const std::vector<std::string>& lines, int window_height) {
    using namespace ftxui;
    constexpr int kChromeLines = 6;  // kenarlık, boşluk, ipucu, "n daha"
    const auto visible = static_cast<std::size_t>(std::max(window_height - kChromeLines, 1));
    Elements body;
    for (std::size_t i = 0; i < lines.size() && i < visible; ++i)
        body.push_back(paragraph(lines[i]));
    if (lines.size() > visible)
        body.push_back(text(core::tr("tui.batch_more", {{"count", std::to_string(lines.size() - visible)}})) |
                       dim);
    body.push_back(text(""));
    body.push_back(text(core::tr("tui.batch_hints")) | dim);
    return window(text(" " + core::tr("tui.batch_title") + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

// Ayarlar formu: etiket / değer iki sütun. Kaydet düğmesi, hata ve ipucu satırı sabit kalır; yalnız 12 alan
// satırı kayar (60x15'te 10 satır görünür), odaklı satır her zaman görünür.
constexpr int kSettingsWidth = 58;
constexpr int kSettingsInnerWidth = kSettingsWidth - 2;  // kenarlık
constexpr int kSettingsLabelWidth = 22;
constexpr std::size_t kSettingsValueCells = 30;  // uzun yol/URL'de sondaki kısım görünür (imleç orada)
constexpr int kSettingsFixedLines = 7;           // kenarlık 2 + 3 düğme satırı + hata (ilk satırı) + ipucu

ftxui::Element settings_row(const SettingsState::Row& row, bool focused) {
    using namespace ftxui;
    std::string shown;
    // Değer config.toml'dan (ör. notes_dir yolu) gelir: denetim karakteri ham basılmaz.
    switch (row.kind) {
    case SettingsState::Kind::Toggle:
        shown = row.value == "true" ? "[x]" : "[ ]";
        break;
    case SettingsState::Kind::Choice:
        shown = "‹ " + tail_cells(terminal_safe(row.value), kSettingsValueCells) + " ›";
        break;
    case SettingsState::Kind::Text:
    case SettingsState::Kind::Token:  // değer modelde zaten maskelenmiş noktalardır
        shown = tail_cells(terminal_safe(row.value), kSettingsValueCells);
        break;
    }
    if (focused && (row.kind == SettingsState::Kind::Text || row.kind == SettingsState::Kind::Token))
        shown += "▏";
    Element value = text(shown);
    if (!row.error.empty())
        value = value | color(Color::Red);
    if (!row.note.empty())
        value = hbox({std::move(value), text("  " + row.note) | dim});
    Element line =
        hbox({text(settings_label(row)) | size(WIDTH, EQUAL, kSettingsLabelWidth), std::move(value)});
    return focused ? line | inverted : line;
}

const char* hints_key(const TuiState& state) {
    if (state.trash_view())
        return "tui.key_hints_trash";
    if (state.focus() == Focus::Preview)
        return "tui.key_hints_preview";
    if (state.focus() == Focus::Links)
        return "tui.key_hints_links";
    if (state.focus() == Focus::Inspector)
        return "inspector.hints";
    // Boş alt klasörde de `a` ve üst klasör söylenir; boş kök liste ipuçlarında kalır.
    const bool empty_folder =
        state.in_folder_view() && state.rows().empty() && !state.current_folder().empty();
    const bool on_folder = state.selected_folder_row() || empty_folder;
    return on_folder ? "tui.key_hints_folder" : "tui.key_hints_list";
}

// Toplamlar: süzgeç yokken eşleşme sayısı gürültüdür. Sayfa yalnız konum bilgisidir, kaydırma sanaldır.
std::string totals_text(const TuiState& state, int panel_height) {
    const auto page_rows = static_cast<std::size_t>(std::max(panel_height, 1));
    const std::size_t shown = state.rows().size();
    std::string totals = core::tr("tui.totals", {{"total", count_text("tui.count_notes", state.total())}});
    if (state.in_folder_view())
        totals.insert(0, core::tr("tui.totals_folder", {{"count", count_text("tui.count_items", shown)}}));
    if (!state.query().empty())
        totals += core::tr("tui.totals_matches", {{"count", count_text("tui.count_matches", shown)}});
    return totals + core::tr("tui.totals_page", {{"page", std::to_string(state.selected() / page_rows + 1)},
                                                 {"pages", std::to_string(std::max<std::size_t>(
                                                               (shown + page_rows - 1) / page_rows, 1))}});
}

// Yol girdisi: imleç metnin içindedir; uzun yolda imleç çevresi görünür kalır.
ftxui::Element rename_bar(const RenameState& rename, int width) {
    if (rename.is_property()) {
        const std::string label =
            rename.property_key().empty()
                ? core::tr("inspector.key_prompt")
                : core::tr("inspector.value_prompt", {{"key", terminal_safe(rename.property_key())}});
        return input_line_bar(label, rename.line(), {}, rename.ghost(), core::tr("tui.rename_hints"), width);
    }
    if (rename.is_relation()) {
        // Yön işareti kutunun yerine: → yönlü, — yönsüz (Ctrl+D değiştirir).
        const std::string label = core::tr(
            rename.renaming() ? "tui.relation_rename_label" : "tui.relation_label",
            {{"from", terminal_safe(rename.from().string())},
             {"arrow", rename.directed() ? "→" : "—"},
             {"to", terminal_safe(rename.relation_to().string())}});
        return input_line_bar(label, rename.line(), {}, rename.ghost(),
                              core::tr(rename.renaming() ? "tui.relation_rename_hints" : "tui.relation_hints"),
                              width);
    }
    const char* label = rename.is_link()      ? "tui.link_label"
                        : rename.is_move_to() ? "tui.move_to_label"
                        : rename.is_export()  ? "tui.export_label"
                        : rename.is_import()  ? "tui.import_label"
                        : rename.is_view()
                            ? (rename.view_query().empty() ? "tui.view_remove_label" : "tui.view_save_label")
                            : "tui.rename_label";
    return input_line_bar(core::tr(label), rename.line(), {}, rename.ghost(), core::tr("tui.rename_hints"),
                          width);
}

// Yeri belli hata: "6: sync değeri geçersiz: nope ...". Metin sayılmadığını kullanıcı burada görür.
std::string query_error_text(const TuiState& state) {
    if (!state.query_error())
        return {};
    return core::tr("tui.query_error",
                    {{"column", std::to_string(state.query_error()->begin + 1)},
                     {"detail", terminal_safe(core::format_error(state.query_error()->error))}});
}

ftxui::Element search_bar(const TuiState& state, int width) {
    const bool global = state.mode() == Mode::Global;
    return input_line_bar(global ? core::tr("tui.global_label") : ">", state.search_line(),
                          search_spans(state.query()), state.search_ghost(),
                          core::tr(global ? "tui.key_hints_global" : "tui.key_hints_search"), width, {},
                          query_error_text(state));
}

ftxui::Element preview_find_bar(const TuiState& state, int width) {
    const PreviewFind& find = state.preview_find();
    return input_bar(core::tr("tui.find_label"), find.query(), core::tr("tui.note_find_hints"), width,
                     match_label(find.current(), find.count()));
}

constexpr std::string_view kFilterPrompt = "> ";

// Süzgeç ya da not içi arama etkin kaldıysa üst çubukta yeri yok: kısayolların başında görünür. Uzun sorgunun
// sonu `room` hücreye kısalır, öncelikli ipuçları çubuktan düşmesin.
std::string active_filter(const TuiState& state, int room) {
    if (state.focus() == Focus::Preview)
        return match_label(state.preview_find().current(), state.preview_find().count());
    if (state.query().empty())
        return {};
    const int query_room = std::max(room - static_cast<int>(kFilterPrompt.size()), 1);
    return std::string(kFilterPrompt) + tail_cells(state.query(), static_cast<std::size_t>(query_room));
}

// Seçim sayısı ve bekleyen yank: klasör değişse de alt satırda görünür.
std::string selection_text(const TuiState& state) {
    std::string out;
    if (state.marked_count() > 0)
        out = core::tr("app.marked_count", {{"count", std::to_string(state.marked_count())}});
    if (const auto& yank = state.yank()) {
        if (!out.empty())
            out += kHintSeparator;
        out += core::tr("tui.yank_copy", {{"count", std::to_string(yank->items.size())}});
    }
    return out;
}

// Katalogdaki ilk kPriorityHints ipucunun kapladığı hücre.
int priority_hints_cells(std::string_view hints) {
    std::size_t end = 0;
    for (int i = 0; i < kPriorityHints && end != std::string_view::npos; ++i)
        end = hints.find(kHintSeparator, end == 0 ? 0 : end + kHintSeparator.size());
    return ftxui::string_width(std::string(hints.substr(0, end)));
}

std::vector<std::string_view> split_hints(std::string_view hints) {
    std::vector<std::string_view> out;
    for (std::size_t start = 0; start < hints.size();) {
        const std::size_t end = std::min(hints.find(kHintSeparator, start), hints.size());
        out.push_back(hints.substr(start, end - start));
        start = end + kHintSeparator.size();
    }
    return out;
}

constexpr std::string_view kHelpKey = "F1 ";

// Katalogdan `F1` yardım ipucunu ayırır (çubuk onu sağa sabitler): {kalan ipuçları, yardım ipucu}. Katalog
// yine tek kaynaktır: ipucunun metni ve dili oradan gelir.
std::pair<std::string, std::string> split_help_hint(std::string_view hints) {
    std::string rest;
    std::string help;
    for (const std::string_view hint : split_hints(hints)) {
        if (help.empty() && hint.starts_with(kHelpKey)) {
            help = hint;
            continue;
        }
        if (!rest.empty())
            rest += kHintSeparator;
        rest += hint;
    }
    return {rest, help};
}

// "tuş açıklama": tuş kalın ve kip renginde, açıklama varsayılan renkte; renksiz terminalde kalınlık ayırır.
ftxui::Elements hint_cells(std::string_view hints, ftxui::Color key_color) {
    using namespace ftxui;
    Elements cells;
    for (const std::string_view hint : split_hints(hints)) {
        if (!cells.empty())
            cells.push_back(text(std::string(kHintSeparator)));
        const std::size_t space = std::min(hint.find(' '), hint.size());
        cells.push_back(text(std::string(hint.substr(0, space))) | bold | color(key_color));
        if (space < hint.size())
            cells.push_back(text(std::string(hint.substr(space))));
    }
    return cells;
}

ftxui::Element bottom_bar(const TuiState& state, const StatusLine& status, const NewRowState* new_row,
                          const RenameState* rename, int width, ftxui::Color key_color) {
    using namespace ftxui;
    if (rename) {
        if (rename->error().empty())
            return rename_bar(*rename, width);
        return vbox({text(terminal_safe(rename->error())) | color(Color::Red), rename_bar(*rename, width)});
    }
    if (new_row)
        return text(fit_hints(core::tr("tui.new_row_hints"), width)) |
               dim;  // girdi listede, burada yalnız tuşlar
    if (state.focus() == Focus::Search)
        return search_bar(state, width);
    if (state.focus() == Focus::PreviewFind)
        return preview_find_bar(state, width);
    if (state.focus() == Focus::SortMenu)
        return text(fit_hints(core::tr("tui.sort_menu"), width)) | bold;
    if (state.awaiting_prefix())
        return text(fit_hints(core::tr("tui.g_hints"), width)) | bold;
    // İleti ve ipucu çoğu kez yol taşır; her kaynağı ayrı ayrı arındırmak yerine burada tek yerde.
    const auto& notices = status.notices.items();
    const bool shown_as_notice = !notices.empty() && notices.back().text == status.message;
    if (!status.message.empty() && !shown_as_notice)
        return text(terminal_safe(status.message)) | bold;
    if (!status.notice.empty())
        return text(fit_hints(terminal_safe(status.notice), width)) | dim;
    const std::string hints = core::tr(hints_key(state));
    const auto [rest, help] = split_help_hint(hints);
    const int separator = static_cast<int>(kHintSeparator.size());
    const int pinned = help.empty() ? 0 : string_width(help) + separator;
    const std::string selection = selection_text(state);
    const std::string filter = active_filter(state, width - priority_hints_cells(rest) - separator -
                                                        string_width(selection) - pinned);
    if (filter.empty() && selection.empty())
        return hint_bar(hints, width, key_color);
    const std::string shown = selection +
                              (!filter.empty() && !selection.empty() ? std::string(kHintSeparator) : "") +
                              filter + std::string(kHintSeparator);
    return hbox({text(shown) | bold, hint_bar(hints, width - string_width(shown), key_color) | flex});
}

ftxui::Element settings_box(const SettingsState& settings, int window_height) {
    using namespace ftxui;
    Elements body;
    if (const std::optional<app::CloseChoice> leave = settings.leave_prompt()) {
        body = {paragraph(core::tr("tui.settings_confirm_leave")) | bold, text(""),
                hbox({discard_choice(core::tr("app.settings_save"), *leave == app::CloseChoice::Save), text("   "),
                      discard_choice(core::tr("tui.discard_yes"), *leave == app::CloseChoice::Discard), text("   "),
                      discard_choice(core::tr("tui.discard_no"), *leave == app::CloseChoice::Cancel)}) |
                    center};
    } else if (const ConfirmState* replace = settings.replace_confirm()) {
        // Mevcut origin `.git/config`'ten gelir: güvenilmez.
        body = confirm_body(terminal_safe(replace->message()), core::tr("tui.replace_yes"),
                            replace->yes_selected());
    } else {
        // Token satırı (varsa) 12 satırın ardındadır; odak/kaydırma aynı sıra numarasıyla yürür.
        std::vector<SettingsState::Row> rows = settings.rows();
        if (settings.token_visible())
            rows.push_back(settings.token_row());
        std::string error = settings.error();
        for (const auto& row : rows)
            if (error.empty())
                error = row.error;
        // Hata dosya yolunu ve toml++ ayrıntısını (sonda: "nothing was saved", satır/sütun) taşır: denetim
        // karakteri ham basılmaz, kutu genişliğinde sarılır. Alan satırlarıyla aynı yeri paylaşır, odaklı satıra
        // yer bırakır; sığmazsa baştan kısalır, sondaki ileti ve konum görünür kalır.
        const int room = std::max(window_height - kSettingsFixedLines, 1);
        const std::string safe_error = terminal_safe(error);
        std::vector<std::string> error_lines = wrap_lines(safe_error, kSettingsInnerWidth, terminal_cell_width);
        if (std::cmp_greater(error_lines.size(), room)) {
            const auto budget = static_cast<std::size_t>(room) * kSettingsInnerWidth;
            error_lines = wrap_lines(tail_cells(safe_error, budget), kSettingsInnerWidth, terminal_cell_width);
            if (std::cmp_greater(error_lines.size(), room))  // sözcük sınırında kırmak birkaç hücre harcar
                error_lines.erase(error_lines.begin(), error_lines.end() - room);
        }
        const auto visible = static_cast<std::size_t>(std::max(room + 1 - static_cast<int>(error_lines.size()), 1));
        const std::size_t focus_row = std::min(settings.focus(), rows.size() - 1);
        const std::size_t first = focus_row < visible ? 0 : focus_row + 1 - visible;
        for (std::size_t i = first; i < rows.size() && i < first + visible; ++i)
            body.push_back(settings_row(rows[i], settings.focus() == i));
        // Güncelleme denetimi (palet `update` ile aynı akış) ve kurulu sürüm; sürüm yanında durur ki ne
        // denetlendiği görünsün.
        Element update = text("[ " + core::tr("command.update") + " ]");
        body.push_back(hbox({settings.on_update() ? update | inverted : update, text("  "),
                             text(app::version_label()) | dim}));
        Element test = text("[ " + core::tr("tui.settings_test") + " ]");
        body.push_back(hbox({settings.on_test() ? test | inverted : test, text(" "),
                             text(terminal_safe(settings.test_result())) | dim | xflex_shrink}));
        Element save = text("[ " + core::tr("app.settings_save") + " ]");
        body.push_back((settings.on_button() ? save | inverted : save) | center);
        for (std::string& line : error_lines)
            body.push_back(text(std::move(line)) | color(Color::Red));
        body.push_back(
            text(core::tr(settings.on_token() ? "tui.settings_token_hints" : "tui.settings_hints")) | dim);
    }
    return window(text(" " + core::tr("tui.settings_title") + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kSettingsWidth) | clear_under;
}

// Paletin kapladığı satırlar dışında kalan: kenarlık 2, girdi, ipucu.
constexpr int kPaletteChromeLines = 4;
constexpr int kPaletteRows = 8;
constexpr int kPaletteTitleCells = 30;  // uzun yolun sonu görünür

// Komutların üzerinde çalışacağı şey: not adı güvenilmezdir.
std::string palette_title(const TuiState& state) {
    std::string target;
    if (state.marked_count() > 0)
        target = core::tr("app.marked_count", {{"count", std::to_string(state.marked_count())}});
    else if (const Row* row = state.selected_entry())
        target = tail_cells(terminal_safe(row->path.string()), kPaletteTitleCells);
    const std::string title = core::tr("app.palette_title");
    return " " + (target.empty() ? title : title + ": " + target) + " ";
}

ftxui::Element palette_row(const PaletteRow& row, bool selected) {
    using namespace ftxui;
    const Command& command = *row.command;
    std::string label = core::tr(command.label_key);
    if (row.reason)
        label += " (" + core::tr(row.reason) + ")";
    std::string keys(command.keys);
    if (!command.alias.empty())
        keys += " " + std::string(command.alias);
    // GUI paletiyle aynı bilgi sırası: okunan etiket solda; kararlı kimlik ve tuş sağda, soluk. Uzun etiket
    // kısalır, kimlik ve tuş sağ kenarda kalır.
    const std::string right =
        " " + std::string(command.id) + (keys.empty() ? std::string{} : "  " + keys) + " ";
    Element line = hbox({text(label) | flex, text(right) | dim});
    if (row.reason)
        line = line | dim;
    return selected ? line | inverted : line;
}

// Seçili satır pencere içinde kalır; satır uzunsa kırpılır, neden ipucu satırında yine görünür.
ftxui::Element palette_box(const TuiState& state, int window_height) {
    using namespace ftxui;
    const std::vector<PaletteRow> rows = state.palette_rows();
    const Command* selected = state.palette_selected();
    const auto found = std::ranges::find(rows, selected, &PaletteRow::command);
    const auto selected_at = static_cast<std::size_t>(found - rows.begin());
    const auto visible =
        static_cast<std::size_t>(std::clamp(window_height - kPaletteChromeLines, 1, kPaletteRows));
    const std::size_t first = selected_at < visible ? 0 : selected_at + 1 - visible;

    InputCells input = input_text_cells(state.palette_line(), {}, {}, kPaletteWidth - kFrameCells - 2);
    Elements filter = {text(std::string(TuiState::kPaletteKey) + " ") | bold};
    std::ranges::move(input.cells, std::back_inserter(filter));
    Elements body = {hbox(std::move(filter))};
    for (std::size_t i = first; i < rows.size() && i < first + visible; ++i)
        body.push_back(palette_row(rows[i], i == selected_at));
    if (rows.empty())
        body.push_back(text(core::tr("tui.editor_not_found")) | dim);
    const bool blocked = found != rows.end() && found->reason;
    body.push_back(text(core::tr(blocked ? found->reason : "tui.palette_hints")) | dim);
    return window(text(palette_title(state)) | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kPaletteWidth) | clear_under;
}

// Güncelleme penceresi (app::UpdateController::view): satırlar, release sayfası, düğmeler; metin
// safe_update_view'dan. Adres boşluksuzdur, paragraph onu bölemez: kutu genişliğinde parçalanır.
ftxui::Element update_box(const UpdateDialogState& update) {
    using namespace ftxui;
    const app::UpdateView view = safe_update_view(update.view());
    Elements body;
    for (const std::string& line : view.lines)
        body.push_back(paragraph(line));
    if (!view.link.empty()) {
        constexpr auto kLinkCells = static_cast<std::size_t>(kDialogWidth - 2);  // kenarlık
        for (std::size_t start = 0; start < view.link.size(); start += kLinkCells)
            body.push_back(text(view.link.substr(start, kLinkCells)) | underlined);
    }
    body.push_back(text(""));
    Elements buttons;
    for (const app::UpdateAction action : view.actions) {
        if (!buttons.empty())
            buttons.push_back(text("   "));
        buttons.push_back(
            discard_choice(core::tr(app::update_action_label(action)), action == update.selected()));
    }
    body.push_back(hbox(std::move(buttons)) | center);
    body.push_back(text(core::tr("tui.update_hints")) | dim);
    return window(text(" " + core::tr("update.ui_title") + " ") | bold, vbox(std::move(body))) |
           size(WIDTH, EQUAL, kDialogWidth) | clear_under;
}

// Birden çoğu açıksa öncelik: yardım > onay > diyalog > güncelleme > ayarlar. Güncelleme penceresi Ayarlar'ın
// "Güncellemeleri denetle" düğmesiyle formun üstünde açılır; kapanınca form yerinde kalır.
ftxui::Element open_modal(const TuiState& state, const HelpState* help, const ConfirmState* confirm,
                          const SaveDialogState* dialog, const SettingsState* settings,
                          const UpdateDialogState* update, int window_height) {
    if (help)
        return help_box(*help, window_height);
    if (confirm)
        return confirm_box(*confirm);
    if (dialog)
        return save_dialog_box(*dialog);
    if (update)
        return update_box(*update);
    if (settings)
        return settings_box(*settings, window_height);
    if (state.palette_open())
        return palette_box(state, window_height);
    return state.summary().empty() ? nullptr : summary_box(state.summary(), window_height);
}

// Açık Tab listesi: tuşlar bu girdiye gittiği için tek olabilir.
const InputLine::Popup* open_input_popup(const TuiState& state, const NewRowState* new_row,
                                         const RenameState* rename) {
    if (rename)
        return rename->line().popup();
    if (new_row)
        return new_row->line().popup();
    if (state.map_view() && state.map_input_kind())  // haritanın süzgeç satırı (F2)
        return state.map_input().popup();
    return state.focus() == Focus::Search ? state.search_line().popup() : nullptr;
}

// Editörün sorusu ya da girdinin açık listesi: üst pencere yoksa.
ftxui::Element inner_modal(const TuiState& state, const EditorSession* editing, const NewRowState* new_row,
                           const RenameState* rename) {
    if (editing)
        if (ftxui::Element modal = editor_modal(*editing))
            return modal;
    const InputLine::Popup* popup = open_input_popup(state, new_row, rename);
    return popup ? input_popup_box(*popup) : nullptr;
}

const EditorSession* active_editor(const TuiState& state, const EditorSession* editor) {
    return state.focus() == Focus::Editor ? editor : nullptr;
}

// Ayırıcı: bölüm taşıyorsa konum (başlıklar sayılmaz), sonra notun etiketleri. Etiket güvenilmezdir.
ftxui::Element links_separator(const LinkPanel& panel, std::size_t visible) {
    using namespace ftxui;
    Elements cells;
    if (panel.lines().size() > visible)
        cells.push_back(
            text(std::to_string(panel.selected_ordinal()) + "/" + std::to_string(panel.item_count()) + " ") |
            dim);
    if (!panel.links().tags.empty()) {
        std::string tags;
        for (const std::string& tag : panel.links().tags)
            tags += "#" + terminal_safe(tag) + " ";
        cells.push_back(text(std::move(tags)));
    }
    if (cells.empty())
        return separator();
    cells.push_back(separatorCharacter("─") | flex);
    return hbox(std::move(cells));
}

// Ad güvenilmezdir ve panelden uzun olabilir: sonu (dosya adı) görünür kalır.
ftxui::Element link_row(const TuiState& state, const core::LinkLine& line, int columns) {
    using namespace ftxui;
    const core::NoteLinks& links = state.link_panel().links();
    if (line.is_header)
        return text(core::tr(core::link_group_key(line.group), {{"count", std::to_string(line.index)}})) |
               bold;
    const bool related = line.group == core::LinkGroup::Related;
    // İlişkide yön oku ve ad da (F1): "↗ b.md — uses"; ikon yine yoldan.
    const std::string path = terminal_safe(core::link_text(links, line));
    const std::string safe = related ? terminal_safe(app::related_row_text(links.related[line.index])) : path;
    const auto room = static_cast<std::size_t>(std::max(columns - 8, 1));
    Elements cells = {text("  ")};
    if (line.group == core::LinkGroup::Missing) {
        // Renk tek sinyal değildir: köşeli ayraçlar çözülmemiş metni söyler.
        cells.push_back(text("[[" + tail_cells(safe, room) + "]]") | dim);
    } else {
        if (state.icon_style() != core::IconStyle::Off) {
            const core::FileIcon icon = core::file_icon(path);
            cells.push_back(text(core::icon_text(icon, state.icon_style())) | color(icon_color(icon.color)));
            cells.push_back(text(std::string(kIconGap)));
        }
        cells.push_back(text(tail_cells(safe, room)));
    }
    return hbox(std::move(cells));
}

// Yalnız görünen satırlar çizilir: binlerce bağlantıda kare maliyeti sabittir.
ftxui::Element links_section(const TuiState& state, int rows, int columns) {
    using namespace ftxui;
    const LinkPanel& panel = state.link_panel();
    const auto visible = static_cast<std::size_t>(rows);
    const std::size_t top = panel.top(visible);
    const bool focused = state.focus() == Focus::Links;
    Elements lines = {links_separator(panel, visible)};
    for (std::size_t i = top; i < panel.lines().size() && i < top + visible; ++i) {
        Element row = link_row(state, panel.lines()[i], columns);
        lines.push_back(focused && i == panel.selected() ? std::move(row) | inverted : std::move(row));
    }
    return vbox(std::move(lines));
}

// Denetçi (F4): ayırıcıda başlık (konu), sonra `anahtar = değer  tür`; salt okunur satır soluk. Metin
// app::Inspector'da temizlenmiş ve kesilmiştir; panele sığmayan değer sağdan kısalır. Yalnız görünen
// satırlar.
ftxui::Element inspector_section(const TuiState& state, int rows, int columns) {
    using namespace ftxui;
    const app::Inspector& inspector = state.inspector();
    const auto room = static_cast<std::size_t>(std::max(columns - 4, 1));
    Elements lines = {hbox({text(tail_cells(terminal_safe(inspector.title()), room / 2) + " ") | bold,
                            separatorCharacter("─") | flex})};
    if (!inspector.error().empty()) {
        lines.push_back(paragraph(terminal_safe(inspector.error())) | color(Color::Red));
    } else if (inspector.rows().empty()) {
        lines.push_back(text(core::tr("inspector.empty")) | dim);
    }
    const auto visible = static_cast<std::size_t>(std::max(rows, 0));
    const std::size_t top = state.inspector_top(visible);
    for (std::size_t i = top; i < inspector.rows().size() && i < top + visible; ++i) {
        const app::InspectorRow& row = inspector.rows()[i];
        std::string line =
            row.kind == app::InspectorRow::Kind::Label ? row.key_text : row.key_text + " = " + row.value_text;
        line += "  (" + core::tr("inspector.type_" + row.type) + ")";
        Element cell = text(head_cells(line, static_cast<int>(room)));
        if (row.read_only)
            cell = std::move(cell) | dim;
        lines.push_back(i == inspector.selected() ? std::move(cell) | inverted : std::move(cell));
    }
    return vbox(std::move(lines));
}

ftxui::Element right_pane(const TuiState& state, const Preview& preview, const EditorSession* editing,
                          int height, int columns, bool show_cursor, SpellChecker* spell) {
    using namespace ftxui;
    if (editing && editing->is_open())
        return editor_pane(*editing, show_cursor, spell);
    if (const int inspector = state.inspector_reserved_rows(height); inspector > 0)
        return vbox({preview_pane(state, preview, height - inspector, columns, spell) | yflex,
                     inspector_section(state, inspector - 1, columns)});
    const int reserved = state.links_reserved_rows(height);
    Element body = preview_pane(state, preview, height - reserved, columns, spell);
    if (reserved == 0)
        return body;
    return vbox({std::move(body) | yflex, links_section(state, reserved - 1, columns)});
}

std::optional<PaneMode> right_mode(const TuiState& state, const Preview& preview,
                                   const EditorSession* editing) {
    if (editing && editing->is_open())
        return editor_mode(*editing);
    if (!right_pane_focused(state.focus()))
        return std::nullopt;
    PaneMode mode = review_mode(preview);
    const Row* row = state.selected_row();
    if (row && state.icon_style() != core::IconStyle::Off)
        mode.label = core::icon_text(core::file_icon(row->path.string()), state.icon_style()) +
                     std::string(kIconGap) + mode.label;
    return mode;
}

// Yakınlaştırmada yalnız odaklı panel çizilir.
ftxui::Element panes(const TuiState& state, const Preview& preview, const EditorSession* editing,
                     const NewRowState* new_row, ftxui::Dimensions window, int height, bool show_cursor,
                     SpellChecker* spell, std::span<const TabLabel> tabs) {
    using namespace ftxui;
    const PaneGeometry geometry = state.layout().geometry(window.dimx, right_pane_focused(state.focus()));
    Elements cells;
    if (geometry.list_visible) {
        // Harita listenin yerinde (SPEC-tui "Harita"); ad satırı açıkken liste görünür kalır.
        Element content = state.map_view() && !new_row
                              ? map_pane(state, geometry.list_width - kPaneChromeCells, height)
                              : note_list(state, height, geometry.list_width, new_row);
        cells.push_back(framed_pane(std::move(content) | flex, state.focus() == Focus::List
                                                                   ? std::optional(list_mode())
                                                                   : std::nullopt) |
                        size(WIDTH, EQUAL, geometry.list_width));
    }
    if (geometry.right_visible) {
        const std::optional<PaneMode> mode = right_mode(state, preview, editing);
        // Kenarın iki köşesi, etiket payı ve etiketin iki boşluğu.
        constexpr int kLabelChrome = 2 + kFrameLabelOffset + 2;
        const int room = geometry.right_width - kLabelChrome - (mode ? ftxui::string_width(mode->label) : 0);
        cells.push_back(
            framed_pane(right_pane(state, preview, editing, height,
                                   editor_viewport(window, state.layout()).columns, show_cursor, spell) |
                            flex,
                        mode, tab_strip_text(tabs, room)) |
            flex);
    }
    return hbox(std::move(cells));
}

}  // namespace

ftxui::Element input_bar(const std::string& label, const std::string& value, const std::string& hints,
                         int width, const std::string& counter, const std::string& error) {
    InputLine line;
    line.set_text(value);
    return input_line_bar(label, line, {}, {}, hints, width, counter, error);
}

std::string trash_indicator(const TuiState& state) {
    if (state.trash_view() || state.trash_count() == 0)
        return {};
    return trash_prefix(state) +
           core::tr("tui.trash_indicator", {{"count", std::to_string(state.trash_count())}});
}

std::optional<std::string> breadcrumb_folder_at(const TuiState& state, int x) {
    int cell = ftxui::string_width(breadcrumb_lead(state));
    for (const Crumb& crumb : breadcrumb(state)) {
        const int end = cell + ftxui::string_width(crumb.text);
        if (x >= cell && x < end)
            return crumb.folder;
        cell = end + static_cast<int>(kCrumbSeparator.size());
    }
    return std::nullopt;
}

const Preview& PreviewCache::get(const core::NoteStore& store, const Row* row) {
    const std::size_t before = revision_;
    const Preview& loaded = load(row, false, [&] { return store.read(row->path); });
    if (revision_ == before)
        return loaded;
    if (auto links = store.links(row->path))
        links_ = std::move(*links);
    else if (links.error().code != core::Error::Code::NotFound)
        core::log_warn("note links: " + core::format_error(links.error()));
    return loaded;
}

void PreviewCache::clear_links() {
    links_ = {};
    ++links_generation_;
}

const Preview& PreviewCache::get_trashed(const core::NoteStore& store, const Row* row) {
    return load(row, true, [&] { return store.read_trash(row->path); });
}

const Preview& PreviewCache::current(const TuiState& state, const core::NoteStore& store) {
    return state.trash_view() ? get_trashed(store, state.selected_row()) : get(store, state.selected_row());
}

const Preview& PreviewCache::throttled(TuiState& state, const core::NoteStore& store,
                                       std::chrono::steady_clock::time_point now) {
    deferred_until_.reset();
    const Row* row = state.selected_row();
    if (!row)
        requested_.reset();  // klasör satırı: sonraki not yine birleştirme kuralından geçer
    if (row && (!requested_ || *requested_ != row->path)) {
        const bool idle = now - last_move_ >= std::chrono::milliseconds(kCoalesceMs);
        requested_ = row->path;
        last_move_ = now;
        waiting_ = !idle;
    }
    if (row && waiting_ && now - last_move_ < std::chrono::milliseconds(kCoalesceMs)) {
        deferred_until_ = last_move_ + std::chrono::milliseconds(kCoalesceMs);
        return shown();
    }
    waiting_ = false;
    current(state, store);
    return publish(state, now);
}

void PreviewCache::build_rendered(int width) {
    rendered_revision_ = revision_;
    rendered_width_ = width;
    rendered_note_.clear();
    rendered_ok_ = false;
    rendered_chunks_.clear();
    rendered_definitions_ = {};
    csv_index_.reset();
    json_index_.reset();
    source_lines_.reset();
    csv_columns_ = {};
    rendered_chunk_count_ = 0;
    rendered_next_ = 0;
    const std::string_view source = preview_.text;
    if (source.size() > view_limits_.source_bytes) {
        rendered_note_ =
            core::tr("tui.view_note_too_large", {{"limit", human_size(view_limits_.source_bytes)}});
        return;
    }
    std::optional<RenderedView> view;
    switch (preview_.view_kind) {
    case ViewKind::Markdown: {
        // Tek parçalık not bütün olarak üretilir (çıktı aynı); büyüğü parça parça (extend_rendered).
        app::MarkdownScan scan = app::scan_markdown(source, app::kMarkdownChunkBytes, app::kMarkdownMaxChunkBytes,
                                                    view_limits_.markdown);
        rendered_chunks_ = std::move(scan.chunks);
        rendered_chunk_count_ = rendered_chunks_.size();
        rendered_next_ = 1;
        if (rendered_chunks_.size() > 1)
            rendered_definitions_ = std::move(scan.definitions);
        const std::string first =
            rendered_definitions_.empty()
                ? std::string{}
                : app::markdown_chunk_source(source, rendered_chunks_.front(), rendered_definitions_,
                                             view_limits_.markdown);
        view = render_markdown(first.empty() ? source.substr(0, rendered_chunks_.front().end) : std::string_view(first),
                               width, terminal_cell_width, view_limits_);
        break;
    }
    case ViewKind::Csv:
        // Dizin bir geçiştir (hücre kurulmaz); görünüm satır öbekleriyle parça parça.
        csv_index_ = app::index_csv(source);
        if (!csv_index_) {
            rendered_note_ = core::tr("tui.view_note_not_table");
            break;
        }
        source_lines_.emplace(source);
        rendered_chunk_count_ = std::max<std::size_t>(
            (std::min(csv_index_->row_count, view_limits_.table_rows) + kDataChunkLines - 1) / kDataChunkLines, 1);
        view = data_chunk(rendered_next_++, view_limits_);
        break;
    case ViewKind::Json:
        json_index_ = app::index_json(source);
        if (!json_index_) {
            rendered_note_ = core::tr("tui.view_note_invalid_json");
            break;
        }
        source_lines_.emplace(source);
        rendered_chunk_count_ =
            std::max<std::size_t>((json_index_->line_count + kDataChunkLines - 1) / kDataChunkLines, 1);
        view = data_chunk(rendered_next_++, view_limits_);
        break;
    case ViewKind::None:
        return;
    }
    if (view) {
        const bool truncated = view->lines.size() > view_limits_.lines;
        rendered_ = rendered_preview(std::move(*view), preview_.view_kind);
        rendered_ok_ = true;
        if (truncated)
            rendered_next_ = rendered_chunk_count_;
        rendered_.partial = rendered_next_ < rendered_chunk_count_;
    }
}

RenderedView PreviewCache::data_chunk(std::size_t chunk, const ViewLimits& limits) {
    const std::size_t first = chunk * kDataChunkLines;
    if (csv_index_)
        return render_csv_rows(preview_.text, *csv_index_, *source_lines_, first, kDataChunkLines,
                               chunk + 1 == rendered_chunk_count_, csv_columns_, rendered_width_, terminal_cell_width,
                               limits);
    return render_json_lines(preview_.text, *json_index_, *source_lines_, first, kDataChunkLines, rendered_width_,
                             terminal_cell_width, limits);
}

std::size_t PreviewCache::chunk_first_line(std::size_t chunk) const {
    if (preview_.view_kind == ViewKind::Markdown)
        return rendered_chunks_[chunk].first_line;
    const std::size_t first = chunk * kDataChunkLines;
    if (csv_index_)
        return source_lines_->line_of(app::csv_rows(preview_.text, *csv_index_, first, 1, 0).at(0).offset);
    const std::vector<app::JsonLine> line = app::json_lines(preview_.text, *json_index_, first, 1);
    return line.empty() ? source_lines_->count() - 1 : source_lines_->line_of(line.front().offset);
}

// Parçanın kendi üretimi tek geçişle aynıdır; aradaki boş satır (üst düzey bloklar arası) ve satır sınırı
// (limits.tui.view_lines) parçalar arasında sürer. Sınır aşan parçanın soluk notu onun üretiminden gelir.
void PreviewCache::append_chunk() {
    if (preview_.view_kind != ViewKind::Markdown) {
        ViewLimits limits = view_limits_;
        limits.lines = view_limits_.lines - std::min(view_limits_.lines, rendered_.lines.size());
        RenderedView view = data_chunk(rendered_next_++, limits);
        const bool truncated = view.lines.size() > limits.lines;
        append_rendered(rendered_, std::move(view), 0);
        if (truncated)
            rendered_next_ = rendered_chunk_count_;
        rendered_.partial = rendered_next_ < rendered_chunk_count_;
        return;
    }
    const app::MarkdownChunk& chunk = rendered_chunks_[rendered_next_++];
    // Başka parçadaki başvuru tanımı da çözülsün: tanımlar ardına eklenir (çıktı ve satır eşlemesi değişmez).
    const std::string source =
        app::markdown_chunk_source(preview_.text, chunk, rendered_definitions_, view_limits_.markdown);
    // Boş satır da sınıra sayılır; sınır dolduysa tek geçişteki gibi düşer.
    const bool gap = rendered_.lines.size() < view_limits_.lines;
    ViewLimits limits = view_limits_;
    limits.lines = view_limits_.lines - std::min(view_limits_.lines, rendered_.lines.size() + (gap ? 1 : 0));
    RenderedView view = render_markdown(source, rendered_width_, terminal_cell_width, limits);
    // Zorla kesilen dev bloğun önüne taşınan satırlar (kod çiti, tablo başlığı) notta parçanın ilk satırındadır.
    for (std::size_t& line : view.source_lines)
        line -= std::min(line, chunk.carry_lines);
    const bool truncated = view.lines.size() > limits.lines;
    if (gap && !view.source_lines.empty()) {
        rendered_.line_starts.push_back(rendered_.text.size());
        rendered_.text += '\n';
        rendered_.lines.emplace_back();
        rendered_.styled.emplace_back();
        rendered_.source_lines.push_back(chunk.first_line + view.source_lines.front());
    }
    append_rendered(rendered_, std::move(view), chunk.first_line);
    if (truncated)
        rendered_next_ = rendered_chunk_count_;
    rendered_.partial = rendered_next_ < rendered_chunk_count_;
}

void PreviewCache::extend_rendered(const TuiState& state) {
    if (!rendered_active_ || !rendered_.partial)
        return;
    const bool whole = state.wants_whole_preview();
    const std::optional<std::size_t> pending = state.pending_source_line();
    // Görünen pencerenin bir ekran ötesine dek: kaydırma çoğunlukla hazır satıra iner.
    const std::size_t wanted = state.preview_scroll() + (2 * static_cast<std::size_t>(std::max(state.window_height(), 1)));
    while (rendered_.partial &&
           (whole || rendered_.lines.size() <= wanted ||
            (pending && chunk_first_line(rendered_next_) <= *pending)))
        append_chunk();
}

bool PreviewCache::resize_settling(int width, std::chrono::steady_clock::time_point now) {
    const auto settle = std::chrono::milliseconds(kResizeSettleMs);
    if (settling_width_ != width) {
        resize_waiting_ = now - last_resize_ < settle;
        settling_width_ = width;
        last_resize_ = now;
    }
    return resize_waiting_ && now - last_resize_ < settle;
}

bool PreviewCache::update_rendered(bool wanted, int width, std::chrono::steady_clock::time_point now) {
    const bool same_revision = rendered_revision_ == revision_;
    const bool width_changed = same_revision && rendered_width_ != width;
    if (wanted && width_changed && resize_settling(width, now))
        return false;
    if (!width_changed)
        settling_width_.reset();
    if (wanted && (!same_revision || width_changed))
        build_rendered(width);
    const bool active = wanted && rendered_ok_;
    // Okuma üretimi zaten artırdı; görünüm aynı metin üzerinde değiştiyse de eşleşme ve seçim eskidir.
    if (active != rendered_active_ || (active && width_changed))
        ++generation_;
    rendered_active_ = active;
    preview_.note = wanted && !active ? rendered_note_ : std::string{};
    return true;
}

const Preview& PreviewCache::publish(TuiState& state, std::chrono::steady_clock::time_point now) {
    state.set_links(links_, links_generation_, path_);
    const int width = editor_viewport({state.window_width(), state.window_height()}, state.layout()).columns;
    const bool wanted = state.review_rendered() && preview_.kind == Preview::Kind::Text &&
                        preview_.view_kind != ViewKind::None;
    if (update_rendered(wanted, width, now)) {
        extend_rendered(state);
        state.set_preview_view(rendered_active_ ? &rendered_.source_lines : nullptr, generation_);
    }
    else
        deferred_until_ = last_resize_ + std::chrono::milliseconds(kResizeSettleMs);  // önceki yerleşim kalır
    return shown();
}

int row_marker_column(const TuiState& state, std::size_t row) {
    constexpr int kCellsBeforeNumber = kPaneContentX + 1;  // kenar, iç boşluk, seçim işareti
    constexpr int kCellsAfterNumber = 1;                   // boşluk
    const int inner_width =
        state.layout().geometry(state.window_width(), right_pane_focused(state.focus())).list_width -
        kPaneChromeCells;
    const int indent = row < state.rows().size() ? row_indent_cells(state.rows()[row], inner_width) : 0;
    return kCellsBeforeNumber + indent + digit_count(state.max_row_number()) + kCellsAfterNumber;
}

namespace {

void split_preview_lines(Preview& preview) {
    std::string_view rest = preview.text;
    while (true) {
        const std::size_t eol = rest.find('\n');
        std::string_view line = rest.substr(0, eol);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        preview.lines.emplace_back(line);
        preview.line_starts.push_back(static_cast<std::size_t>(rest.data() - preview.text.data()));
        if (eol == std::string_view::npos)
            break;
        rest.remove_prefix(eol + 1);
    }
}

}  // namespace

const Preview& PreviewCache::load(const Row* row, bool from_trash, const Reader& read) {
    if (!row) {
        if (!links_.empty())
            clear_links();
        path_.reset();
        preview_ = {};
        pdf_.reset();
        return preview_;
    }
    if (path_ && *path_ == row->path && from_trash_ == from_trash) {
        refresh_pdf();
        return preview_;
    }
    clear_links();
    path_ = row->path;
    from_trash_ = from_trash;
    preview_ = {};
    ++generation_;
    ++revision_;
    auto content = read();
    if (!content) {
        pdf_.reset();
        preview_.kind = Preview::Kind::Error;
        preview_.error = core::format_error(content.error());
        return preview_;
    }
    const std::u8string extension = row->path.value().extension().u8string();
    const std::string type =
        extension.empty() ? std::string{} : std::string(extension.begin() + 1, extension.end());
    if (pdf_options_ && app::is_pdf_type(type)) {
        start_pdf(std::move(content->text));
        return preview_;
    }
    pdf_.reset();
    if (core::looks_binary(content->text)) {
        preview_.kind = Preview::Kind::Binary;
        return preview_;
    }
    preview_.kind = Preview::Kind::Text;
    preview_.spell_checked = spellcheck_applies_to(type);
    preview_.view_kind = view_kind_for(type);
    preview_.highlight_line_bytes = syntax_limits_.line_bytes;
    if (content->text.size() <= syntax_limits_.note_bytes)
        preview_.language = type.empty() ? SyntaxLanguage::Plain : syntax_language_for(type);
    // Sarma geçerli UTF-8 ister; ikili sayılmayan ama bozuk bayt içeren notta gösterim için arındırılır.
    if (!core::is_valid_utf8(content->text) || content->text.find('\0') != std::string::npos) {
        content->text = sanitized_utf8(content->text);
        preview_.display_only = true;
    }
    preview_.text = std::move(content->text);
    // Baştaki BOM metin değil kodlama işaretidir: editör de atar. Kalsaydı ilk satırda gizli karakter
    // işareti olarak görünürdü.
    if (preview_.text.starts_with(kBom))
        preview_.text.erase(0, kBom.size());
    split_preview_lines(preview_);
    return preview_;
}

void PreviewCache::set_pdf(core::PdfLimits limits, std::filesystem::path worker, std::function<void()> on_change) {
    pdf_options_ = app::PdfPreviewOptions{std::move(limits), std::move(worker), std::move(on_change)};
}

// Aynı baytlar (yeniden tarama invalidate etti, not değişmedi) çalışan oturumu korur: işçi yeniden açmaz, metin
// baştan eklenir (alınmış sayfalar oturumda tutulmadığından yeniden okunur).
void PreviewCache::start_pdf(std::string bytes) {
    const std::size_t hash = std::hash<std::string_view>{}(bytes);
    if (!pdf_ || hash != pdf_bytes_hash_) {
        pdf_.reset();  // önce eskisi durur: iki işçi süreci aynı anda yaşamasın
        pdf_ = std::make_unique<app::PdfPreview>(std::move(bytes), *pdf_options_);
        pdf_bytes_hash_ = hash;
    }
    pdf_next_page_ = 0;
    pdf_truncated_ = false;
    pdf_loaded_bytes_ = 0;
    pdf_loaded_lines_ = 0;
    pdf_version_ = std::numeric_limits<std::uint64_t>::max();  // ilk refresh_pdf durumu okusun
    preview_.kind = Preview::Kind::Text;
    preview_.display_only = true;  // metin PDF'in kendisi değil
    append_preview_line(core::tr("app.pdf_loading"));
    refresh_pdf();
}

// Sayfalar sırayla, en çok kPdfReadAhead sayfa önden istenir ve alınınca oturumdan silinir (take_text): metnin tek
// kopyası buradadır. Metin limits.tui.view_bytes'a (source_bytes) ulaşınca okuma durur, bekleyen istekler düşer ve
// kesildiği söylenir: senkronla gelen PDF seçilince bellek sayfa x text_bytes'a büyümez (aşım en çok bir sayfa).
void PreviewCache::refresh_pdf() {
    if (!pdf_ || pdf_truncated_)
        return;
    const std::uint64_t version = pdf_->version();
    if (version == pdf_version_)
        return;
    pdf_version_ = version;
    const app::PdfPreview::State state = pdf_->state();
    if (state == app::PdfPreview::State::Opening)
        return;
    preview_.text.resize(pdf_loaded_bytes_);
    preview_.lines.resize(pdf_loaded_lines_);
    preview_.line_starts.resize(pdf_loaded_lines_);
    if (state == app::PdfPreview::State::Failed) {
        append_preview_line(core::format_error(pdf_->open_error().value()));
        return;
    }
    const int count = pdf_->page_count();
    while (pdf_next_page_ < count) {
        const auto text = pdf_->take_text(pdf_next_page_);
        if (!text)
            break;  // sayfalar sırayla eklenir
        append_preview_line(app::pdf_page_separator(pdf_next_page_, count));
        ++pdf_next_page_;
        if (!*text) {
            append_preview_line(core::format_error(text->error()));
            continue;
        }
        std::string_view rest = **text;
        if (rest.ends_with('\n'))
            rest.remove_suffix(1);
        while (!rest.empty()) {
            const std::size_t eol = rest.find('\n');
            append_preview_line(rest.substr(0, eol));
            rest.remove_prefix(eol == std::string_view::npos ? rest.size() : eol + 1);
        }
        if (preview_.text.size() >= view_limits_.source_bytes && pdf_next_page_ < count) {
            pdf_truncated_ = true;
            pdf_->drop_pending();  // yürüyen tek istek biter, sonucu alınmaz
            append_preview_line(core::tr("app.pdf_truncated", {{"limit", human_size(view_limits_.source_bytes)}}));
            return;
        }
    }
    pdf_loaded_bytes_ = preview_.text.size();
    pdf_loaded_lines_ = preview_.lines.size();
    if (pdf_next_page_ >= count)
        return;
    for (int page = pdf_next_page_; page < std::min(count, pdf_next_page_ + kPdfReadAhead); ++page)
        pdf_->request_text(page);
    append_preview_line(core::tr("app.pdf_page_loading", {{"page", std::to_string(pdf_next_page_ + 1)}}));
}

void PreviewCache::append_preview_line(std::string_view line) {
    if (!preview_.lines.empty())
        preview_.text += '\n';
    preview_.line_starts.push_back(preview_.text.size());
    preview_.text += line;
    preview_.lines.emplace_back(line);
}

std::string selected_text(const Preview& preview, const PreviewRange& range) {
    if (range.end_line >= preview.line_starts.size())
        return {};
    const std::size_t begin = preview.line_starts[range.begin_line] + range.begin_offset;
    const std::size_t end = preview.line_starts[range.end_line] + range.end_offset;
    // Uçlar çizildiği andaki metne aittir: bu arada küçülen not substr'ı out_of_range'e götürürdü.
    return end > begin && begin < preview.text.size() ? preview.text.substr(begin, end - begin)
                                                      : std::string{};
}

std::string tail_cells(const std::string& value, std::size_t limit) {
    const auto budget = static_cast<int>(limit);
    if (ftxui::string_width(value) <= budget)
        return value;
    if (limit <= 1)
        return "…";  // uzun hata satırı girdiye tek hücre bırakabilir
    int used = 1;    // "…"
    std::size_t begin = value.size();
    while (begin > 0) {
        std::size_t previous = begin - 1;
        while (previous > 0 && (static_cast<unsigned char>(value[previous]) & 0xC0) == 0x80)
            --previous;
        const int cells = ftxui::string_width(value.substr(previous, begin - previous));
        if (used + cells > budget)
            break;
        used += cells;
        begin = previous;
    }
    return "…" + value.substr(begin);
}

std::string head_ellipsis(const std::string& value, int limit) {
    if (ftxui::string_width(value) <= limit)
        return value;
    if (limit < 1)
        return {};
    return head_cells(value, limit - 1) + "…";
}

std::string fit_hints(std::string_view hints, int width) {
    std::string fitted;
    std::size_t start = 0;
    while (start < hints.size()) {
        const std::size_t end = hints.find(kHintSeparator, start);
        std::string candidate = fitted;
        if (!candidate.empty())
            candidate += kHintSeparator;
        candidate += hints.substr(start, end - start);
        if (ftxui::string_width(candidate) > width)
            break;
        fitted = std::move(candidate);
        if (end == std::string_view::npos)
            break;
        start = end + kHintSeparator.size();
    }
    return fitted;
}

int pinned_help_cells(std::string_view hints) {
    const std::string help = split_help_hint(hints).second;
    return help.empty() ? 0 : ftxui::string_width(help) + static_cast<int>(kHintSeparator.size());
}

ftxui::Element hint_bar(std::string_view hints, int width, ftxui::Color key_color) {
    using namespace ftxui;
    const auto [rest, help] = split_help_hint(hints);
    if (help.empty())
        return hbox(hint_cells(fit_hints(rest, width), key_color));
    // Yardım en dar pencerede de görünür: önce onun yeri ayrılır, ipuçları kalan yere sığdığınca.
    const int room = width - string_width(help) - static_cast<int>(kHintSeparator.size());
    Elements cells = hint_cells(fit_hints(rest, room), key_color);
    cells.push_back(filler());
    std::ranges::move(hint_cells(help, key_color), std::back_inserter(cells));
    return hbox(std::move(cells));
}

std::string tab_strip_text(std::span<const TabLabel> tabs, int width) {
    constexpr std::string_view kSeparator = " │ ";
    constexpr std::string_view kMore = "…";
    if (tabs.empty() || width <= 0)
        return {};
    std::vector<std::string> cells;
    cells.reserve(tabs.size());
    std::size_t active = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const TabLabel& tab = tabs[i];
        // Ad güvenilmez (senkron): denetim baytı ve bidi terminale gitmez.
        std::string title = terminal_safe(tab.title) + (tab.dirty ? std::string(kDirtyMark).substr(1) : "");
        cells.push_back(tab.active ? "[" + title + "]" : title);
        if (tab.active)
            active = i;
    }
    const auto separator = static_cast<int>(ftxui::string_width(std::string(kSeparator)));
    const auto more = static_cast<int>(ftxui::string_width(std::string(kMore)));
    const auto cells_of = [&](std::size_t first, std::size_t last) {
        int used = 2;  // baştaki ve sondaki boşluk
        for (std::size_t i = first; i <= last; ++i)
            used += ftxui::string_width(cells[i]) + (i > first ? separator : 0);
        return used + (first > 0 ? more + separator : 0) + (last + 1 < cells.size() ? more + separator : 0);
    };
    // Etkin sekmeden iki yana büyür: önce sağa, sonra sola.
    std::size_t first = active;
    std::size_t last = active;
    for (bool grew = true; grew;) {
        grew = false;
        if (last + 1 < cells.size() && cells_of(first, last + 1) <= width) {
            ++last;
            grew = true;
        }
        if (first > 0 && cells_of(first - 1, last) <= width) {
            --first;
            grew = true;
        }
    }
    std::string line = " ";
    if (first > 0)
        line += std::string(kMore) + std::string(kSeparator);
    for (std::size_t i = first; i <= last; ++i)
        line += (i > first ? std::string(kSeparator) : std::string{}) + cells[i];
    if (last + 1 < cells.size())
        line += std::string(kSeparator) + std::string(kMore);
    line += " ";
    // Tek sekme de sığmıyorsa sonu kısalır.
    return ftxui::string_width(line) <= width ? line : tail_cells(line, static_cast<std::size_t>(width));
}

EditorViewport editor_viewport(ftxui::Dimensions window, const PaneLayout& layout) {
    const PaneGeometry geometry = layout.geometry(window.dimx, true);
    return {std::max(geometry.right_width - kPaneChromeCells, 1),
            std::max(window.dimy - TuiState::kChromeLines, 1)};
}

// En yeni bildirim: tür öneki (renk tek sinyal değil), metin, yineleme sayısı, bekleyen diğerleri. Sığmazsa
// sonu kısalır. Bildirim yoksa nullptr.
app::UpdateView safe_update_view(app::UpdateView view) {
    for (std::string& line : view.lines)
        line = terminal_safe(line);
    view.link = terminal_safe(view.link);
    return view;
}

ftxui::Element notice_line(const app::NoticeQueue& queue, int width) {
    using namespace ftxui;
    const auto& items = queue.items();
    if (items.empty())
        return nullptr;
    const app::Notice& notice = items.back();
    const char* prefix_key = notice.kind == app::NoticeKind::Error     ? "notice.error"
                             : notice.kind == app::NoticeKind::Success ? "notice.success"
                                                                       : "notice.info";
    std::string line = " " + core::tr(prefix_key) + "  " + terminal_safe(notice.text);
    if (notice.repeats > 1)
        line += "  (×" + std::to_string(notice.repeats) + ")";
    if (notice.update_version)
        line += "  " + core::tr("tui.notice_update_hint");  // GUI'nin Kur düğmesi: palet `update`
    if (items.size() > 1)
        line += "  " + core::tr("notice.more", {{"count", std::to_string(items.size() - 1)}});
    line += " ";
    const Color tint = notice.kind == app::NoticeKind::Error     ? Color::Red
                       : notice.kind == app::NoticeKind::Success ? Color::Green
                                                                 : Color::Cyan;
    return text(fit_hints(line, std::max(width - 2, 1))) | inverted | color(tint) | clear_under;
}

ftxui::Element render(const TuiState& state, const Preview& preview, const StatusLine& status,
                      ftxui::Dimensions window, const SaveDialogState* dialog, const ConfirmState* confirm,
                      const HelpState* help, const SettingsState* settings, const NewRowState* new_row,
                      const EditorSession* editor, SpellChecker* spell, const RenameState* rename,
                      std::span<const TabLabel> tabs, const UpdateDialogState* update) {
    using namespace ftxui;
    if (window.dimx < kMinWidth || window.dimy < kMinHeight)
        return text(core::tr("tui.too_small")) | center | flex;

    // Yol girdisi hatası alt çubuğa bir satır ekler: panel kısalır ki seçili satır kırpılmasın.
    const bool input_error = rename && !rename->error().empty();
    const EditorSession* editing = active_editor(state, editor);
    const bool editor_open = editing && editing->is_open();
    const int panel_height = window.dimy - TuiState::kChromeLines - (input_error ? 1 : 0);
    Element modal = open_modal(state, help, confirm, dialog, settings, update, window.dimy);
    const bool modal_open = modal != nullptr;
    Elements lines = {
        editing && state.new_note_view() ? new_note_header(*editing) : top_bar(state, status, window.dimx),
        panes(state, preview, editing, new_row, window, panel_height, !modal_open, spell, tabs) | flex};
    // Biçimli görünüm olmadıysa neden burada: içerik satırlarını kaydırmaz, fare ve arama eşlemesi bozulmaz.
    const std::string note = preview.note.empty() ? std::string{} : preview.note + "  ";
    // Alt satırlar odaktaki işi anlatır: satır kip hapıyla açılır, ipucu tuşları aynı renktedir.
    const PaneMode mode = status_mode(state, editing);
    const int info_width = window.dimx - ftxui::string_width(pill_text(mode));
    // cppcheck-suppress nullPointerRedundantCheck ; editor_open, editing dolu demektir
    lines.push_back(
        hbox({mode_pill(mode), editor_open ? editor_info_bar(*editing, info_width) | flex
                                           : text(" " + note + totals_text(state, panel_height)) | dim}));
    // cppcheck-suppress nullPointerRedundantCheck ; editor_open, editing dolu demektir
    lines.push_back(editor_open ? editor_status_bar(*editing, window.dimx, mode.color)
                                : bottom_bar(state, status, new_row, rename, window.dimx, mode.color));
    Element main = vbox(std::move(lines));
    if (Element toast = notice_line(status.notices, window.dimx))
        main = dbox({main, vbox({filler(), hbox({filler(), toast}), text(""), text("")})});
    if (!modal)
        modal = inner_modal(state, editing, new_row, rename);
    if (!modal)
        return main;
    // FTXUI komşu kutu çizgilerini birleştirir (automerge): modal kenarı alttaki panel çizgisiyle ┬┴ olurdu,
    // clear_under bunu önlemez. Kenar hücreleri birleşmeden çıkarılır; modalın boyu ve yeri değişmez.
    return dbox({main, modal | edge_cells([](ftxui::Cell& cell) { cell.automerge = false; }) | center});
}

}  // namespace noteapp::tui
