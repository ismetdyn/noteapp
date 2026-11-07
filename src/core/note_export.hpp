#pragma once

// Düz dosyaya dışa aktarma: `note export --all`.
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace noteapp::core {

struct ExportedFile {
    NotePath note;
    std::filesystem::path file;  // çıktı dizinine göreli
};

struct RefusedFile {
    std::string path;
    std::string reason;  // reason.<değer> kataloğu: invalid_name, link, unreadable, exists
};

struct ExportAllReport {
    std::vector<ExportedFile> written;
    std::vector<UnreachableNote> unreachable;  // veride duruyor, güvenli yolu yok: note graph export verir
    std::vector<RefusedFile>
        refused;  // yol reddedildi ya da hedef doluydu; var olan dosyanın üzerine yazılmaz
};

// Tüm notları (baytları aynen) düz dosya olarak dir altına yazar, çöp `.trash/` altına (NotePath noktayı
// reddettiğinden canlı notla çakışmaz): uygulamadan bağımsız kurtarma yolu. Yollar uzaktan gelmiş olabilir:
// NotePath'ten yeniden geçer, hiçbir bileşen bağ olamaz, kök dışına çıkılamaz, üzerine yazılmaz. Dosyalar
// 0600, yeni dizinler 0700; var olan dizinin izni değişmez.
std::expected<ExportAllReport, Error> export_all_notes(const NoteStore& store,
                                                       const std::filesystem::path& dir);

}  // namespace noteapp::core
