#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "detect/detect.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace noteapp::app {

// Dosya sisteminden not olarak içe alma: CLI `note import`, TUI ve GUI aynı kuralı buradan çağırır.
struct ImportOptions {
    std::string folder = {};  // hedef klasör, "" kök; arayüzde app::new_note_folder, CLI'de default_folder
    std::string type = {};    // dolu: her dosyanın tipi, dosyanın uzantısının yerine geçer
    // Dolu: tek kaynağın adı. Dosyada not adıdır (uzantısı tipi verir, `type`ı geçer); klasörde hedefteki
    // klasörün adı. Birden çok kaynakla çağrılmaz (arayüz reddeder).
    std::string name = {};
    bool overwrite = false;    // aynı yolda not varsa baytları üzerine yazılır; yoksa deponun `-2` adı
    detect::Options detect{};  // uzantısız dosyanın tipi: yeni notun içerik tespiti
};

struct ImportReport {
    struct Imported {
        std::string source;  // dosya yolu, UTF-8; güvenilmez (terminale temizlenerek yazılır)
        core::NotePath note;
        bool renamed = false;      // ad doluydu: deponun `-2`, `-3`... adı
        bool overwritten = false;  // --overwrite: var olan notun baytları değişti
    };
    struct Skipped {
        std::string source;
        std::string reason;  // reason.<değer>: import_link, import_not_file
    };
    struct Failed {
        std::string source;
        core::Error error;
    };
    std::vector<Imported> imported = {};
    std::vector<Skipped> skipped = {};
    std::vector<Failed> failed = {};
    std::size_t hidden = 0;  // klasör gezilirken atlanan `.` ile başlayan girdi (içine inilmez)
    bool wrote = false;      // depo değişti: senkron haber almalı

    bool complete() const { return skipped.empty() && failed.empty(); }
};

// Yollar sırayla içe alınır; biri düşerse kalanlar sürer, özgün dosyalara dokunulmaz. Dosya: baytlar aynen
// (BOM, CRLF, ikili; ikili not app::classify ile salt okunurdur), ad dosya adından (core::split_typed_name),
// tip uzantıdan ya da içerikten, created_at doğum zamanından (yoksa mtime), updated_at mtime'dan. Klasör:
// kendi adıyla hedefin altına, alt klasörleriyle (boş olanlar da); hedefte aynı ad varsa birleşir. Bağ
// izlenmez, gizli girdi yalnız açıkça verilmişse alınır. Boyut tavanı yok: depolamanın sınırı öğe hatasıdır.
// Her not kendi yazma işlemindedir; dosya sayısına tavan yok. Depo kilidi çağıranın işidir.
ImportReport import_paths(core::NoteStore& store, std::span<const std::filesystem::path> sources,
                          const ImportOptions& options);

// Config'in tespit ayarlarıyla seçenekler; `folder` hedef klasör (arayüzde app::new_note_folder).
ImportOptions import_options(const core::Config& config, std::string folder);

// Raporun metni, sırayla: alınanlar, atlananlar, alınamayanlar, gizli sayısı, özet. `problem`: kullanıcı
// bakmalı (CLI stderr). Metin dosya sisteminden gelen yollar taşır: arayüz kendi temizliğinden geçirir.
struct ImportLine {
    std::string text;
    bool problem = false;
};
std::vector<ImportLine> import_report_lines(const ImportReport& report);
// Tek satırlık durum (TUI durum satırı, GUI durum çubuğu): özet, varsa ilk sorunun satırı.
std::string import_status(const ImportReport& report);

}  // namespace noteapp::app
