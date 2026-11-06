#pragma once

// `note` alt komutlarının tek tablosu: ayrılmış sözcükler (args.cpp) ve kabuk tamamlama betikleri
// (`note completion zsh|bash|fish`) buradan türer. Yeni komut ya da seçenek önce buraya eklenir; açıklamalar
// i18n anahtarıdır, menüde kullanıcının dilinde görünür.
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace noteapp::cli {

// Bir seçeneğin ya da konumlu argümanın değeri nereden tamamlanır.
enum class ValueKind {
    None,       // bayrak: değer almaz
    Text,       // serbest metin: tamamlanmaz
    Note,       // not yolu (`note __complete note`)
    Folder,     // klasör (`note __complete folder`)
    Type,       // tip, config `types` (`note __complete type`)
    View,       // kayıtlı görünüm adı (`note __complete view`)
    ConfigKey,  // `note config` anahtarı (`note __complete config-key`)
    Query,      // ls/find sorgusu: anahtar adları ve tag:/type:/in:/view: değerleri (`note __complete query`)
    Command,    // bu tablonun komut adları (`note help KOMUT`)
    File,       // dosya sistemi yolu: kabuk tamamlar
    Directory,  // dosya sistemi dizini: kabuk tamamlar
};

struct OptionSpec {
    std::string_view name;   // "--folder" ya da "-f"
    std::string_view alias;  // eş ad ("-f") ya da boş
    ValueKind value;
    std::string_view description;  // i18n anahtarı
};

struct VerbSpec {
    std::string_view name;
    std::string_view description;  // i18n anahtarı
    std::span<const OptionSpec> options;
    ValueKind operand;
};

// Genel yardımdaki bölüm (`note help`): sık kullanılanlar üstte.
enum class CommandGroup { Notes, Sync, Advanced, Other };

struct CommandSpec {
    std::string_view name;
    std::string_view description;  // i18n anahtarı
    std::span<const OptionSpec> options;
    ValueKind operand;
    // Boş değilse ilk argüman bunlardan biridir; seçenek ve konumlu argüman o zaman fiilde durur, komutun
    // options/operand alanları kullanılmaz.
    std::span<const VerbSpec> verbs;
    CommandGroup group;
    // İlk argüman olarak komutun kendisi: kısa bayrak (`-l`, TUI tuşuyla aynı harf) ve onun uzun eşi `--ad`.
    // Boşsa komutun bayrağı yoktur, uzun eşi de (`--help` hızlı eklemenin yardım seçeneği kalır).
    std::string_view flag = "";
    std::string_view alias = "";  // eş ad (`cat` = `show`) ya da boş
};

// Kabukların gizli yardımcı komutu: dinamik değerleri satır satır yazar. Ayrılmıştır ama yardımda ve
// tamamlamada görünmez.
inline constexpr std::string_view kCompleteCommand = "__complete";

namespace command_table_detail {

using O = OptionSpec;
using V = VerbSpec;
using K = ValueKind;

inline constexpr O kNoSync{"--no-sync", "", K::None, "cli.opt_no_sync"};
inline constexpr O kJson{"--json", "", K::None, "cli.opt_json"};
inline constexpr O kOutput{"--output", "-o", K::File, "cli.opt_output"};
inline constexpr O kQuiet{"--quiet", "-q", K::None, "cli.opt_quiet"};
// Uzun çıktı veren komutlar bunu taşır: tabloda olması komutu sayfalanabilir yapar (args.cpp, pager.hpp).
inline constexpr O kNoPager{"--no-pager", "", K::None, "cli.opt_no_pager"};

inline constexpr std::array kAdd{
    // Düz ekleme, alt komutsuz `note` ile aynı; ilk argümanda komut adını metin yapar (`note -a ls`).
    O{"--add", "-a", K::None, "cli.opt_add"},
    O{"--type", "-t", K::Type, "cli.opt_type"},   O{"--folder", "-f", K::Folder, "cli.opt_folder"},
    O{"--name", "-n", K::Text, "cli.opt_name"},   O{"--editor", "-e", K::None, "cli.opt_editor"},
    kQuiet,                                       kNoSync,
    O{"--help", "-h", K::None, "cli.opt_help"},   O{"--version", "", K::None, "cli.opt_version"},
};
inline constexpr O kLimit{"--limit", "-n", K::Text, "cli.opt_limit"};
inline constexpr std::array kLs{
    O{"--folder", "-f", K::Folder, "cli.opt_in_folder"}, kLimit, O{"--long", "-l", K::None, "cli.opt_long"},
    O{"--print0", "-0", K::None, "cli.opt_print0"},     kJson,
    kNoPager,
};
inline constexpr std::array kFind{kLimit, O{"--long", "-l", K::None, "cli.opt_find_long"}, kJson, kNoPager};
inline constexpr std::array kNoPagerOnly{kNoPager};
inline constexpr std::array kJsonPaged{kJson, kNoPager};
inline constexpr std::array kNoSyncOnly{kNoSync};
inline constexpr std::array kRm{O{"--yes", "-y", K::None, "cli.opt_yes"}, kQuiet, kNoSync};
inline constexpr std::array kMv{kQuiet, kNoSync};
inline constexpr std::array kExport{
    O{"--pdf", "", K::None, "cli.opt_pdf"},
    kOutput,
    O{"--all", "", K::Directory, "cli.opt_all"},
    O{"--ignore-skipped", "", K::None, "cli.opt_ignore_skipped"},
};
inline constexpr std::array kQuery{
    O{"--write", "", K::None, "cli.opt_write"},
    kJson,
    O{"--param", "", K::Text, "cli.opt_param"},
};
inline constexpr std::array kJsonOnly{kJson};
inline constexpr std::array kImport{
    O{"--folder", "-f", K::Folder, "cli.opt_folder"}, O{"--type", "-t", K::Type, "cli.opt_type"},
    O{"--name", "-n", K::Text, "cli.opt_name"},       O{"--overwrite", "", K::None, "cli.opt_overwrite"},
    kJson,                                            kQuiet,
    kNoSync,
};
inline constexpr std::array kSyncInit{O{"--replace", "", K::None, "cli.opt_replace"}};
inline constexpr std::array kGraphMigrate{O{"--new", "", K::None, "cli.opt_new"},
                                          O{"--skip-failed", "", K::None, "cli.opt_skip_failed"}};
inline constexpr std::array kGraphStatus{O{"--ignore-skipped", "", K::None, "cli.opt_ignore_skipped"},
                                         O{"--clear-skipped", "", K::None, "cli.opt_clear_skipped"}};
inline constexpr std::array kGraphExport{O{"--format", "", K::Text, "cli.opt_format"}, kOutput};
inline constexpr std::array kOverwrite{O{"--overwrite", "", K::None, "cli.opt_overwrite"}};
inline constexpr std::array kViewSave{O{"--force", "", K::None, "cli.opt_force"}};

inline constexpr std::array kSync{
    V{"status", "cli.desc_sync_status", {}, K::None}, V{"on", "cli.desc_sync_on", {}, K::None},
    V{"off", "cli.desc_sync_off", {}, K::None},       V{"init", "cli.desc_sync_init", kSyncInit, K::Text},
    V{"login", "cli.desc_sync_login", {}, K::Text},   V{"logout", "cli.desc_sync_logout", {}, K::Text},
};
inline constexpr std::array kConfig{
    V{"path", "cli.desc_config_path", {}, K::None},
    V{"list", "cli.desc_config_list", kNoPagerOnly, K::None},
    V{"get", "cli.desc_config_get", {}, K::ConfigKey},
    V{"set", "cli.desc_config_set", {}, K::ConfigKey},
};
inline constexpr std::array kGraph{
    V{"migrate", "cli.desc_graph_migrate", kGraphMigrate, K::None},
    V{"status", "cli.desc_graph_status", kGraphStatus, K::None},
    V{"export", "cli.desc_graph_export", kGraphExport, K::None},
    V{"import", "cli.desc_graph_import", kOverwrite, K::File},
};
inline constexpr std::array kView{
    V{"save", "cli.desc_view_save", kViewSave, K::Text}, V{"ls", "cli.desc_view_ls", kJsonPaged, K::None},
    V{"run", "cli.desc_view_run", kJsonOnly, K::View},   V{"rm", "cli.desc_view_rm", {}, K::View},
    V{"show", "cli.desc_view_show", {}, K::View},
};
inline constexpr std::array kRelationAddOptions{O{"--name", "", K::Text, "cli.opt_relation_name"},
                                                O{"--directed", "", K::None, "cli.opt_directed"}};
inline constexpr std::array kRelationRmOptions{O{"--name", "", K::Text, "cli.opt_relation_name"},
                                               O{"--id", "", K::Text, "cli.opt_relation_id"}};
inline constexpr std::array kRelation{
    V{"add", "cli.desc_relation_add", kRelationAddOptions, K::Note},
    V{"rm", "cli.desc_relation_rm", kRelationRmOptions, K::Note},
    V{"rename", "cli.desc_relation_rename", {}, K::Note},
    V{"ls", "cli.desc_relation_ls", kJsonPaged, K::Note},
};
inline constexpr std::array kShells{
    V{"zsh", "cli.desc_completion_zsh", {}, K::None},
    V{"bash", "cli.desc_completion_bash", {}, K::None},
    V{"fish", "cli.desc_completion_fish", {}, K::None},
};

}  // namespace command_table_detail

// Hızlı eklemenin (alt komutsuz `note`) seçenekleri.
inline constexpr std::span<const OptionSpec> kAddOptions = command_table_detail::kAdd;

inline constexpr std::array<CommandSpec, 17> kCommands = [] {
    using namespace command_table_detail;
    using G = CommandGroup;
    return std::array<CommandSpec, 17>{{
        {"ls", "cli.desc_ls", kLs, K::Query, {}, G::Notes, "-l"},
        {"find", "cli.desc_find", kFind, K::Query, {}, G::Notes, "-s"},
        {"show", "cli.desc_show", kNoPagerOnly, K::Note, {}, G::Notes, "-S", "cat"},
        {"edit", "cli.desc_edit", kNoSyncOnly, K::Note, {}, G::Notes, "-E"},
        {"rm", "cli.desc_rm", kRm, K::Note, {}, G::Notes, "-d"},
        {"mv", "cli.desc_mv", kMv, K::Note, {}, G::Notes, "-m"},
        {"export", "cli.desc_export", kExport, K::Note, {}, G::Notes, "-x"},
        {"sync", "cli.desc_sync", {}, K::None, kSync, G::Sync},
        {"config", "cli.desc_config", {}, K::None, kConfig, G::Advanced},
        {"query", "cli.desc_query", kQuery, K::Text, {}, G::Advanced},
        {"graph", "cli.desc_graph", {}, K::None, kGraph, G::Advanced},
        {"tags", "cli.desc_tags", kJsonPaged, K::None, {}, G::Notes, "-T"},
        {"relation", "cli.desc_relation", {}, K::None, kRelation, G::Notes},
        {"view", "cli.desc_view", {}, K::None, kView, G::Advanced},
        {"import", "cli.desc_import", kImport, K::File, {}, G::Notes, "-i"},
        {"completion", "cli.desc_completion", {}, K::None, kShells, G::Other},
        {"help", "cli.desc_help", kNoPagerOnly, K::Command, {}, G::Other},
    }};
}();

// `--ls` gibi uzun komut bayrağı ya da boş (kısa bayrağı olmayan komutun uzun eşi de yoktur).
inline std::string long_flag(const CommandSpec& command) {
    return command.flag.empty() ? std::string() : "--" + std::string(command.name);
}

// Adı, eş adı, kısa ya da uzun bayrağı `word` olan komut ya da nullptr (gizli `__complete` tabloda değildir).
inline const CommandSpec* find_command(std::string_view word) {
    for (const CommandSpec& command : kCommands) {
        const bool is_flag = !command.flag.empty() && (command.flag == word || long_flag(command) == word);
        if (command.name == word || (!command.alias.empty() && command.alias == word) || is_flag)
            return &command;
    }
    return nullptr;
}

}  // namespace noteapp::cli
