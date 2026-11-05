#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "detect/detect.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
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

// Ele alınan girdi: dosya, klasör ya da atlanan (bağ, aygıt). Gizli girdiler sayılmaz.
struct ImportProgress {
    std::size_t done = 0;   // şimdiki dahil başlanan girdi
    std::size_t total = 0;  // ön sayım; dosya sistemi arada büyürse done'dan küçük kalmaz
    std::string current;    // şimdiki girdinin yolu, UTF-8; güvenilmez (arayüz temizler)
};

// İçe almanın akışı (neyin alınacağı ImportOptions'ta).
struct ImportControl {
    // Dosyanın baytları ya da okuma hatasının metni; boş: diskten okunur. Test bellek hatasını burada
    // benzetir.
    std::function<std::expected<std::string, std::string>(const std::filesystem::path&)> read = {};
    // İstenince sıradaki girdiye geçilmez; alınanlar kalır, kalan girdiler raporda `cancelled`.
    std::stop_token stop = {};
    // Her girdiye başlarken, çağıranın iş parçacığında. Doluysa girdiler önce sayılır (payda).
    std::function<void(const ImportProgress&)> progress = {};
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
    // Durdurma ya da beklenmeyen istisnayla alınmayan girdi; içine inilmeyen klasör bir sayılır.
    std::size_t cancelled = 0;
    bool wrote = false;  // depo değişti: senkron haber almalı

    bool complete() const { return skipped.empty() && failed.empty() && cancelled == 0; }
};

// Yollar sırayla içe alınır; biri düşerse kalanlar sürer, özgün dosyalara dokunulmaz. Dosya: baytlar aynen
// (BOM, CRLF, ikili; ikili not app::classify ile salt okunurdur), ad dosya adından (core::split_typed_name),
// tip uzantıdan ya da içerikten, created_at doğum zamanından (yoksa mtime), updated_at mtime'dan. Klasör:
// kendi adıyla hedefin altına, alt klasörleriyle (boş olanlar da); hedefte aynı ad varsa birleşir. Bağ
// izlenmez, gizli girdi yalnız açıkça verilmişse alınır. Boyut tavanı yok: depolamanın sınırı öğe hatasıdır.
// Her not kendi yazma işlemindedir; dosya sayısına tavan yok. Belleğe sığmayan dosya (bad_alloc,
// length_error) süreci düşürmez, öğe hatasıdır. Yazmaları deponun kendi kilidi sıralar (NoteStore kopyaları
// aynı bağlantıyı paylaşır, thread-safe).
ImportReport import_paths(core::NoteStore& store, std::span<const std::filesystem::path> sources,
                          const ImportOptions& options, const ImportControl& control = {});

// Arayüzlerin arka plan içe alması (GUI ve TUI aynı yoldan): tek işçi, UI iş parçacığı beklemez. İlerleme ve
// rapor UI iş parçacığına `post` ile gelir; ilerleme birikmez (en çok bir bekleyen iş, en son değerle), rapor
// son ilerlemeden sonradır. `post` iş parçacığı güvenli olmalı ve işleri sırayla çalıştırmalı.
class ImportJob {
public:
    using Post = std::function<void(std::function<void()>)>;
    using OnProgress = std::function<void(const ImportProgress&)>;
    using OnDone = std::function<void(ImportReport)>;

    explicit ImportJob(Post post) : post_(std::move(post)) {}
    ImportJob(const ImportJob&) = delete;
    ImportJob& operator=(const ImportJob&) = delete;
    ~ImportJob() = default;  // işçi durdurulur ve beklenir (jthread); bekleyen işler çalıştırılmaz

    // Yalnız UI iş parçacığından. false: iş sürüyor (ikinci içe alma başlamaz), `post` boş ya da iş parçacığı
    // açılamadı; running() false kalır. `store` UI deposunun kopyası olabilir: yazmalar deponun kilidiyle
    // sıralanır.
    bool start(core::NoteStore store, std::vector<std::filesystem::path> sources, ImportOptions options,
               OnProgress on_progress, OnDone on_done);
    bool running() const { return running_; }  // rapor UI iş parçacığına gelene dek true
    void cancel() { worker_.request_stop(); }  // sıradaki girdiye geçilmez; rapor yine gelir

private:
    void work(core::NoteStore& store, const std::vector<std::filesystem::path>& sources,
              const ImportOptions& options, const std::stop_token& stop);  // işçide
    void publish(const ImportProgress& progress);                          // işçiden

    Post post_;
    OnProgress on_progress_;  // yalnız UI iş parçacığı
    OnDone on_done_;
    bool running_ = false;
    std::mutex mutex_;
    std::optional<ImportProgress> latest_;  // mutex_ altında; doluysa UI'ye gönderilmiş bir iş bekliyor
    std::jthread worker_;                   // son üye: ilk durur ve katılır
};

// İlerlemenin tek satırlık metni (TUI durum satırı, GUI durum çubuğu): sıra/toplam ve şimdiki girdinin adı.
std::string import_progress_text(const ImportProgress& progress);

// Config'in tespit ayarlarıyla seçenekler; `folder` hedef klasör (arayüzde app::new_note_folder).
ImportOptions import_options(const core::Config& config, std::string folder);

// Raporun metni, sırayla: alınanlar, durdurulanların sayısı, atlananlar, alınamayanlar, gizli sayısı, özet.
// `problem`: kullanıcı bakmalı (CLI stderr). Metin dosya sisteminden gelen yollar taşır: arayüz kendi
// temizliğinden geçirir.
struct ImportLine {
    std::string text;
    bool problem = false;
};
std::vector<ImportLine> import_report_lines(const ImportReport& report);
// Tek satırlık durum (TUI durum satırı, GUI durum çubuğu): özet, varsa ilk sorunun satırı.
std::string import_status(const ImportReport& report);

}  // namespace noteapp::app
