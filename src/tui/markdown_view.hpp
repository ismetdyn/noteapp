#pragma once

#include "tui/styled_view.hpp"

#include <optional>
#include <string_view>

namespace noteapp::tui {

// Notun REVIEW'daki biçimli görünümü; uzantıdan belirlenir, None ise yalnız ham metin vardır.
enum class ViewKind { None, Markdown, Csv, Json };
ViewKind view_kind_for(std::string_view extension);

// Saf: kaynak baytlar + genişlik -> biçimli satırlar; ağ ya da dosya yok. `source` geçerli UTF-8 olmalı
// (PreviewCache arındırır). HTML düz metin kalır, görsel yer tutucuya, bağlantı metin + soluk URL'ye iner.
RenderedView render_markdown(std::string_view source, int width, CellWidthFn cell_width);

// Geçerli JSON girintilenir; geçersiz ya da kMaxJsonDepth'ten derinse nullopt (ham metne düşülür).
inline constexpr int kMaxJsonDepth = 128;
std::optional<RenderedView> render_json(std::string_view source, int width, CellWidthFn cell_width);

// Ayırıcı `detect` kuralıyla bulunur; tutarlı tablo değilse nullopt. İlk satır başlıktır.
std::optional<RenderedView> render_csv(std::string_view source, int width, CellWidthFn cell_width);

}  // namespace noteapp::tui
