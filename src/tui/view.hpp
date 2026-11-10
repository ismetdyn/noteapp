#pragma once

#include "app/notices.hpp"
#include "app/pdf_preview.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "edit/spellcheck.hpp"
#include "edit/syntax.hpp"
#include "tui/editor_tabs.hpp"
#include "tui/markdown_view.hpp"
#include "tui/modals.hpp"
#include "tui/save_dialog.hpp"
#include "tui/settings.hpp"
#include "tui/sixel.hpp"
#include "tui/sync_indicator.hpp"
#include "tui/tui_state.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

class EditorSession;

// SPEC-tui: bu boyutun altında yerleşim anlamsızlaşır, ortalanmış bir uyarı gösterilir.
inline constexpr int kMinWidth = 60;
inline constexpr int kMinHeight = 15;
// Kaydedilmemiş değişikliğin çerçeve etiketindeki ve alt çubuktaki işareti.
inline constexpr std::string_view kDirtyMark = " *";
// Alt satırda ipuçları ve notlar arasındaki boşluk.
inline constexpr std::string_view kHintSeparator = "  ";

// Seçili notun önizlemesi; dosya okuması render'dan ayrı tutulur (render saf kalır).
struct Preview {
    enum class Kind { Empty, Text, Binary, Error };
    Kind kind = Kind::Empty;
    std::vector<std::string> lines;        // Kind::Text; satır sonu baytları hariç
    std::string text;                      // gösterilen baytlar; seçim buradan kopyalanır (CRLF korunur)
    std::vector<std::size_t> line_starts;  // `lines[i]`in `text`teki başı
    bool display_only = false;             // geçersiz bayt arındırıldı: `text` özgün değil
    bool spell_checked = false;            // md/txt: yazım denetimi altı çizer
    std::string error;                     // Kind::Error: çevrilmiş hata metni
    SyntaxLanguage language = SyntaxLanguage::Plain;
    mutable SyntaxStateCache syntax;  // yalnız görünen satıra dek tembel dolar; metin değişmez
    std::size_t highlight_line_bytes = kDefaultHighlightLineBytes;  // limits.tui.highlight_line_bytes
    ViewKind view_kind = ViewKind::None;  // md/csv/json: `R` ham ve biçimli görünüm arasında geçer
    // Biçimli görünümde `lines`/`text` görüntülenen metindir (seçim onu kopyalar, arama onda çalışır);
    // satırlar zaten panele sığar. `styled` ve `source_lines` `lines` ile paraleldir.
    bool rendered = false;
    bool partial = false;  // büyük md parça parça üretiliyor: devamı var (TuiState::set_preview_lines)
    std::vector<std::vector<StyledSpan>> styled;
    std::vector<std::size_t> source_lines;
    std::vector<std::string> wiki_targets;  // biçimli markdown: RenderedView::wiki_targets, parçalar ardışık
    std::string note;  // biçimli görünüm istendi, olmadı: toplamlar satırında soluk neden

    // PDF notu ve görünümü (`R`, TuiState::pdf_page_view). Sayfa görünümünde `lines` ayırıcı ve durum satırlarıdır,
    // görüntü satırında boştur: seçim, kopya ve arama yoktur.
    enum class Pdf { No, Text, Pages };
    Pdf pdf = Pdf::No;
    // Sayfa görünümü, `lines` ile paralel: görüntü satırında sayfa (0 tabanlı) ve sayfanın hücre satırı; ayırıcı,
    // boş ve durum satırında page < 0. Çizici (blok; TPI-B'de sixel) sayfanın piksellerini bu satırlardan keser.
    struct PageRow {
        int page = -1;
        int row = 0;
    };
    std::vector<PageRow> page_rows;
    // Sayfa başına (dizin sayfa): pikseller yalnız görünen ve sonraki sayfada dolu; boşsa sayfanın ilk satırında
    // `message` (yükleniyor ya da metin görünümüyle aynı hata, display_safe).
    struct PageImage {
        std::shared_ptr<const pdfview::RenderedPage> pixels;
        std::string message;
    };
    std::vector<PageImage> page_images;
    // Sayfa sixel çiziliyorsa terminal hücresinin piksel boyu (PreviewCache::set_sixel_cell), yoksa `▀` blok.
    std::optional<CellPixels> sixel;
    // Sixel: çizim (ftxui::Render) görünen görüntü satırlarının ekrandaki yerini buraya yazar, kare yazıcısı
    // (FrameDiff) okur. Her çizimden önce boşaltılır (main); kare çiziminin çıktısıdır, önizleme verisi değil.
    mutable std::vector<SixelImage> sixels;
};

