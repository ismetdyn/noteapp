#pragma once

#include "core/i18n.hpp"
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

using core::count_text;

using core::human_size;
using core::local_time_text;
using core::short_time_text;

}  // namespace noteapp::tui
