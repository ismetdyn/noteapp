#pragma once

#include "core/error.hpp"
#include "core/json_parser.hpp"
#include "core/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::core {

struct SyncConfig {
    bool enabled{};
    std::string remote;
    std::string branch;
    int idle_seconds{};
    std::string author_name;
    std::string author_email;

    bool operator==(const SyncConfig&) const = default;
};

// Dil sunucuları (TUI). Komut ve ayar yalnız bu yerel dosyadan gelir; not ya da graf verisi ikisini de
// taşıyamaz.
struct LspConfig {
    static constexpr std::size_t kMaxServers = 32;
    static constexpr std::size_t kMaxServersPerLanguage = 4;
    static constexpr std::size_t kMaxLanguageIdBytes = 32;
    static constexpr std::size_t kMaxArguments = 64;
    static constexpr std::size_t kMaxArgumentBytes = 4096;
    // [lsp.settings.<dil>] tablosu: iç içe derinlik (dil tablosu 0) ve toplam değer sayısı.
    static constexpr int kMaxSettingsDepth = 16;
    static constexpr std::size_t kMaxSettingsValues = 4096;

    bool enabled{};
    // LSP languageId -> sunucuların kabuksuz argv'leri; hepsi o dilin belgesini alır (html + htmx-lsp).
    std::map<std::string, std::vector<std::vector<std::string>>> servers{};
    // LSP languageId -> JSON nesnesi; sunucunun gömülü ayarının (sertleştirme) üstüne anahtar anahtar biner.
    std::map<std::string, Json> settings{};

    bool operator==(const LspConfig&) const = default;
};

// [update]: açılışta sürüm denetimi (TUI, GUI). Ayar ekranında görünmez; dosyadan değişir ([tuning] gibi).
struct UpdateConfig {
    // GitHub'ın kimliksiz API sınırı saatte 60 istektir: 0 dışında 1 saatten kısa aralık reddedilir.
    static constexpr std::uint64_t kMinCheckIntervalMs = 60ULL * 60ULL * 1000ULL;

    Limit check_interval;  // ms; 0 = açılışta denetim yok (elle `note update` yine çalışır)

    bool operator==(const UpdateConfig&) const = default;
};

// defaults.toml `graph_batch_ops`ın yapılandırmasız NoteStore'daki (test, geçiş) karşılığı. Üst değeri sabit
// değildir; çerçeve sınırı da onu kırpmaz (L4b: işlem kayıtlara bölünür).
inline constexpr std::size_t kDefaultGraphBatchOps = 30000;
// defaults.toml `limits.core.folder_path_bytes`ın yapılandırmasız NoteStore'daki (test, geçiş) karşılığı; bir
// test ikisinin aynı olduğunu denetler.
inline constexpr std::size_t kDefaultFolderPathBytes = 4096;

// Arayüzde görünmeyen ayarlar; defaults.toml'dan (ileri düzey kullanıcı geçersiz kılabilir) gelir.
struct Tuning {
    std::size_t slug_max_length;
    double min_confidence;
    std::size_t max_scan_bytes;
    Limit max_note_bytes;          // bundan büyük not düzenlenmez/aranmaz; 0 = sınırsız, birimli yazılabilir
    int rescan_seconds;
    int autosave_ms;
    int exit_timeout_seconds;
    std::uintmax_t log_max_bytes;  // log.txt bu boyutu aşınca log.1.txt'ye döner
    int sync_stale_lock_minutes;   // okunamayan kilit bundan eskiyse bayattır; sahibi canlı kilit devralınmaz
    int sync_timeout_seconds;      // AutoSync bir senkron sürecini bu kadar bekler, sonra sonlandırır
    std::size_t graph_batch_ops;   // graf işleminin işlem bütçesi; iş bölünür, kullanıcı işlemi reddedilmez
    std::size_t completion_min_prefix_bytes;  // TUI tamamlamasının öneri için beklediği en kısa önek
    Limit notice_ms;  // işlem bildiriminin (toast) ekranda kalma süresi; hata kalıcı, 0 = kendiliğinden kapanmaz
    double tui_list_share;  // TUI'de sol panelin ilk genişliği, pencereye oranı (0..1); sürüklenen genişlik geçer
    bool search_relations;  // aramalar etiket, ilişki adı ve bağlı notları da tarar (SPEC-search "İlişki
                            // araması")

    bool operator==(const Tuning&) const = default;
};

struct Config {
    std::string language;             // auto | tr | en
    std::filesystem::path notes_dir;  // ~ ve ortam değişkenleri genişletilmiş
    std::string editor;
    std::string default_type;
    std::string default_folder;
    std::vector<std::string> types;
    std::string icons;  // nerd | off; dosyadaki `auto` nerd okunur
    SyncConfig sync{};
    LspConfig lsp{};
    UpdateConfig update{};
    Tuning tuning{};
    Limits limits{};  // [limits.<modül>]; 0 = sınırsız (SPEC-core "Sınırlar")

    bool operator==(const Config&) const = default;
};

struct LoadedConfig {
    Config config{};
    std::vector<Error> warnings;  // code=ConfigInvalid; metin format_error ile üretilir
};

// Gömülü varsayılanlar, üstüne kullanıcının config_file()'ı (yoksa yalnız varsayılan). Çökmez:
// bozuk/geçersiz her şey varsayılana düşer ve uyarı üretir.
LoadedConfig load_config();

// Saf çekirdek: gömülü varsayılanların üstüne `user_toml` biner. Dosya erişimi yok.
LoadedConfig parse_config(std::string_view user_toml);

// Kullanıcıya dönük ayarlar (tuning ve token hariç) sıralı {anahtar, değer} metinleri olarak; `sync.*`
// noktalı anahtardır, liste "md, txt" biçiminde birleşir. `note config list/get` bunu kullanır.
std::vector<std::pair<std::string, std::string>> config_entries(const Config& config);

// [limits] anahtarları "limits.<modül>.<ad>" ve dosyadaki yazımları; `note config list/get` bunu da gösterir.
// Ayar ekranları göstermez (yalnız dosya ve CLI).
std::vector<std::pair<std::string, std::string>> limit_entries(const Config& config);

// Tek bir ayarı metinden (ör. CLI argümanı) şemadaki türüne çevirip `config`'e uygular. Doğrulama
// parse_config ile AYNIDIR (aynı kurallar); geçersiz değer/bilinmeyen anahtarda `config` değişmez ve ilk
// uyarı Error olarak döner (ConfigInvalid). `types` virgülle ayrılır; `limits.*` anahtarları da kabul edilir.
// Kaydetmek çağıranın işidir.
std::expected<void, Error> set_config_value(Config& config, std::string_view key, std::string_view value);

// Kullanıcıya dönük tüm anahtarları ve [tuning]'in yalnız gömülü varsayılandan farklı anahtarlarını
// config_file()'a atomik yazar; dizini oluşturur. [lsp] ve [limits] dosyadaki hâliyle kalır; [limits]'te
// yalnız bellekte değişen anahtar üzerine yazılır. Yorumlar korunmaz.
std::expected<void, Error> save_config(const Config& config);

}  // namespace noteapp::core
