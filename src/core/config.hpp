#pragma once

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::core {

struct SyncConfig {
    bool enabled;
    std::string remote;
    std::string branch;
    int idle_seconds;
    std::string author_name;
    std::string author_email;

    bool operator==(const SyncConfig&) const = default;
};

// Arayüzde görünmeyen ayarlar; defaults.toml'dan (ileri düzey kullanıcı geçersiz kılabilir) gelir.
struct Tuning {
    std::size_t slug_max_length;
    double min_confidence;
    std::size_t max_scan_bytes;
    std::uintmax_t max_note_bytes;
    int rescan_seconds;
    int autosave_ms;
    int exit_timeout_seconds;
    std::uintmax_t log_max_bytes;  // log.txt bu boyutu aşınca log.1.txt'ye döner
    int sync_stale_lock_minutes;   // bundan eski senkron kilidi, sahibi yaşasa bile bayattır

    bool operator==(const Tuning&) const = default;
};

struct Config {
    std::string language;             // auto | tr | en
    std::filesystem::path notes_dir;  // ~ ve ortam değişkenleri genişletilmiş
    std::string editor;
    std::string default_type;
    std::string default_folder;
    std::vector<std::string> types;
    SyncConfig sync;
    Tuning tuning;

    bool operator==(const Config&) const = default;
};

struct LoadedConfig {
    Config config;
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

// Tek bir ayarı metinden (ör. CLI argümanı) şemadaki türüne çevirip `config`'e uygular. Doğrulama
// parse_config ile AYNIDIR (aynı kurallar); geçersiz değer/bilinmeyen anahtarda `config` değişmez ve ilk
// uyarı Error olarak döner (ConfigInvalid). `types` virgülle ayrılır. Kaydetmek çağıranın işidir.
std::expected<void, Error> set_config_value(Config& config, std::string_view key, std::string_view value);

// Kullanıcıya dönük tüm anahtarları (tuning hariç) config_file()'a atomik yazar; dizini oluşturur.
// Yorumlar korunmaz.
std::expected<void, Error> save_config(const Config& config);

}  // namespace noteapp::core
