#pragma once

#include "core/note_store.hpp"
#include "tui/key.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::tui {

// Tek satırlık girdilerin ortak modeli: UTF-8 imleci, gölge öneri, Tab açılır listesi, geçmişte gezinme. Saf:
// G/Ç yapmaz, FTXUI'ye bağlı değildir; adaylar bellekteki listelerden gelir, tuş başına depo okunmaz.
enum class InputKind {
    Search,  // `/` `f`: sözcük sözcük tamamlanır, argüman adayları da gelir, geçmiş vardır
    Path,    // `a` `F2` ad istemi: metnin tamamı bir yoldur
    Find,    // editörde bul: metnin tamamı düz metindir, adaylar nottaki sözcüklerdir
    Folder,  // `m`: metnin tamamı bir klasördür, adaylar yalnız klasörler
};

// Değişmez anlık görüntü: açık girdi, yeniden tarama listeyi değiştirse de kendi kopyasını tutar. Hepsi
// sıralı.
struct InputSources {
    std::vector<std::string> folders;     // "work/"
    std::vector<std::string> names;       // "work/plan.md"
    std::vector<std::string> base_names;  // "plan.md", tekil
    std::vector<std::string> types;       // ".md", tekil
    std::vector<std::string> tags;        // "proje/alt" (tag_key), tekil
    std::vector<std::string> views = {};  // kayıtlı görünüm adları, ilk yazılış
};
using SharedInputSources = std::shared_ptr<const InputSources>;

SharedInputSources build_input_sources(std::span<const core::NoteMeta> notes,
                                       std::span<const std::string> views = {});
// Editörde bul: nottaki sözcükler `names` olur, öbür listeler boştur.
SharedInputSources word_input_sources(std::vector<std::string> words);

// Arama satırının boyalı kısımları, bayt aralığı ve sıralı; kalanı düz metindir.
struct InputSpan {
    enum class Kind { Key, Value, Invalid };
    std::size_t begin;
    std::size_t end;
    Kind kind;
};
std::vector<InputSpan> search_spans(std::string_view query);

// Geçmişe, en yeni başta, yinelenmeden, en çok kMaxSearchHistory ekler; değiştiyse true. Sır benzeri değer ya
// da not adı taşıyan sorgu yazılmaz: dosya yalnız kullanıcının kendi aramalarını taşır.
inline constexpr std::size_t kMaxSearchHistory = 200;
inline constexpr std::size_t kMaxSearchEntryBytes = 1024;
bool remember_search(std::vector<std::string>& history, std::string_view query, const InputSources* sources);
// `tui-state.toml`daki `search_history` değeri (TOML dizgi dizisi). Okurken bozuk, sır benzeri ve fazla kayıt
// düşer: dosya elle değişmiş olabilir.
std::string format_search_history(std::span<const std::string> history);
std::vector<std::string> parse_search_history(std::string_view value);
// `["a", "b"]` listesi: süzmesiz; kaçışlar format_search_history ile aynı.
std::vector<std::string> parse_quoted_list(std::string_view value);

class InputLine {
public:
    enum class Result {
        Ignored,   // tuş bu satırın değil: çağıran kendi eylemine baksın
        Consumed,  // işlendi, metin değişmedi
        Edited,    // metin değişti
    };
    struct Popup {
        std::string query;
        std::vector<std::string> items;  // süzülmüş, en çok kPopupRows
        std::size_t selected = 0;
    };
    static constexpr std::size_t kPopupRows = 8;

    explicit InputLine(InputKind kind = InputKind::Path) : kind_(kind) {}

    // `recent`: en yeni başta geçmiş; yalnız Search kullanır.
    Result handle(Key key, std::string_view typed = {}, std::span<const std::string> recent = {});
    // İmleç metnin sonundayken eklenecek kısım; yoksa boş.
    std::string ghost(std::span<const std::string> recent = {}) const;

    void set_sources(SharedInputSources sources) { sources_ = std::move(sources); }
    void set_text(std::string text);
    void set_text(std::string text, std::size_t cursor);
    void clear() { set_text({}); }

    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }  // bayt ofseti, kod noktası başında
    const Popup* popup() const { return popup_ ? &*popup_ : nullptr; }

private:
    struct HistoryWalk {
        std::string pattern;
        std::optional<std::size_t> index;
    };

    Result edit(Key key, std::string_view typed);
    Result handle_popup(Popup& popup, Key key, std::string_view typed);
    Result open_popup();
    std::vector<std::string> path_candidates(std::string& word, std::size_t& begin) const;
    Result step_history(std::span<const std::string> recent, bool older);
    Result accept_ghost(std::span<const std::string> recent);
    void refresh_popup(Popup& popup);
    bool insert_candidate(const std::string& candidate);
    void place(std::string text, std::size_t cursor);

    InputKind kind_;
    SharedInputSources sources_;
    std::string text_;
    std::size_t cursor_ = 0;
    std::optional<Popup> popup_;
    std::vector<std::string> popup_candidates_;
    std::size_t popup_begin_ = 0;  // [popup_begin_, cursor_) seçimle değişir
    std::optional<HistoryWalk> history_;
};

// Girdinin hata satırı yazımla temizlenir; Backspace ve Delete boş metinde de temizler.
bool clears_input_error(Key key, InputLine::Result result);

}  // namespace noteapp::tui
