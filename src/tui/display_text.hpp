#pragma once

#include "core/text.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace noteapp::tui {

// Dışarıdan gelen metni (dosya adı, kesit, senkrondan gelen yol) terminale güvenli hale getirir: C0, DEL, C1,
// görünmez bidi/sıfır genişlikli karakterler (core::find_hidden_control) ve geçersiz UTF-8 U+FFFD olur.
// Çizimden hemen önce çağrılır; model ham baytı tutar.
std::string terminal_safe(std::string_view raw);

// "1 item", "3 items": `<key>_one` birdir; Türkçede ikisi aynı metindir.
std::string count_text(std::string_view key, std::size_t count);

using core::human_size;
using core::local_time_text;
using core::short_time_text;

}  // namespace noteapp::tui
