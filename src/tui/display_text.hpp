#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace noteapp::tui {

// Dışarıdan gelen metni (dosya adı, kesit, senkrondan gelen yol) terminale güvenli hale getirir: C0, DEL, C1,
// bidi denetimleri ve geçersiz UTF-8 U+FFFD olur. Çizimden hemen önce çağrılır; model ham baytı tutar.
std::string terminal_safe(std::string_view raw);

// "512 B", "1.5 KiB", "2.0 MiB".
std::string human_size(std::uintmax_t bytes);

// Yerel saatle "2026-11-01 14:32".
std::string local_time_text(std::filesystem::file_time_type time);
// Bugünse "14:32", başka günse "2026-11-01" (yerel saat).
std::string short_time_text(std::filesystem::file_time_type time, std::chrono::system_clock::time_point now);

}  // namespace noteapp::tui
