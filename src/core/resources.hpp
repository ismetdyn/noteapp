#pragma once

#include <optional>
#include <string_view>

namespace noteapp::core {

// Derlemede ikiliye gömülen resources/ dosyaları; anahtar resources/'a göre göreli yoldur
// ("defaults.toml", "i18n/tr.toml"). Bilinmeyen anahtar → nullopt. Dönen görünüm program boyunca geçerlidir.
std::optional<std::string_view> embedded_resource(std::string_view relative_path);

}  // namespace noteapp::core
