#pragma once

// Eski düz dosya deposundan geçiş ve düz dosyaya dışa aktarma (SPEC-graph "Eski depodan geçiş").
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace noteapp::core {

// notes_dir'de bu veri dizinine geçirilmemiş eski not var mı. Gizli girdiler ve oplog taşıyıcıları not
// değildir; .trash ve .keep sayılır (kullanıcının verisidir).
bool legacy_notes_pending(const std::filesystem::path& notes_dir,
                          const std::filesystem::path& data_directory);

// Bu dizinde geçirilecek yerel eski not kalmadı: bundan sonra git'le gelen dosyalar yazmayı kilitlemez.
std::expected<void, Error> mark_notes_dir_current(const std::filesystem::path& notes_dir,
                                                  const std::filesystem::path& data_directory);

struct MigrationOptions {
    bool skip_failed = false;  // geçemeyen dosyalar raporlanır, yerinde kalır, kapı açılır
};

struct SkippedFile {
    std::string path;
    // reason.<değer> kataloğu: invalid_name, invalid_folder, too_many_collisions, link, too_large,
    // unreadable, unreadable_folder (içi bilinmeyen dizin: izin ya da yol uzunluğu), verify_failed
    std::string reason;
};

struct MigrationReport {
    std::size_t imported = 0;
    std::size_t unchanged = 0;
    std::size_t folders = 0;
    std::vector<NotePath> conflict_copies;
    // Eski listede de görünmeyen ad, bağ ve 60 MiB üstü dosya: geçişi durdurmaz, dosya yerinde kalır,
    // raporlanır.
    std::vector<SkippedFile> skipped;
    std::vector<SkippedFile> failed;  // geçişi tamamlatmaz: kapı kapalı kalır
    std::filesystem::path backup;
    bool complete = false;
};

// Önce yedek, sonra toplu içe aktarma, sonra taze bağlantıyla bayt karşılaştırması. Eski dosyalara hiçbir
// koşulda dokunmaz. Tekrar çağrı etkisizdir. Hata yalnız yedeğin ya da deponun kendisindendir; dosya başına
// sorun rapordadır. Depoda görünmeyecek dosya (klasör sınırı aşan, düğümü güvenli yola çıkamayan) failed'dır.
std::expected<MigrationReport, Error> migrate_legacy_notes(const std::filesystem::path& notes_dir,
                                                           const std::filesystem::path& data_directory,
                                                           MigrationOptions options = {});

// Geçişin dışarıda bıraktığı dosyalar (skipped ve failed), path tam yolla. Son geçiş sonucudur: dosya
// düzeltilip geçiş yinelenince listeden çıkar. Dosya yoksa boş; okunamayan ya da bozuk liste hatadır, "hiçbir
// şey atlanmadı" sayılmaz: bu liste eski dosyaları silmeden önceki son uyarıdır.
std::expected<std::vector<SkippedFile>, Error>
read_skipped_files(const std::filesystem::path& data_directory);

// Kayıt bu notes_dir'den mi. Liste veri dizinini paylaşan bütün kökleri tutar; alt dizindeki ayrı kök
// kayıtta ayırt edilemez, üst kökün sayılır.
bool skipped_file_is_under(const SkippedFile& file, const std::filesystem::path& notes_dir);

// Listeyi (bütün kökler) unutur. Yalnız kaydı siler, eski dosyalara dokunmaz; geçiş yinelenince o kökün
// dosyaları yeniden listelenir. Liste yoksa etkisizdir.
std::expected<void, Error> clear_skipped_files(const std::filesystem::path& data_directory);

struct ExportedFile {
    NotePath note;
    std::filesystem::path file;  // çıktı dizinine göreli
};

struct ExportAllReport {
    std::vector<ExportedFile> written;
    std::vector<UnreachableNote> unreachable;  // veride duruyor, güvenli yolu yok: note graph export verir
    std::vector<SkippedFile>
        refused;  // yol reddedildi ya da hedef doluydu; var olan dosyanın üzerine yazılmaz
};

// Tüm notları (baytları aynen) düz dosya olarak dir altına yazar, çöp `.trash/` altına (NotePath noktayı
// reddettiğinden canlı notla çakışmaz): uygulamadan bağımsız kurtarma yolu. Yollar uzaktan gelmiş olabilir:
// NotePath'ten yeniden geçer, hiçbir bileşen bağ olamaz, kök dışına çıkılamaz, üzerine yazılmaz. Dosyalar
// 0600, yeni dizinler 0700; var olan dizinin izni değişmez.
std::expected<ExportAllReport, Error> export_all_notes(const NoteStore& store,
                                                       const std::filesystem::path& dir);

}  // namespace noteapp::core