// Son okunan notu tutar: her yeniden çizimde diske gidilmez. Not değişince (yeniden tarama, editör dönüşü)
// invalidate() çağrılır.
class PreviewCache {
public:
    // `view`: biçimli görünümün sınırları; `syntax`: ham önizlemenin renklendirmesi (limits.tui.*).
    explicit PreviewCache(ViewLimits view = {}, SyntaxLimits syntax = {})
        : view_limits_(view), syntax_limits_(syntax) {}

    const Preview& get(const core::NoteStore& store, const Row* row);
    // Çöpteki notun içeriği; önbellekte canlı ağaçtaki aynı adlı notla karışmaz.
    const Preview& get_trashed(const core::NoteStore& store, const Row* row);
    // Görünüme göre: çöp görünümünde çöpteki, değilse canlı seçili not.
    const Preview& current(const TuiState& state, const core::NoteStore& store);
    // Hızlı gezinmede depo okunmaz: seçim değişimleri bu aralıktan sıksa yalnız son seçim, bu süre sonra
    // okunur; sakin kullanıcının tek hareketi bekletilmez. Beklerken önceki içerik döner, `deferred()` true
    // olur ve çağıran yeniden çizim ister.
    static constexpr int kCoalesceMs = 60;
    // Sürükleyerek boyutlandırmada biçimli görünüm her genişlikte yeniden sarılmaz: art arda değişimlerde
    // önceki yerleşim kalır, değişim bu süre durunca üretilir. Sakin kullanıcının tek değişimi beklemez.
    static constexpr int kResizeSettleMs = 120;
    // Büyük csv/json biçimli görünümünün parçası: bu kadar tablo satırı ya da girintili JSON satırı. Kapasite
    // değil iş birimi (ilk kare ve kaydırma karesi bütçesi).
    static constexpr std::size_t kDataChunkLines = 2000;
    // Dönen önizleme kullanıcının seçtiği görünümdür (`R`); biçimli görünüm not sürümü ve genişlik başına bir
    // kez üretilir, görüntü satırı -> kaynak satırı eşlemesi modele buradan bildirilir.
    const Preview& throttled(TuiState& state, const core::NoteStore& store,
                             std::chrono::steady_clock::time_point now);
    // Şu an gösterilen: PDF sayfa görünümü, biçimli görünüm ya da ham metin. Seçim kopyası bunu okur.
    const Preview& shown() const {
        if (pages_active_)
            return pages_;
        return rendered_active_ ? rendered_ : preview_;
    }
    bool deferred() const { return deferred_until_.has_value(); }
    // Ertelemenin bittiği an: çağıran o ana dek çizim istemez, o an bir kez yeniden çizer.
    std::optional<std::chrono::steady_clock::time_point> deferred_until() const { return deferred_until_; }
    void invalidate() { path_.reset(); }
    // Her yeniden okumada artar: not içi arama eşleşmeleri eski metne ait kalmasın.
    std::size_t generation() const { return generation_; }
    // Seçili notun bağlantıları: gövdeyle aynı ertelenmiş yüklemede okunur, gezinmede depoya gidilmez.
    const core::NoteLinks& links() const { return links_; }

private:
    using Reader = std::function<std::expected<core::NoteContent, core::Error>()>;
    const Preview& load(const Row* row, bool from_trash, const Reader& read);
    void clear_links();
    const Preview& publish(TuiState& state, std::chrono::steady_clock::time_point now);
    bool resize_settling(int width, std::chrono::steady_clock::time_point now);
    // Görünümü gerekirse üretir; false: boyutlandırma sürüyor, önceki yerleşim kalır.
    bool update_rendered(bool wanted, int width, std::chrono::steady_clock::time_point now);
    void build_rendered(int width);
    // Büyük md (app::markdown_chunks), csv ve json (dizinden satır öbekleri) parça parça: görünen pencere, bekleyen
    // kaynak satırı ya da (arama) tümü üretilene dek sıradaki parçalar eklenir.
    void extend_rendered(const TuiState& state);
    void append_chunk();
    RenderedView data_chunk(std::size_t chunk, const ViewLimits& limits);
    std::size_t chunk_first_line(std::size_t chunk) const;  // parçanın notta başladığı satır

