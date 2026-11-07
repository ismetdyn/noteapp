#pragma once

#include "core/error.hpp"
#include "core/note_path.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace noteapp::app {

// İkili notun dışa aktarma hatasının anahtarı: CLI onu kullanıcı hatası (çıkış 1) sayar, yol hatası değil.
inline constexpr std::string_view kExportBinaryKey = "app.export_binary";

// PDF'in önerilen dosya adı (CLI, TUI, GUI aynı): notun adı + `.pdf`; ad core::split_note_path kuralıyla
// bölünür (`a.tar.gz` -> `a.tar.pdf`).
std::string default_export_name(const core::NotePath& note);

// Çıktı yolunda bir ad var mı. Kırık bağ da var olan addır (exists bağı izler, onu "yok" sayıp sormadan
// yazardı). Varsa ne yapılacağını arayüz sorar: TUI ikinci Enter, GUI onay kutusu, CLI `--force`.
bool export_target_exists(const std::filesystem::path& output);

// Notun baytlarından (NoteStore::read'in döndürdüğü, dosya yeniden açılmaz) A4 PDF yazar; varsa hedefin
// yerine. İkili not reddedilir (InvalidPath, kExportBinaryKey), hiçbir şey yazılmaz.
std::expected<void, core::Error> export_note_pdf(std::string_view bytes, const core::NotePath& note,
                                                 const std::filesystem::path& output);

}  // namespace noteapp::app
