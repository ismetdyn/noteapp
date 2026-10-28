#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::core {

// TODO(T08): defaults.toml'dan oku
inline constexpr std::size_t kDefaultSlugMaxLength = 60;

// Geçersiz UTF-8'de (fazla uzun kodlama, vekil çift, > U+10FFFF, kesik dizi) nullopt döner.
std::optional<std::u32string> decode_utf8(std::string_view utf8);
std::string encode_utf8(std::u32string_view codepoints);
bool is_valid_utf8(std::string_view utf8);

// ç ğ ı İ ö ş ü ve büyük harfleri ASCII karşılığına çevirir; diğer karakterler aynen kalır.
// Geçersiz UTF-8 girdisi değiştirilmeden döner.
std::string transliterate_turkish(std::string_view utf8);

// Not adı için slug üretir (SPEC-core "Slug"). Geçersiz UTF-8 girdisi çökertmez: ASCII olmayan
// baytlar harf/rakam sayılmadığından ayırıcı olur; sonuç boşsa zaman damgalı ad döner.
// `now` parametre olması testleri deterministik yapar.
std::string slugify(std::string_view first_line, std::chrono::system_clock::time_point now,
                    std::size_t max_length = kDefaultSlugMaxLength);

}  // namespace noteapp::core
