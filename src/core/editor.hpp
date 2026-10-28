#pragma once

#include "core/config.hpp"
#include "core/error.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::core {

// Komutu argümanlara böler: boşlukla ayrılır, "çift" ve 'tek' tırnak destekli. Ters eğik çizgi düz
// karakterdir (Windows yolları); tek istisna çift tırnak içindeki \" dizisidir.
std::vector<std::string> split_command_line(std::string_view command);

// Sıra: config.editor → $VISUAL → $EDITOR → platform varsayılanı. Yapılandırılan değer olduğu gibi
// kabul edilir (varlığı çalıştırmada denetlenir); yalnız platform adayları PATH'te aranır.
// Hiçbiri yoksa Error{EditorNotFound, "error.editor_not_found", {{"candidates", "a, b"}}}.
std::expected<std::vector<std::string>, Error> resolve_editor(const Config& config);

// Dosya yolu son argüman olarak eklenir. Sıfır dışı çıkış: Error{EditorFailed, "error.editor_failed",
// {{"editor", ...}, {"code", ...}}}; program bulunamazsa EditorNotFound.
std::expected<void, Error> run_editor(const Config& config, const std::filesystem::path& file);

}  // namespace noteapp::core
