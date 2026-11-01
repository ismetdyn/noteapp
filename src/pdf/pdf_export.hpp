#pragma once

#include "core/error.hpp"

#include <expected>
#include <filesystem>
#include <string_view>

namespace noteapp::pdf {

enum class NoteFormat { Markdown, PlainText };

// Dizgi kuralı notun uzantısından gelir: yalnız `.md` (büyük/küçük harf duyarsız) Markdown'dır.
NoteFormat format_for(const std::filesystem::path& note_path);

// Metni A4, 15 mm kenar boşluklu PDF olarak yazar; çıktı atomiktir (yarım PDF kalmaz). Girdi güvenilmezdir
// (senkronla gelir): HTML yorumlanmaz, görsel/bağlantı hedefine hiçbir istek yapılmaz. Dosya okumaz: not
// baytları çağıranın NoteStore::read denetiminden geçer. Hata core::Error'dur; metni core::format_error
// üretir.
std::expected<void, core::Error> export_pdf(std::string_view text, NoteFormat format,
                                            const std::filesystem::path& output);

}  // namespace noteapp::pdf
