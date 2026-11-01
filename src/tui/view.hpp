#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "tui/modals.hpp"
#include "tui/save_dialog.hpp"
#include "tui/settings.hpp"
#include "tui/spellcheck.hpp"
#include "tui/syntax.hpp"
#include "tui/tui_state.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

class EditorSession;

// SPEC-tui: bu boyutun altında yerleşim anlamsızlaşır, ortalanmış bir uyarı gösterilir.
inline constexpr int kMinWidth = 60;
inline constexpr int kMinHeight = 15;
// Kaydedilmemiş değişikliğin çerçeve etiketindeki ve alt çubuktaki işareti.
inline constexpr std::string_view kDirtyMark = " *";

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
};

// Son okunan notu tutar: her yeniden çizimde diske gidilmez. Not değişince (yeniden tarama, editör dönüşü)
// invalidate() çağrılır.
class PreviewCache {
public:
    const Preview& get(const core::NoteStore& store, const Row* row);
    // Çöpteki notun içeriği; önbellekte canlı ağaçtaki aynı adlı notla karışmaz.
    const Preview& get_trashed(const core::NoteStore& store, const Row* row);
    // Görünüme göre: çöp görünümünde çöpteki, değilse canlı seçili not.
    const Preview& current(const TuiState& state, const core::NoteStore& store);
    // Hızlı gezinmede depo okunmaz: seçim değişimleri bu aralıktan sıksa yalnız son seçim, bu süre sonra
    // okunur; sakin kullanıcının tek hareketi bekletilmez. Beklerken önceki içerik döner, `deferred()` true
    // olur ve çağıran yeniden çizim ister.
    static constexpr int kCoalesceMs = 60;
    const Preview& throttled(const TuiState& state, const core::NoteStore& store,
                             std::chrono::steady_clock::time_point now);
    bool deferred() const { return deferred_; }
    void invalidate() { path_.reset(); }
    // Her yeniden okumada artar: not içi arama eşleşmeleri eski metne ait kalmasın.
    std::size_t generation() const { return generation_; }

private:
    using Reader = std::function<std::expected<core::NoteContent, core::Error>()>;
    const Preview& load(const Row* row, bool from_trash, const Reader& read);

    std::optional<core::NotePath> path_;
    bool from_trash_ = false;
    std::optional<core::NotePath> requested_;
    std::chrono::steady_clock::time_point last_move_{};
    bool waiting_ = false;
    bool deferred_ = false;
    Preview preview_;
    std::size_t generation_ = 0;
};

struct StatusLine {
    std::string sync;     // üst çubuk: senkron durumu metni
    std::string message;  // geçici ileti; doluysa kısayol çubuğunun yerine geçer
};

// Tüm ekran: üst çubuk, sol liste, sağ önizleme, toplamlar, alt çubuk. `window` terminal boyutu (testte ekran
// boyutu). Modal doluysa ana yerleşimin üstüne ortalanmış çizilir; birden çoğu doluysa öncelik yardım > onay
// > diyalog. `new_row` doluysa listenin ilk satırıdır; alt çubukta yalnız tuşları kalır.
ftxui::Element render(const TuiState& state, const Preview& preview, const StatusLine& status,
                      ftxui::Dimensions window, const SaveDialogState* dialog = nullptr,
                      const ConfirmState* confirm = nullptr, const HelpState* help = nullptr,
                      const SettingsState* settings = nullptr, const NewRowState* new_row = nullptr,
                      const EditorSession* editor = nullptr, SpellChecker* spell = nullptr,
                      const RenameState* rename = nullptr);

// Satırın `▸` `▾` işaretinin ekran sütunu: kenar, seçim, numara, boşluk.
int row_marker_column(const TuiState& state);

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

// Değerin son `limit` kod noktası; kesilen başa "…" konur (imleç sonda, görünür kalsın).
std::string tail_code_points(const std::string& value, std::size_t limit);

}  // namespace noteapp::tui