    ViewLimits view_limits_;
    SyntaxLimits syntax_limits_;
    std::optional<core::NotePath> path_;
    bool from_trash_ = false;
    std::optional<core::NotePath> requested_;
    std::chrono::steady_clock::time_point last_move_{};
    bool waiting_ = false;
    std::optional<std::chrono::steady_clock::time_point> deferred_until_;
    Preview preview_;
    std::size_t generation_ = 0;
    core::NoteLinks links_;
    std::size_t links_generation_ = 0;
    // Biçimli görünüm: `revision_` (her okumada artar) ve genişlik aynıysa yeniden üretilmez.
    Preview rendered_;
    std::size_t revision_ = 0;
    std::optional<std::size_t> rendered_revision_;
    int rendered_width_ = 0;
    bool rendered_ok_ = false;
    bool rendered_active_ = false;
    std::vector<app::MarkdownChunk> rendered_chunks_;
    app::MarkdownDefinitions rendered_definitions_;  // birden çok parçada notun başvuru tanımları
    // csv/json: dizin, kaynak satır eşlemesi ve (csv) ilk parçadan sütun genişlikleri; parça satır öbeğidir.
    std::optional<app::CsvIndex> csv_index_;
    std::optional<app::JsonIndex> json_index_;
    std::optional<SourceLines> source_lines_;
    TableColumns csv_columns_;
    std::size_t rendered_chunk_count_ = 0;
    std::size_t rendered_next_ = 0;  // sıradaki üretilecek parça
    std::string rendered_note_;
    std::optional<int> settling_width_;
    std::chrono::steady_clock::time_point last_resize_{};
    bool resize_waiting_ = false;

public:
    // PDF notu (tip pdf) biçimli metin olarak gösterilir: sayfalar arka planda (app::PdfPreview) sırayla okunur,
    // yapılı metinden bloklara (app::pdf_page_blocks) çevrilir ve markdown görünümüyle aynı görünüşte, panel
    // genişliğine sarılarak (render_pdf_page) ayırıcı satırının (app::pdf_page_separator) altına eklenir; okunmayan
    // kısım tek "yükleniyor" satırıdır. Gösterilen metin seçilip kopyalanır. Genişlik değişince eklenmiş sayfalar
    // yeniden sarılır. Kurulmazsa PDF ikili not gibi görünür. `on_change` işçi iş parçacığından gelir: yalnız
    // yeniden çizim ister.
    void set_pdf(core::PdfLimits limits, std::filesystem::path worker, std::function<void()> on_change);
    // Sayfa görünümünün çizimi (main her çizimde tui::sixel_cell ile çözer): hücre boyu sixel, nullopt `▀` blok.
    // Değişince yerleşim yeniden kurulur, sayfalar yeni ölçekle istenir.
    void set_sixel_cell(std::optional<CellPixels> cell) { sixel_cell_ = cell; }
    // Oturumu kapatır (işçiyi durdurur, bekler): on_change'in hedefi yıkılmadan önce çağrılır.
    void close_pdf() { pdf_.reset(); }
    const app::PdfPreview* pdf() const { return pdf_.get(); }  // test

private:
    // Önden istenen sayfa: işçi boş beklemez, kesilince en çok bu kadar sayfa boşa okunur. İş birimi, sınır değil.
    static constexpr int kPdfReadAhead = 4;
    // Görünüm sarılmadan önce panelin genişliği bilinmiyorsa (publish'ten önce) bu genişlik kullanılır.
    static constexpr int kPdfDefaultWidth = 80;
    void start_pdf(std::string bytes);  // load'un PDF yolu: "yükleniyor" satırı, oturum
    // Gelen sayfaları sona ekler (metin yalnız büyür, kaydırma yerinde kalır); load ve get her çağrıda.
    void refresh_pdf();
    // Eklenmiş sayfaları ve durum satırını `width`'e yeniden sarar (publish, genişlik değişince).
    void layout_pdf(int width);
    void append_pdf_view(RenderedView view);
    // Sayfa görünümü (publish, TuiState::pdf_page_view): yerleşim durum, sayfa sayısı, genişlik ya da sayfa boyu
    // değişince yeniden kurulur; görünen sayfalar ve bir sonraki `columns` piksel genişliğe çizilmek üzere istenir.
    void update_pages(const TuiState& state, int columns, int rows);
    void layout_pages(int columns);
    std::pair<float, float> pdf_page_points() const;  // ilk okunan sayfanın boyu (punto), yoksa A4
    void cut_pdf_status();  // durum satırını atar: sayfa metni pdf_loaded_* uzunluğuna döner
    void show_pdf_status();  // pdf_status_'u (boş değilse) sona ekler

