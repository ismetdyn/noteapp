#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::core {

// Varsayılan; çalışma anındaki değer Tuning::slug_max_length (defaults.toml) ile gelir.
inline constexpr std::size_t kDefaultSlugMaxLength = 60;

// Geçersiz UTF-8'de (fazla uzun kodlama, vekil çift, > U+10FFFF, kesik dizi) nullopt döner.
std::optional<std::u32string> decode_utf8(std::string_view utf8);
std::string encode_utf8(std::u32string_view codepoints);
bool is_valid_utf8(std::string_view utf8);

// ç ğ ı İ ö ş ü ve büyük harfleri ASCII karşılığına çevirir; diğer karakterler aynen kalır.
// Geçersiz UTF-8 girdisi değiştirilmeden döner.
std::string transliterate_turkish(std::string_view utf8);

// CRLF → LF; tek başına CR olduğu gibi kalır. Tek geçiş (doğrusal): yeni notlar (NoteStore::create) ve GUI
// editörüne yüklenen CRLF'li içerik aynı kuraldan geçer.
std::string normalize_line_endings(std::string_view text);

// İlk dolu (yalnız boşluk olmayan) satır, olduğu gibi; yoksa boş. Not adı bundan türetilir — CLI, TUI ve GUI
// aynı kuralı kullansın diye tek kaynak burasıdır.
std::string first_non_empty_line(std::string_view content);

// İçerikten not adı türetilirken slugify'a verilecek metin; değer asla dosya adına ve senkrona girmesin diye
// CLI, TUI ve kayıt yolu buradan geçer.
//  - env/properties/ini/toml/yaml: yorum, bölüm ve liste satırları atlanır, ilk satırın yalnız anahtarı;
//  anahtar
//    yoksa boş döner (slugify zaman damgalı ad üretir).
//  - Diğer tipler: ilk dolu satır `[export ]IDENT=değer` ya da `IDENT: tek-sözcük` ise yalnız IDENT (IDENT
//  ASCII,
//    boşluksuz; değer boş değil). `Toplantı: bütçe`, `Not: süt al` ve `https://x` düzyazıdır, satır aynen
//    döner.
std::string note_name_source(std::string_view content, std::string_view type);

// env/properties/ini/toml/yaml: değerleri sır olabilir; adlara ve öneriye yalnız anahtarları girer.
bool type_has_secret_values(std::string_view type);
// Böyle bir tipteki satırın anahtarı; yorum, bölüm, liste ya da anahtarsız satırda boş. `line`ın içine bakar.
std::string_view secret_line_key(std::string_view line);
// Tipten bağımsız `[export ]IDENT=değer` ya da `IDENT: değer` satırının anahtarı; böyle bir satır değilse
// boş. `line`ın içine bakar.
std::string_view assignment_line_key(std::string_view line);
// Metinde belirteç gibi görünen sözcük var mı: bilinen önek (ghp_, sk-, AKIA...) ya da uzun base64/hex.
bool has_token_like_word(std::string_view text);

// Kullanıcının yazdığı ad: son noktadan sonrası uzantıdır; baştaki ve sondaki nokta ya da boşluk taşıyan son
// parça uzantı değildir (`extension` boş). Uzantı olduğu gibi kalır: değiştirilmez, üstüne eklenmez.
struct TypedName {
    std::string_view stem;
    std::string_view extension;
};
TypedName split_typed_name(std::string_view name);

// Not adı için slug üretir (SPEC-core "Slug"). Geçersiz UTF-8 girdisi çökertmez: ASCII olmayan
// baytlar harf/rakam sayılmadığından ayırıcı olur; sonuç boşsa zaman damgalı ad döner.
// `now` parametre olması testleri deterministik yapar.
std::string slugify(std::string_view first_line, std::chrono::system_clock::time_point now,
                    std::size_t max_length = kDefaultSlugMaxLength);
// Noktalar arasını ayrı ayrı slug'lar (`a.tar`, `surum-1.2`); boş kalırsa zaman damgalı ad.
std::string slugify_stem(std::string_view stem, std::chrono::system_clock::time_point now,
                         std::size_t max_length = kDefaultSlugMaxLength);

}  // namespace noteapp::core
