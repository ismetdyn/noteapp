#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Motor (pdf_engine.cpp) ve testleri içindir; dışa açık API'ye girmez.
namespace noteapp::pdfview::detail {

// `text`in ilk UTF-8 dizisinin kod noktası; geçersizse (fazla uzun, vekil, > U+10FFFF, F5+ ya da C0/C1 öncü, kesik
// dizi) nullopt. `length` tüketilen bayttır: geçersizde 1 (ardından gelen geçerli metin kaybolmasın). Ön koşul:
// `text` boş değil.
std::optional<char32_t> decode_one(std::string_view text, std::size_t& length) noexcept;

// Sayfa metni belgenin ToUnicode'undan gelir ve düşmanca olabilir: MuPDF tek vekili (U+D800) geçersiz UTF-8 olarak,
// NUL/ESC/DEL/C1'i olduğu gibi yazar. Sonuç geçerli UTF-8'dir; '\n' ve '\t' dışındaki denetim karakterleri (C0, DEL,
// C1) ve geçersiz baytlar U+FFFD olur (kanıt kalır, terminale kaçış dizisi gitmez).
std::string clean_text(std::string_view raw);

}  // namespace noteapp::pdfview::detail
