#pragma once

#include <cstddef>
#include <string_view>

namespace noteapp::core {

// state_dir()/log.txt'ye "YYYY-MM-DDTHH:MM:SS seviye pid mesaj" satırı ekler (yerel saat). Hiçbir zaman
// fırlatmaz: G/Ç hatasında mesaj sessizce düşer (log uygulamayı çökertmemeli). Token/parola loglanmaz —
// çağıranın sorumluluğu.
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

// Dönüş eşiği (bayt): log.txt bunu aşınca log.1.txt'ye taşınır. Çağrılmazsa gömülü
// tuning.log_max_bytes kullanılır; uygulamalar config yüklenince bunu çağırır, testler küçük değer verir.
void init_log(std::size_t max_bytes);

}  // namespace noteapp::core
