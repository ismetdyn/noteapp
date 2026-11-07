#pragma once

#include "core/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::core {

// Error::args ile aynı biçim: {yer tutucu adı, değer}.
using Args = std::vector<std::pair<std::string, std::string>>;

// "auto" | "tr" | "en"; bilinmeyen değer "en" sayılır. Süreç başında bir kez çağrılır (load_config'ten
// sonra). Çağrılmazsa dil "en"dir.
void init_i18n(std::string_view language_setting);

// Yalnız testler için: gömülü kataloglar yerine verilen TOML metinlerini kullanır.
void init_i18n_with_catalogs(std::string_view language_setting, std::string_view en_toml,
                             std::string_view tr_toml);

// Sistem diline göre "tr" veya "en": Windows'ta UI dili, POSIX'te LC_ALL → LC_MESSAGES → LANG.
std::string detect_language();

// Etkin dil: "tr" veya "en".
std::string current_language();

// Anahtarı etkin dilde çevirir, {ad} yer tutucularını doldurur. Geri düşüş: etkin dil → en → anahtarın
// kendisi (eksik anahtar için log'a bir kez uyarı). Başlatma sonrası okumalar iş parçacığı güvenlidir.
std::string tr(std::string_view key, const Args& args = {});

// "1 item", "3 items": `<key>_one` birdir; Türkçede ikisi aynı metindir.
std::string count_text(std::string_view key, std::size_t count);

// tr(message_key, args); `reason` argümanının `reason.<değer>` kataloğu varsa çevrilmiş hâliyle değişir.
std::string format_error(const Error& error);

}  // namespace noteapp::core
