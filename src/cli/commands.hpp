#pragma once

#include "cli/args.hpp"
#include "cli/term.hpp"
#include "core/config.hpp"
#include "core/note_query.hpp"
#include "core/note_store.hpp"
#include "graph/query.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
// args.rest: export --format jsonl [-o DOSYA] | import DOSYA
int run_graph(const ParsedArgs& args, const core::Config& config);
int run_export_all(const std::string& directory, const core::Config& config);
// args.rest: [--write] [--json] [--param K=D]... CYPHER. Sınırlar kullanıcının profilidir: limits.query.*.
int run_query(const ParsedArgs& args, const core::Config& config);
// `note query`nin ve `view run`ın stdout biçimi: tablo (app::query_table hücreleri) ya da `--json` (ham
// değer; commands_query.cpp).
std::string query_output(const core::UserQuery& query, bool json);
int run_tags(const ParsedArgs& args, const core::Config& config);  // args.rest: [--json]
// args.rest: YOL... [--folder K] [--type T] [--name AD] [--overwrite] [--json] [--no-sync]
int run_import(const ParsedArgs& args, const core::Config& config);
// args.rest: save AD CYPHER [--force] | ls [--json] | run AD [--json] | rm AD | show AD
int run_view(const ParsedArgs& args, const core::Config& config);
// args.rest: add A B [--name AD] [--directed] | rm A B [--name AD | --id KİMLİK] | rename A B ESKİ YENİ |
// ls [NOT] [--json] (commands_graph.cpp)
int run_relation(const ParsedArgs& args, const core::Config& config);
// args.rest: get NOT [ANAHTAR] [--json] | set NOT ANAHTAR DEĞER [--type text|number|bool] | rm NOT ANAHTAR
// (commands_props.cpp; SPEC-cli "Özellikler").
int run_prop(const ParsedArgs& args, const core::Config& config);
// `note relation prop …`: rest[0] fiildir (get|set|rm), hedef ilişkinin kimliği (32 hex, `relation ls NOT`).
int run_relation_prop(std::span<const std::string> rest, const core::Config& config);
// `note relation ls NOT`un kimliği (32 küçük ya da büyük hex); başka metin nullopt.
std::optional<core::RelationId> relation_id_from_hex(std::string_view text);
// args.rest: zsh | bash | fish. Tamamlama betiğini stdout'a yazar (commands_complete.cpp).
int run_completion(const ParsedArgs& args);
// args.rest: TÜR [ÖNEK]. Kabuk betiklerinin gizli yardımcısı: adayları satır satır yazar, salt okur.
int run_complete(const ParsedArgs& args, const core::Config& config);
// Alt komut dağıtıcısı; gerçeklenmemiş olanlar "henüz kullanılamıyor" (çıkış 2) verir.
int run_reserved(const ParsedArgs& args, const core::Config& config);
int run_invalid(const ParsedArgs& args);

// Ortak yardımcılar (commands.cpp / sync_commands.cpp).

// Hatayı stderr'e yazar ve türünden çıkış kodunu döner: geçersiz yol/ad 2, bulunamadı/çakışma 1, kalanı 3.
int fail(const core::Error& error);

// Kullanım hataları (çıkış 2, SPEC-cli "Hata biçimi"): ilk satır bugünkü mesajdır (betikler onu arar), ardından
// en çok iki girintili ipucu satırı: yakın sözcük önerisi ve `note help KOMUT`. `command` "" = hızlı ekleme.
int usage_error(std::string_view command);  // `cli.<command>_usage`
int unknown_option_error(std::string_view command, const std::string& option);
int missing_value_error(std::string_view command, const std::string& option);
// Ortak ipucu satırları: varsa "did you mean", sonra `note help KOMUT` (command "" ise `note --help`).
void print_hints(std::string_view command, std::optional<std::string_view> guess);

// show/edit/export'un ortak "en iyi eşleşme" kuralı (commands.cpp); eşleşme yoksa mesajı yazıp çıkış kodunu
// döner.
// max_candidate: limits.search.fuzzy_candidate_chars (sınırsız SIZE_MAX).
std::expected<core::NoteInfo, int> pick_best_note(const core::NoteStore& store, const std::string& query,
                                                  std::size_t max_candidate);

// `note config list` anahtarları: ayar ekranlarınınkiler ve [limits] (yalnız dosya ve CLI).
std::vector<std::pair<std::string, std::string>> all_entries(const core::Config& config);

// Kaydetmek için config'i diskten yeniden okur. Bozuk (sözdizimi hatalı/okunamayan) dosya varsayılanlarla
// okunur ve üzerine yazmak elle yazılmış ayarları yok ederdi: hatayı yazar, nullopt döner (çıkış 1).
std::optional<core::Config> load_config_for_update();

// Notlar değiştiyse ve `sync.enabled` iken (`--no-sync` yoksa) ayrık `note sync --background` başlatır,
// beklemez (SPEC-cli "Senkron"). Başlatılamazsa yalnız loglanır: not zaten kaydedildi.
void start_background_sync(const core::Config& config, bool no_sync);

}  // namespace noteapp::cli
