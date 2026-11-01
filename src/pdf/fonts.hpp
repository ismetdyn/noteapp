#pragma once

#include <optional>
#include <string_view>

namespace noteapp::pdf {

// İkiliye gömülü DejaVu fontları; anahtar resources/'a göre göreli yoldur ("fonts/DejaVuSans.ttf").
// Bilinmeyen anahtar → nullopt. Dönen görünüm program boyunca geçerlidir. Hem PDF hem GUI bu fontları
// kullanır: sistem fontuna bağlı kalmamak Türkçe gliflerin her makinede aynı çıkmasını sağlar.
std::optional<std::string_view> embedded_font(std::string_view relative_path);

}  // namespace noteapp::pdf
