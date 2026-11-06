#pragma once

#include "core/text.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace noteapp::tui {

// Kural core'dadır (app katmanı da kullanır: sorgu hücreleri bir kez temizlenir).
using core::terminal_safe;

// "1 item", "3 items": `<key>_one` birdir; Türkçede ikisi aynı metindir.
std::string count_text(std::string_view key, std::size_t count);

using core::human_size;
using core::local_time_text;
using core::short_time_text;

}  // namespace noteapp::tui
