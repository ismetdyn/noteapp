#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::search {

// Eşleştirme için katlanmış metin. Üç dizi de kod noktası başına bir eleman içerir (1:1),
// bu yüzden katlanmış indeks -> özgün bayt ofseti eşlemesi doğrudan `byte_offsets`tir.
struct Folded {
    std::u32string original;  // katlanmamış kod noktaları (camelCase sınırı için büyük/küçük harf bilgisi)
    std::u32string text;      // katlanmış kod noktaları
    std::vector<std::size_t> byte_offsets;  // her kod noktasının özgün UTF-8'deki başlangıç baytı
};

// Basit küçük harf (ASCII, Latin-1, Latin Extended-A); I ve İ -> i. Türkçe harfleri ASCII'ye indirmez.
char32_t to_lower(char32_t cp);

// Büyük/küçük harf duyarsız; ı -> i, ç ğ ö ş ü -> c g o s u. Geçersiz UTF-8 baytı çökertmez:
// her geçersiz bayt tek bir U+FFFD olur (ofseti o baytın kendisi), ayrıştırma sonraki baytla sürer.
Folded fold(std::string_view utf8);

// Yalnız katlanmış metin gerekiyorsa (içerik araması için ortak).
std::string fold_to_utf8(std::string_view utf8);

}  // namespace noteapp::search
