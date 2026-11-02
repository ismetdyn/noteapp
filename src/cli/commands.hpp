#pragma once

#include "cli/args.hpp"
#include "core/config.hpp"
#include "core/note_store.hpp"

#include <expected>
#include <optional>
#include <string>

namespace noteapp::cli {

// Çıkış kodları (SPEC-cli "Çıkış kodları").
inline constexpr int kExitOk = 0;
inline constexpr int kExitUser = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitInternal = 3;
inline constexpr int kExitSync = 4;

// Komut gövdeleri düz fonksiyonlardır; hepsi çıkış kodunu döner, çıktıyı stdout/stderr'e yazar.
int run_add(const ParsedArgs& args, const core::Config& config);
int run_help(const ParsedArgs& args);  // args.command doluysa yalnız o komutun yardımı
int run_config(const ParsedArgs& args, const core::Config& config);  // args.rest: path|list|get K|set K V
int run_version();
int run_rm(const ParsedArgs& args, const core::Config& config);    // args.rest: YOL [-y] [--no-sync]
int run_mv(const ParsedArgs& args, const core::Config& config);    // args.rest: KAYNAK HEDEF [--no-sync]
int run_ls(const ParsedArgs& args, const core::Config& config);    // args.rest: [SORGU] [-f KLASÖR]
int run_show(const ParsedArgs& args, const core::Config& config);  // args.rest: SORGU
int run_edit(const ParsedArgs& args, const core::Config& config);  // args.rest: SORGU [--no-sync]
int run_find(const ParsedArgs& args, const core::Config& config);  // args.rest: TERİM… [--limit N]
// args.rest: [] | status | on | off | init REMOTE [--replace] | login [HOST] | logout [HOST] | --background
int run_sync(const ParsedArgs& args, const core::Config& config);
int run_export(const ParsedArgs& args, const core::Config& config);  // args.rest: --pdf SORGU|YOL [-o ÇIKTI]
// args.rest: migrate | export --format jsonl [-o DOSYA] | import DOSYA
int run_graph(const ParsedArgs& args, const core::Config& config);
int run_export_all(const std::string& directory, bool ignore_skipped, const core::Config& config);
int run_query(const ParsedArgs& args);  // args.rest: [--write] [--json] [--param K=D]... CYPHER
// Alt komut dağıtıcısı; gerçeklenmemiş olanlar "henüz kullanılamıyor" (çıkış 2) verir.
int run_reserved(const ParsedArgs& args, const core::Config& config);
int run_invalid(const ParsedArgs& args);

// Ortak yardımcılar (commands.cpp / sync_commands.cpp).

// Hatayı stderr'e yazar ve türünden çıkış kodunu döner: geçersiz yol/ad 2, bulunamadı/çakışma 1, kalanı 3.
int fail(const core::Error& error);

// show/edit/export'un ortak "en iyi eşleşme" kuralı (commands.cpp); eşleşme yoksa mesajı yazıp çıkış kodunu
// döner.
std::expected<core::NoteInfo, int> pick_best_note(const core::NoteStore& store, const std::string& query);

// stdin gerçek bir terminal mi (Windows'ta NUL aygıtı TTY sayılmaz).
bool stdin_is_tty();

// Kaydetmek için config'i diskten yeniden okur. Bozuk (sözdizimi hatalı/okunamayan) dosya varsayılanlarla
// okunur ve üzerine yazmak elle yazılmış ayarları yok ederdi: hatayı yazar, nullopt döner (çıkış 1).
std::optional<core::Config> load_config_for_update();

// Notlar değiştiyse ve `sync.enabled` iken (`--no-sync` yoksa) ayrık `note sync --background` başlatır,
// beklemez (SPEC-cli "Senkron"). Başlatılamazsa yalnız loglanır: not zaten kaydedildi.
void start_background_sync(const core::Config& config, bool no_sync);

}  // namespace noteapp::cli