    std::optional<app::PdfPreviewOptions> pdf_options_;
    std::unique_ptr<app::PdfPreview> pdf_;
    std::size_t pdf_bytes_hash_ = 0;   // aynı not aynı baytlarla yeniden okununca oturum korunur
    std::uint64_t pdf_version_ = 0;    // son eklemede görülen PdfPreview::version
    bool pdf_truncated_ = false;       // metin view_bytes'a ulaştı: okuma durdu
    int pdf_next_page_ = 0;            // eklenmemiş ilk sayfa
    std::size_t pdf_loaded_bytes_ = 0;  // eklenen sayfaların metni; ardındaki durum satırı her eklemede yenilenir
    std::size_t pdf_loaded_lines_ = 0;
    // Eklenmiş sayfalar (bloklar ya da hata): genişlik değişince yeniden sarılır; metnin tek kopyası burada ve
    // preview_'dadır (view_bytes sınırı preview_.text'e bakar).
    struct PdfPageView {
        int page = 0;
        std::vector<pdf::Block> blocks;
        std::string error;
        float width = 0.0F;  // punto; sayfa görünümünün ölçeği ve satır sayısı (hatalı sayfada 0)
        float height = 0.0F;
    };
    std::vector<PdfPageView> pdf_pages_;
    std::string pdf_status_;  // son satır: yükleniyor, kesildi ya da açılış hatası; boş = yok
    int pdf_width_ = kPdfDefaultWidth;

    // Sayfa görünümü: yerleşimin girdileri ve son istenen sayfa aralığı (değişince bekleyen istekler düşer).
    struct PagesLayout {
        app::PdfPreview::State state = app::PdfPreview::State::Opening;
        int count = 0;
        int columns = 0;
        std::pair<float, float> points;
        std::optional<CellPixels> cell;
        bool operator==(const PagesLayout&) const = default;
    };
    struct PagesWanted {
        int first = 0;
        int last = 0;
        float scale = 0.0F;
        bool operator==(const PagesWanted&) const = default;
    };
    Preview pages_;
    bool pages_active_ = false;
    std::optional<PagesLayout> pages_layout_;
    std::optional<PagesWanted> pages_wanted_;
    std::optional<CellPixels> sixel_cell_;
};

struct StatusLine {
    std::string sync;         // üst çubuk: senkron durumu metni
    std::string message;      // geçici ileti; doluysa kısayol çubuğunun yerine geçer
    std::string notice = {};  // ilk tuşa dek sönük ipucu; hata değildir, message ondan önceliklidir
    SyncIndicator::State sync_state = SyncIndicator::State::Disabled;  // senkron metninin rengi
    // İşlem bildirimleri (toast; GUI ile ortak model, SPEC-tui "Bildirimler"): alt çubuğun üstünde sağda tek
    // satır, en yenisi. Süre `tuning.notice_ms` (main kurar); hata sonraki tuşa dek kalır (main kapatır).
    app::NoticeQueue notices{std::chrono::milliseconds(0)};

    // İşlem sonucu: ileti satırı ve bildirim aynı metin; bildirim görünürken alt çubuk metni yinelemez.
    void notify(app::NoticeKind kind, std::string text) {
        message = text;
        notices.post(kind, std::move(text), std::chrono::steady_clock::now());
    }
};

// Tüm ekran: üst çubuk, sol liste, sağ önizleme, toplamlar, alt çubuk. `window` terminal boyutu (testte ekran
// boyutu). Modal doluysa ana yerleşimin üstüne ortalanmış çizilir; birden çoğu doluysa öncelik yardım > onay
// > diyalog > ayarlar > güncelleme.
// `new_row` doluysa listenin ilk satırıdır; alt çubukta yalnız tuşları kalır.
ftxui::Element render(const TuiState& state, const Preview& preview, const StatusLine& status,
                      ftxui::Dimensions window, const SaveDialogState* dialog = nullptr,
                      const ConfirmState* confirm = nullptr, const HelpState* help = nullptr,
                      const SettingsState* settings = nullptr, const NewRowState* new_row = nullptr,
                      const EditorSession* editor = nullptr, SpellChecker* spell = nullptr,
                      const RenameState* rename = nullptr, std::span<const TabLabel> tabs = {},
                      const UpdateDialogState* update = nullptr);
