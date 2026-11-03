#pragma once

#include "tui/editor_session.hpp"
#include "tui/spellcheck.hpp"

#include <cstddef>
#include <ftxui/dom/elements.hpp>
#include <optional>
#include <span>

namespace noteapp::tui {

// Kod noktasının ekrandaki hücre sayısı: tamponun ölçüsü FTXUI'nin çizimiyle aynı olsun diye ondan gelir.
int terminal_cell_width(char32_t code_point);

// Bir görsel satırın boyanması; önizleme ve editör aynı yoldan çizer, kontrol baytı ikisinde de ekrana
// gitmez.
struct RowPaint {
    std::optional<std::size_t> cursor;
    std::optional<ByteRange> selection;
    std::span<const ByteRange> highlights;
    std::span<const SyntaxSpan> syntax = {};  // satır içi ofsetler; `syntax_origin` satırın tampondaki başı
    std::size_t syntax_origin = 0;
    std::span<const ByteRange> misspelled = {};        // alt çizgi; `highlights` ile aynı ofsetler
    std::string_view ghost = {};                       // satır sonundaki imlecin ardında soluk öneri
    int columns = 0;                                   // gölgenin kırpılacağı genişlik
    std::span<const DiagnosticMark> diagnostics = {};  // dil sunucusu tanısı: önem rengiyle alt çizgi
};
ftxui::Element paint_row(const EditorBuffer& buffer, const VisualRow& row, const RowPaint& paint);
// Sözdizimi türünün renk ve biçimi; REVIEW'un biçimli görünümü de aynı paleti kullanır.
ftxui::Element token_styled(ftxui::Element element, TokenKind token);

// `show_cursor`: üstte modal yokken gerçek terminal imleci buraya konur; arama ve soru tuşları alırken
// gizlidir.
// `spell` yalnız md/txt notunda kullanılır; yoksa ya da kapalıysa alt çizgi yoktur.
ftxui::Element editor_pane(const EditorSession& session, bool show_cursor, SpellChecker* spell = nullptr);
// Micro gibi durum çubuğu: dosya, kirli işareti, tip, kodlama, satır sonu, BOM, satır:sütun, toplam satır.
ftxui::Element editor_info_bar(const EditorSession& session, int width);
// En alt satır: ileti ve sığan kısayollar; arama ve ad isteminde tek satırlık girdi.
ftxui::Element editor_status_bar(const EditorSession& session, int width);
// Açık soru penceresi (kaydedilmemiş değişiklik, çakışma); yoksa nullptr.
ftxui::Element editor_modal(const EditorSession& session);

}  // namespace noteapp::tui
