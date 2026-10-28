#pragma once

#include "core/error.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::core {

// Boş değer "tanımsız" sayılır. Windows'ta geniş API ile okunup UTF-8'e çevrilir.
std::optional<std::string> get_env_utf8(std::string_view name);

// Programı PATH'te arar (Windows: PATHEXT'teki .com/.exe/.bat/.cmd uzantılarıyla; çalışma dizinine
// bakılmaz). Ad dizin içeriyorsa yalnız o dizine bakılır. Bulunamazsa nullopt.
std::optional<std::filesystem::path> find_program(std::string_view name);

// Kabuksuz başlatır; çocuk stdin/stdout/stderr'i devralır (editör terminali kullanır) ve çıkış kodu
// beklenir. `env_overrides` mevcut ortamın üstüne biner. Program bulunamazsa Error{NotFound,
// "error.process_not_found", {{"program", ...}}}; başlatma hatası Error{Io, "error.process_spawn_failed"}.
std::expected<int, Error>
run_and_wait(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& env_overrides = {});

}  // namespace noteapp::core