// Dosya ikonunun terminal rengi (liste ve harita aynı rengi kullanır).
ftxui::Color icon_color(core::IconColor color);
// Güncelleme penceresinin metni terminal_safe'ten geçmiş hâliyle: sürüm, hata ayrıntısı ve adres sunucudan
// gelir. Pencere bunu çizer; testler FTXUI'ye girmeden önceki metni denetler.
app::UpdateView safe_update_view(app::UpdateView view);
// Harita (`view.map`, SPEC-tui "Harita"): kapsam satırı ve braille tuvali, `columns` x `rows` hücre.
ftxui::Element map_pane(const TuiState& state, int columns, int rows);
// Sekme satırı (sağ panelin üst kenarı): ` a.md │ [b.md*] │ c.md `. Etkin sekme köşeli parantezde; sığmayan
// sekmeler etkin sekme görünür kalacak şekilde düşer, yerlerinde `…`. En çok `width` hücre.
std::string tab_strip_text(std::span<const TabLabel> tabs, int width);

// Satırın ok hücresinin ekran sütunu (klasörde `▸` `▾`): kenar, seçim işareti, girinti, numara, boşluk.
int row_marker_column(const TuiState& state, std::size_t row = 0);

// Üst satırdaki klasör yolunun `x` sütunundaki parçasının klasörü; ayırıcıda ve dışında nullopt.
std::optional<std::string> breadcrumb_folder_at(const TuiState& state, int x);

// Üst satırın en solundaki çöp göstergesi ("çöp: 3"); çöp boşken ya da çöp görünümünde boş. Metin esastır:
// simge karakterinin genişliği terminale göre değişir ve satırı kaydırırdı.
std::string trash_indicator(const TuiState& state);

// Seçili aralığın özgün metni: sarma yok, satır sonları dosyadaki gibi.
std::string selected_text(const Preview& preview, const PreviewRange& range);

// Sağ panelin metne kalan alanı; editör imleci buna göre kaydırılır.
struct EditorViewport {
    int columns = 1;
    int rows = 1;
};
EditorViewport editor_viewport(ftxui::Dimensions window, const PaneLayout& layout);

// Alt satırdaki tek satırlık girdi (arama, not içi arama, ad istemi, hızlı ekleme): metnin sonu her zaman
// görünür, `counter` ("3/12") ve `error` ipuçlarından önce gelir; ipuçları kelime ortasında kesilmez.
ftxui::Element input_bar(const std::string& label, const std::string& value, const std::string& hints,
                         int width, const std::string& counter = {}, const std::string& error = {});

// Çift boşlukla ayrılmış ipuçlarından baştan `width` sütuna sığanları döner; katalogdaki sıra önem sırasıdır,
// sığmayan ipucu ve ardındakiler bütün olarak düşer (kelime ortasında kesilmez).
std::string fit_hints(std::string_view hints, int width);
// Kısayol çubuğu (SPEC-tui "Yerleşim"): tuş kalın ve `key_color` renginde, açıklama varsayılan renkte. `F1`
// yardım ipucu sağ kenara sabitlenir, kalanlar sırayla `fit_hints` kuralıyla soldan dolar.
ftxui::Element hint_bar(std::string_view hints, int width, ftxui::Color key_color);
// `hint_bar`ın sağa sabitlediği `F1` ipucunun ayracıyla kapladığı hücre; katalogda yoksa 0.
int pinned_help_cells(std::string_view hints);

// Değerin son `limit` kod noktası; kesilen başa "…" konur (imleç sonda, görünür kalsın).
std::string tail_cells(const std::string& value, std::size_t limit);
// Değerin baştan en çok `limit` hücresi; kesilirse sona "…" konur (iletinin başı okunur). Hiç yer yoksa boş.
std::string head_ellipsis(const std::string& value, int limit);

}  // namespace noteapp::tui
