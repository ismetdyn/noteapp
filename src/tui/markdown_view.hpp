#pragma once

#include "app/data_preview.hpp"
#include "tui/styled_view.hpp"

#include <optional>
#include <string_view>

namespace noteapp::tui {

// Notun REVIEW'daki biçimli görünümü: kural GUI önizlemesiyle ortak (app::preview_kind_for).
using ViewKind = app::PreviewKind;
inline ViewKind view_kind_for(std::string_view extension) {
    return app::preview_kind_for(extension);
}

// Saf: kaynak baytlar + genişlik -> biçimli satırlar; ağ ya da dosya yok. `source` geçerli UTF-8 olmalı
// (PreviewCache arındırır). HTML düz metin kalır, görsel yer tutucuya, bağlantı metin + soluk URL'ye iner;
// [[wiki]] bağlantısı GUI gibi ayraçsız metniyle, hedefi RenderedView::wiki_targets'ta.
// `limits`: satır, tablo ve kod bloğu satırı sınırları (source_bytes'a çağıran bakar).
RenderedView render_markdown(std::string_view source, int width, CellWidthFn cell_width,
                             const ViewLimits& limits = {});

// Geçerli JSON girintilenir; geçersizse nullopt (ham metne düşülür). İç içelik sınırı yok: yazıcı
// özyinelemesiz.
std::optional<RenderedView> render_json(std::string_view source, int width, CellWidthFn cell_width,
                                        const ViewLimits& limits = {});

// Ayırıcı `detect` kuralıyla bulunur; tutarlı tablo değilse nullopt. İlk satır başlıktır.
std::optional<RenderedView> render_csv(std::string_view source, int width, CellWidthFn cell_width,
                                       const ViewLimits& limits = {});

// Büyük csv/json parça parça (PreviewCache): dizinden yalnız istenen satırlar üretilir. `lines` kaynağın satır
// eşlemesi (bir kez kurulur). csv: [first, first + count) satırları (limits.table_rows altında; satır 0 başlık);
// `columns` boşsa bu parçadan hesaplanıp yazılır, doluysa onunla çizilir; `last` son parça (kesilen satır ve sütun
// notu orada). json: [first, first + count) girintili satırları.
RenderedView render_csv_rows(std::string_view source, const app::CsvIndex& index, const SourceLines& lines,
                             std::size_t first, std::size_t count, bool last, TableColumns& columns, int width,
                             CellWidthFn cell_width, const ViewLimits& limits);
RenderedView render_json_lines(std::string_view source, const app::JsonIndex& index, const SourceLines& lines,
                               std::size_t first, std::size_t count, int width, CellWidthFn cell_width,
                               const ViewLimits& limits);

}  // namespace noteapp::tui
