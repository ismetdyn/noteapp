#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace noteapp::detect {

// Satır biçimiyle kesin ayrılan tipler: anahtar/değer ailesi, işaretleme ve Dockerfile. Puanlamaya girmezler,
// çünkü bir tek kod dışı satır bile (ör. `echo`) kararı bozar; ortalama puan bunu göremez.
enum class Structured { Env, Properties, Ini, Toml, Yaml, Xml, Html, Dockerfile };

// `lines`: boş olmayan satırlar, dosyadaki sırayla.
std::optional<Structured> classify_structure(const std::vector<std::string_view>& lines);

}  // namespace noteapp::detect
