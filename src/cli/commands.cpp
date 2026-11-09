#include "cli/commands.hpp"

#include "app/import_files.hpp"
#include "app/listing.hpp"
#include "app/pdf_preview.hpp"
#include "app/settings.hpp"
#include "cli/command_table.hpp"
#include "cli/confirm.hpp"
#include "cli/highlight.hpp"
#include "cli/suggest.hpp"
#include "cli/term.hpp"
#include "core/editor.hpp"
#include "core/error.hpp"
#include "core/file_stat.hpp"
#include "core/i18n.hpp"
#include "core/json_text.hpp"
#include "core/note_store.hpp"
#include "core/note_views.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "core/version.hpp"
#include "search/content_index.hpp"
#include "search/fuzzy.hpp"
#include "search/query.hpp"
#include "search/relation_search.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace noteapp::cli {

using core::append_json_string;

namespace {

bool stdout_is_tty() {
    return stream_is_tty(Stream::Out);
}

// Terminale temizlenmiş, boruya bayt bayt (highlight.hpp sözleşmesi).
std::string terminal_or_bytes(std::string_view text) {
    return stdout_is_tty() ? sanitize_for_terminal(text) : std::string(text);
}

// Ham baytlar: dönüştürme yok (CRLF/BOM/geçersiz UTF-8 olduğu gibi kalır; çıktıyı NoteStore doğrular).
std::string read_all_stdin() {
    std::string data;
    char buffer[8192];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, stdin)) > 0)
        data.append(buffer, n);
    return data;
}

bool is_blank(const std::string& text) {
    return std::ranges::all_of(
        text, [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

// Hata türü → çıkış kodu. Geçersiz yol/ad/tip kullanıcının argümanından gelir (2); var olmayan/çakışan/
// dışarıdan değişen kullanıcı kaynaklıdır (1); kalanı G/Ç veya iç hata (3).
int exit_code_for(const core::Error& error) {
    using Code = core::Error::Code;
    switch (error.code) {
    case Code::InvalidPath:
    case Code::InvalidQuery:
        return kExitUsage;
    case Code::NotFound:
    case Code::AlreadyExists:
    case Code::ModifiedExternally:
    case Code::NotEmpty:
        return kExitUser;
    default:
        return kExitInternal;
    }
}

}  // namespace

int fail(const core::Error& error) {
    std::cerr << sanitize_for_terminal(core::format_error(error)) << '\n';
    return exit_code_for(error);
}

namespace {

void add_option_names(std::vector<std::string_view>& names, std::span<const OptionSpec> options) {
    for (const OptionSpec& option : options) {
        names.push_back(option.name);
        if (!option.alias.empty())
            names.push_back(option.alias);
    }
}

// Öneri adayları tablodan: komutun ve fiillerinin seçenekleri, her komutta geçerli `-h/--help`.
std::vector<std::string_view> option_names_of(std::string_view command) {
    std::vector<std::string_view> names{"--help", "-h"};
    const CommandSpec* spec = find_command(command);
    if (spec == nullptr) {
        add_option_names(names, kAddOptions);
        return names;
    }
    add_option_names(names, spec->options);
    for (const VerbSpec& verb : spec->verbs)
        add_option_names(names, verb.options);
    return names;
}

}  // namespace

void print_hints(std::string_view command, std::optional<std::string_view> guess) {
    if (guess)
        std::cerr << sanitize_for_terminal(core::tr("cli.hint_did_you_mean", {{"word", std::string(*guess)}}))
                  << '\n';
    std::cerr << (command.empty() ? core::tr("cli.hint_help_general")
                                  : core::tr("cli.hint_help_command", {{"command", std::string(command)}}))
              << '\n';
}

int usage_error(std::string_view command) {
    std::cerr << core::tr("cli." + std::string(command) + "_usage") << '\n';
    print_hints(command, std::nullopt);
    return kExitUsage;
}

int unknown_option_error(std::string_view command, const std::string& option) {
    std::cerr << sanitize_for_terminal(core::tr("cli.unknown_option", {{"option", option}})) << '\n';
    print_hints(command, suggest(option, option_names_of(command)));
    return kExitUsage;
}

int missing_value_error(std::string_view command, const std::string& option) {
    std::cerr << core::tr("cli.missing_value", {{"option", option}}) << '\n';
    print_hints(command, std::nullopt);
    return kExitUsage;
}

namespace {

namespace fs = std::filesystem;

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Tip/ad/klasör kuralları hızlı ekleme ile aynıdır. Başarıda tek satır yazdırıp notun yolunu, hatada hatayı
// döner.
std::expected<std::string, core::Error> save_note(const ParsedArgs& args, const core::Config& config,
                                                  const std::string& content) {
    // Adda yazılan uzantı -t'yi ve detect'i geçer; değiştirilmez, üstüne eklenmez.
    const std::string name = args.name.value_or("");
    const core::TypedName typed = core::split_typed_name(name);
    const bool chosen = args.type.has_value() || !typed.extension.empty();
    const std::string type =
        app::new_note_type(typed.extension, args.type, {}, content,
                           {config.default_type, config.tuning.min_confidence, config.tuning.max_scan_bytes});

    core::NoteStore store = core::NoteStore::for_config(config);
    const auto created = store.create(
        core::NewNote{args.folder.value_or(config.default_folder), std::string(typed.stem), type, content});
    if (!created)
        return std::unexpected(created.error());

    if (!args.quiet)
        std::cout << sanitize_for_terminal(core::tr(
                         "cli.saved", {{"path", created->string()},
                                       {"type", type},
                                       {"how", core::tr(chosen ? "cli.how_chosen" : "cli.how_detected")}}))
                  << '\n';
    start_background_sync(config, args.no_sync);
    return created->string();
}

// `note lss` bir not kaydeder (hızlı ekleme, SPEC-cli); tek sözcük bir komut adına yakınsa yazım hatası
// olabilir. İpucu yalnız stderr terminaldeyken: betik ve boru çıktısı hiç değişmez, otomatik düzeltme yok.
void hint_if_mistyped_command(const ParsedArgs& args, const std::string& path) {
    if (args.text.size() != 1 || args.text.front().find_first_of(" \t\n") != std::string::npos ||
        !stream_is_tty(Stream::Err))
        return;
    std::vector<std::string_view> names;
    for (const CommandSpec& command : kCommands)
        names.push_back(command.name);
    const auto guess = suggest(args.text.front(), names);
    if (!guess)
        return;
    std::cerr << sanitize_for_terminal(core::tr(
                     "cli.hint_saved_typo",
                     {{"word", args.text.front()}, {"command", std::string(*guess)}, {"path", path}}))
              << '\n';
}

// Değiştiren komutlar (rm/mv/edit) `--no-sync`'i her konumda kabul eder; kalan argümanlar döner.
std::vector<std::string> take_no_sync(const std::vector<std::string>& rest, bool& no_sync) {
    std::vector<std::string> kept;
    for (const std::string& arg : rest) {
        if (arg == "--no-sync")
            no_sync = true;
        else
            kept.push_back(arg);
    }
    return kept;
}

// Editör akışı (SPEC-cli "Hızlı ekleme kuralları"): geçici dosya yalnız veri kaybı riski yoksa silinir.
int run_editor_flow(const ParsedArgs& args, const core::Config& config, const std::string& initial) {
    const auto temp = core::create_private_temp_file(config.default_type, initial);
    if (!temp)
        return fail(temp.error());

    const auto edited = core::run_editor(config, *temp);
    const std::string content = read_file(*temp);
    std::error_code ec;
    const auto report_kept = [&] {
        std::cerr << core::tr("cli.temp_kept", {{"path", to_utf8(*temp)}}) << '\n';
    };

    if (!edited) {
        const int code = fail(edited.error());
        if (is_blank(content))
            fs::remove(*temp, ec);
        else
            report_kept();
        return code;
    }
    if (is_blank(content)) {
        fs::remove(*temp, ec);
        std::cerr << core::tr("cli.cancelled") << '\n';
        return kExitUser;
    }
    if (const auto saved = save_note(args, config, content); !saved) {
        const int code = fail(saved.error());
        report_kept();  // kayıt başarısız: yazılan metin kaybolmasın
        return code;
    }
    fs::remove(*temp, ec);
    return kExitOk;
}

// `--limit N`: pozitif tam sayı; değilse mesajı yazıp kullanım hatası (2) döner.
std::expected<std::size_t, int> parse_limit(const std::string& value) {
    std::size_t used = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value, &used);
    } catch (const std::exception&) {  // sayı değil / taşma: kullanım hatası
        used = 0;
    }
    if (used != value.size() || value.empty() || value[0] == '-' || parsed == 0) {
        std::cerr << sanitize_for_terminal(core::tr("cli.invalid_limit", {{"value", value}})) << '\n';
        return std::unexpected(kExitUsage);
    }
    return static_cast<std::size_t>(parsed);
}

// show/edit argümanları: yalnız konumsal değerler. Seçenekte hata mesajını yazar, çıkış kodunu döner.
struct BrowseArgs {
    std::vector<std::string> positional;
};

std::expected<BrowseArgs, int> parse_browse_args(std::string_view command, const std::vector<std::string>& rest) {
    BrowseArgs parsed;
    for (const std::string& arg : rest) {
        if (arg.size() > 1 && arg[0] == '-')
            return std::unexpected(unknown_option_error(command, arg));
        parsed.positional.push_back(arg);
    }
    return parsed;
}

}  // namespace

// "En iyi eşleşme" (SPEC-cli): tam yol varsa o; yoksa rank_notes ilk sonucu ve seçilen yol stderr'e yazılır.
// Eşleşme yoksa mesajı yazar ve 1 döner. Tek kaynak: show/edit/export aynı kuralı kullanır.
std::expected<core::NoteInfo, int> pick_best_note(const core::NoteStore& store, const std::string& query,
                                                  std::size_t max_candidate) {
    const std::vector<core::NoteInfo> notes = store.list();
    if (const auto exact = core::NotePath::parse(query)) {
        const auto it = std::ranges::find(notes, *exact, &core::NoteInfo::path);
        if (it != notes.end())
            return *it;
    }
    const auto ranked = search::rank_notes(query, notes, max_candidate);
    if (ranked.empty()) {
        std::cerr << sanitize_for_terminal(core::tr("cli.no_match", {{"query", query}})) << '\n';
        return std::unexpected(kExitUser);
    }
    const core::NoteInfo& best = notes[ranked.front().index];
    std::cerr << sanitize_for_terminal(core::tr("cli.chosen", {{"path", best.path.string()}})) << '\n';
    return best;
}

int run_add(const ParsedArgs& args, const core::Config& config) {
    std::string content;
    for (std::size_t i = 0; i < args.text.size(); ++i)
        content += (i == 0 ? "" : " ") + args.text[i];

    if (args.editor || (args.text.empty() && stdin_is_tty()))
        return run_editor_flow(args, config, content);
    if (args.text.empty())
        content = read_all_stdin();
    if (is_blank(content)) {
        std::cerr << core::tr("cli.empty_input") << '\n';
        return kExitUser;
    }
    const auto saved = save_note(args, config, content);
    if (!saved)
        return fail(saved.error());
    hint_if_mistyped_command(args, *saved);
    return kExitOk;
}

namespace {

// Token kasada (T27'de `note sync login`) tutulur; config'e yazılmaz/okunmaz (SPEC-cli kabul 8).
bool is_token_key(std::string_view key) {
    return key == "sync.token";
}

}  // namespace

std::vector<std::pair<std::string, std::string>> all_entries(const core::Config& config) {
    auto entries = core::config_entries(config);
    std::ranges::move(core::limit_entries(config), std::back_inserter(entries));
    return entries;
}

std::optional<core::Config> load_config_for_update() {
    core::LoadedConfig fresh = core::load_config();
    if (const auto broken = app::config_broken_warning(fresh)) {
        std::cerr << core::format_error(*broken) << '\n' << core::tr("cli.config_not_saved") << '\n';
        return std::nullopt;
    }
    return std::move(fresh.config);
}

int run_config(const ParsedArgs& args, const core::Config& config) {
    const std::vector<std::string>& rest = args.rest;
    const std::string verb = rest.empty() ? "" : rest.front();
    const std::size_t operands = rest.empty() ? 0 : rest.size() - 1;

    if (verb == "path" && operands == 0) {
        std::cout << to_utf8(core::config_file()) << '\n';
        return kExitOk;
    }
    if (verb == "list" && operands == 0) {
        for (const auto& [key, value] : all_entries(config))
            std::cout << key << " = " << value << '\n';
        return kExitOk;
    }
    if ((verb == "get" && operands == 1) || (verb == "set" && operands == 2)) {
        const std::string& key = rest[1];
        if (is_token_key(key)) {
            std::cerr << core::tr("cli.token_not_config") << '\n';
            return kExitUsage;
        }
        const auto entries = all_entries(config);
        const auto entry = std::ranges::find(entries, key, &std::pair<std::string, std::string>::first);
        if (entry == entries.end()) {
            std::cerr << sanitize_for_terminal(core::tr("cli.config_unknown_key", {{"key", key}})) << '\n';
            return kExitUsage;
        }
        if (verb == "get") {
            std::cout << entry->second << '\n';
            return kExitOk;
        }

        std::optional<core::Config> fresh = load_config_for_update();
        if (!fresh)
            return kExitUser;
        if (const auto set = core::set_config_value(*fresh, key, rest[2]); !set) {
            std::cerr << core::format_error(set.error()) << '\n' << core::tr("cli.config_not_saved") << '\n';
            return kExitUsage;
        }
        if (const auto saved = core::save_config(*fresh); !saved)
            return fail(saved.error());
        // Normalleşmiş değeri yazdır (ör. "md, py ,rs" → "md, py, rs"): kaydedilen budur.
        for (const auto& [saved_key, saved_value] : all_entries(*fresh))
            if (saved_key == key)
                std::cout << saved_key << " = " << saved_value << '\n';
        return kExitOk;
    }
    return usage_error("config");
}

int run_version() {
    std::cout << "note " << core::version() << '\n';
    return kExitOk;
}

int run_rm(const ParsedArgs& args, const core::Config& config) {
    bool yes = false;
    bool quiet = false;
    bool no_sync = false;
    std::vector<std::string> paths;
    for (const std::string& arg : take_no_sync(args.rest, no_sync)) {
        if (arg == "-y" || arg == "--yes") {
            yes = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg.size() > 1 && arg[0] == '-') {
            return unknown_option_error("rm", arg);
        } else {
            paths.push_back(arg);
        }
    }
    if (paths.size() != 1) {
        return usage_error("rm");
    }

    // Yalnız tam yol: fuzzy eşleşme yok. Silmeden önce yolun var olduğu doğrulanır (onay boşuna sorulmasın).
    const auto path = core::NotePath::parse(paths.front());
    if (!path)
        return fail(path.error());
    core::NoteStore store = core::NoteStore::for_config(config);
    const auto found = store.read(*path);
    if (!found)
        return fail(found.error());

    if (!yes) {
        if (!stdin_is_tty()) {
            std::cerr << sanitize_for_terminal(core::tr("cli.rm_needs_yes", {{"path", path->string()}}))
                      << '\n';
            return kExitUser;
        }
        std::cerr << sanitize_for_terminal(core::tr("cli.confirm_delete", {{"path", path->string()}}))
                  << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) || !is_delete_confirmed(answer)) {
            std::cerr << core::tr("cli.delete_not_confirmed") << '\n' << core::tr("cli.hint_rm_confirm") << '\n';
            return kExitUser;
        }
    }
    // Onaydaki hâl silinir: soru beklerken senkron notu değiştirdiyse not kalır (TUI/GUI ile aynı kural).
    if (auto removed = store.remove(*path, found->revision); !removed) {
        core::Error error = std::move(removed.error());
        if (error.code == core::Error::Code::ModifiedExternally)
            error.message_key = "error.changed_since_confirm";
        return fail(error);
    }
    if (!quiet)
        std::cout << sanitize_for_terminal(core::tr("cli.removed", {{"path", path->string()}})) << '\n';
    start_background_sync(config, no_sync);
    return kExitOk;
}

int run_mv(const ParsedArgs& args, const core::Config& config) {
    bool no_sync = false;
    bool quiet = false;
    std::vector<std::string> rest;
    for (const std::string& arg : take_no_sync(args.rest, no_sync)) {
        if (arg == "-q" || arg == "--quiet")
            quiet = true;
        else if (arg.size() > 1 && arg[0] == '-')
            return unknown_option_error("mv", arg);
        else
            rest.push_back(arg);
    }
    if (rest.size() != 2) {
        return usage_error("mv");
    }
    const auto from = core::NotePath::parse(rest[0]);
    if (!from)
        return fail(from.error());
    const auto to = core::NotePath::parse(rest[1]);
    if (!to)
        return fail(to.error());

    core::NoteStore store = core::NoteStore::for_config(config);
    if (const auto moved = store.move(*from, *to); !moved)
        return fail(moved.error());
    if (!quiet)
        std::cout << core::tr("cli.moved", {{"from", from->string()}, {"to", to->string()}}) << '\n';
    start_background_sync(config, no_sync);
    return kExitOk;
}

namespace {

struct LsArgs {
    std::string query;
    std::optional<std::string> folder;
    std::size_t limit = std::numeric_limits<std::size_t>::max();  // varsayılan sınırsız
    bool json = false;
    bool long_format = false;
    bool print0 = false;
};

std::expected<LsArgs, int> parse_ls_args(const std::vector<std::string>& rest) {
    LsArgs parsed;
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        const bool folder = arg == "-f" || arg == "--folder";
        if (folder || arg == "-n" || arg == "--limit") {
            if (i + 1 >= rest.size())
                return std::unexpected(missing_value_error("ls", arg));
            const std::string& value = rest[++i];
            if (folder) {
                parsed.folder = value;
                continue;
            }
            const auto limit = parse_limit(value);
            if (!limit)
                return std::unexpected(limit.error());
            parsed.limit = *limit;
        } else if (arg == "--json") {
            parsed.json = true;
        } else if (arg == "-l" || arg == "--long") {
            parsed.long_format = true;
        } else if (arg == "-0" || arg == "--print0") {
            parsed.print0 = true;
        } else if (arg.size() > 1 && arg[0] == '-') {
            return std::unexpected(unknown_option_error("ls", arg));
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() > 1)
        return std::unexpected(usage_error("ls"));
    if (!positional.empty())
        parsed.query = positional.front();
    return parsed;
}

std::int64_t unix_ms(std::filesystem::file_time_type when) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(core::to_system_time(when).time_since_epoch())
        .count();
}

// İlişkiyle bulunan satırın alanları: "reason" (tag|relation|linked), "label" ve nedenin alanları: relation
// "other", "direction"; linked "other", "hops" (SPEC-cli `find --json`).
void append_reason_json(std::string& out, const search::RelationReason& reason) {
    using Kind = search::RelationReason::Kind;
    out += reason.kind == Kind::Tag        ? ",\"reason\":\"tag\""
           : reason.kind == Kind::Relation ? ",\"reason\":\"relation\""
                                           : ",\"reason\":\"linked\"";
    out += ",\"label\":";
    append_json_string(out, reason.label);
    if (!reason.other.empty()) {
        out += ",\"other\":";
        append_json_string(out, reason.other);
    }
    if (reason.kind == Kind::Relation)
        out += reason.direction == core::RelationDirection::Outgoing   ? ",\"direction\":\"outgoing\""
               : reason.direction == core::RelationDirection::Incoming ? ",\"direction\":\"incoming\""
                                                                       : ",\"direction\":\"undirected\"";
    if (reason.kind == Kind::Linked)
        out += ",\"hops\":" + std::to_string(reason.hops);
}

// İlişki satırında yolun ardından neden: terminalde iki boşlukla, boruda TAB'la; neden tek satırdır.
std::string reason_suffix(const search::RelationReason& reason, bool tty) {
    const std::string text = single_line(app::reason_text(reason));
    return tty ? "  " + sanitize_for_terminal(text) : '\t' + text;
}

// İlişki satırları yalnız terminalde, -l ile ya da --json'da: düz boru çıktısı ve çıkış kodu eskisi gibi kalır
// (betikler `ls` yolları ve `find` `yol:satır: kesit` bekler; `note ls x | xargs note rm` fazladan not almaz).
bool shows_related(bool json, bool long_format) {
    return json || long_format || stdout_is_tty();
}

struct SearchCall {
    std::string_view query;
    app::SearchMode mode;
    std::string_view scope;  // klasör alt ağacı; boş: tüm notlar
    bool related;            // ilişki satırları (shows_related)
    std::size_t limit;
};

// TUI/GUI'nin arama hattı (app::Listing): sorgu argümanları, sıra, `sort:`, ilişki satırları doğrudan
// satırların altında. En çok `limit` satır. Bozuk argüman iletisi yazılır, çıkış 2 döner.
std::expected<std::vector<app::ListRow>, int> search_rows(app::Listing& listing, const core::NoteStore& store,
                                                          const core::Config& config, const SearchCall& call) {
    app::connect_store(listing, store, config);
    if (!call.related)
        listing.set_relation_depth(std::nullopt);
    listing.set_notes(store.list());
    app::SearchResult found = listing.search(call.query, call.mode, call.scope);
    if (found.error) {
        // Bozuk argümanın yeri (TUI'nin arama çubuğu gibi): kullanıcı hangi sözcüğün sayılmadığını görür.
        std::cerr << sanitize_for_terminal(core::tr("cli.query_argument_at",
                                                    {{"column", std::to_string(found.error->begin + 1)},
                                                     {"detail", core::format_error(found.error->error)}}))
                  << '\n';
        return std::unexpected(kExitUsage);
    }
    if (found.rows.size() > call.limit)  // ListRow varsayılan kurulamaz: resize değil erase
        found.rows.erase(found.rows.begin() + static_cast<std::ptrdiff_t>(call.limit), found.rows.end());
    return std::move(found.rows);
}

// [{"path","type","size","updated_at"}]; zaman unix ms (`view ls --json` ile aynı birim). İlişkiyle bulunan
// satır ayrıca nedenin alanlarını taşır (append_reason_json).
std::string ls_json(const std::vector<app::ListRow>& rows, const app::Listing& listing) {
    std::string out = "[";
    for (const app::ListRow& row : rows) {
        const core::NoteMeta& meta = *listing.meta_of(row.path);
        out += out.size() > 1 ? ",{\"path\":" : "{\"path\":";
        append_json_string(out, meta.path.string());
        out += ",\"type\":";
        append_json_string(out, core::split_note_path(meta.path.string()).type);
        out += ",\"size\":" + std::to_string(meta.size) + ",\"updated_at\":" +
               std::to_string(unix_ms(meta.updated_at));
        if (row.reason)
            append_reason_json(out, *row.reason);
        out += '}';
    }
    return out + "]\n";
}

std::size_t code_points(std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// `GÜNCELLEME  BOYUT  TÜR  YOL`: başlık satırı yok, TTY ve boru aynı (betik dostu); boyut sağa, tür sola yaslı.
std::vector<std::string> ls_long_lines(const std::vector<app::ListRow>& rows, const app::Listing& listing) {
    std::size_t size_width = 0;
    std::size_t type_width = 0;
    for (const app::ListRow& row : rows) {
        size_width = std::max(size_width, std::to_string(listing.meta_of(row.path)->size).size());
        type_width = std::max(type_width, code_points(core::split_note_path(row.path.string()).type));
    }
    std::vector<std::string> lines;
    for (const app::ListRow& row : rows) {
        const core::NoteMeta& meta = *listing.meta_of(row.path);
        const std::string size = std::to_string(meta.size);
        const std::string type = core::split_note_path(meta.path.string()).type;
        lines.push_back(core::local_time_text(meta.updated_at) + "  " + std::string(size_width - size.size(), ' ') +
                        size + "  " + type + std::string(type_width - code_points(type), ' ') + "  " +
                        meta.path.string());
    }
    return lines;
}

}  // namespace

int run_ls(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_ls_args(args.rest);
    if (!parsed)
        return parsed.error();
    std::string scope;  // "work" alt ağacı: "work/a.md" girer, "workshop.md" girmez
    if (parsed->folder) {
        const auto folder = core::NotePath::parse(*parsed->folder);
        if (!folder)
            return fail(folder.error());
        scope = folder->string();
    }

    // -0 akışı xargs -0 içindir: -l ile de, terminalde de yalnız doğrudan eşleşmelerin yolları (--json -0'ı geçer).
    const bool related = parsed->json || (!parsed->print0 && (parsed->long_format || stdout_is_tty()));
    core::NoteStore store = core::NoteStore::for_config(config);
    app::Listing listing;
    const auto rows = search_rows(listing, store, config,
                                  {parsed->query, app::SearchMode::Name, scope, related, parsed->limit});
    if (!rows)
        return rows.error();
    if (parsed->json) {
        std::cout << ls_json(*rows, listing);
        return kExitOk;
    }
    const bool tty = stdout_is_tty();
    const char separator = parsed->print0 ? '\0' : '\n';
    const std::vector<std::string> lines =
        parsed->long_format ? ls_long_lines(*rows, listing) : std::vector<std::string>{};
    for (std::size_t i = 0; i < rows->size(); ++i) {
        const app::ListRow& row = (*rows)[i];
        std::cout << terminal_or_bytes(parsed->long_format ? lines[i] : row.path.string())
                  << (row.reason ? reason_suffix(*row.reason, tty) : std::string()) << separator;
    }
    return kExitOk;
}

namespace {

// PDF'in kendisinden gelen hata (bozuk, şifreli, [limits.pdf] aşımı) kullanıcı kaynaklıdır (1): notun içeriği. İşçi
// başlatılamazsa fail()'in kuralı.
int pdf_failure(const core::Error& error) {
    using Code = core::Error::Code;
    if (error.code != Code::InvalidDocument && error.code != Code::Encrypted && error.code != Code::LimitExceeded)
        return fail(error);
    std::cerr << sanitize_for_terminal(core::format_error(error)) << '\n';
    return kExitUser;
}

// PDF notunun metni sayfa sayfa, her sayfanın önünde ayırıcı satır (SPEC-cli "show"). Metin işçide arındırılmıştır
// (denetim karakteri yok); terminalde yine de düz metin gibi temizlenir. Sayfa hatası stderr'e yazılır, kalan
// sayfalar sürer, çıkış 1. Ctrl-C süreci her zamanki gibi bitirir (130); işçi ebeveyni ölünce çıkar.
int show_pdf(std::string_view bytes, const core::Config& config) {
    int exit_code = kExitOk;
    const bool tty = stdout_is_tty();
    const auto read = app::read_pdf_text(
        bytes, config.limits.pdf, core::note_executable_beside_self(), {},
        [&](int page, int count, const std::expected<std::string, core::Error>& text) {
            std::cout << app::pdf_page_separator(page, count) << '\n';
            if (!text) {
                std::cout.flush();
                exit_code = std::max(exit_code, pdf_failure(text.error()));
                return;
            }
            std::cout << (tty ? sanitize_for_terminal(*text) : *text)
                      << (text->empty() || text->ends_with('\n') ? "" : "\n");
        });
    return read ? exit_code : pdf_failure(read.error());
}

}  // namespace

int run_show(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_browse_args("show", args.rest);
    if (!parsed)
        return parsed.error();
    if (parsed->positional.size() != 1)
        return usage_error("show");
    core::NoteStore store = core::NoteStore::for_config(config);
    const auto note =
        pick_best_note(store, parsed->positional.front(), config.limits.search.fuzzy_candidate_chars.cap());
    if (!note)
        return note.error();
    const auto content = store.read(note->path);
    if (!content)
        return fail(content.error());
    if (app::is_pdf_type(core::split_note_path(note->path.string()).type))
        return show_pdf(content->text, config);
    if (core::looks_binary(content->text)) {
        std::cerr << sanitize_for_terminal(core::tr("cli.binary_note", {{"path", note->path.string()}}))
                  << '\n';
        return kExitUser;
    }
    // Terminalde kaçış dizileri etkisizleşir; boruya/dosyaya içerik bayt bayt gider.
    // Terminalde son satır sonu yoksa eklenir: istem metnin sonuna yapışmasın.
    const std::string& text = content->text;
    if (stdout_is_tty())
        std::cout << sanitize_for_terminal(text) << (text.empty() || text.ends_with('\n') ? "" : "\n");
    else
        std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
    return kExitOk;
}

int run_edit(const ParsedArgs& args, const core::Config& config) {
    bool no_sync = false;
    const auto parsed = parse_browse_args("edit", take_no_sync(args.rest, no_sync));
    if (!parsed)
        return parsed.error();
    if (parsed->positional.size() != 1)
        return usage_error("edit");
    core::NoteStore store = core::NoteStore::for_config(config);
    const auto note =
        pick_best_note(store, parsed->positional.front(), config.limits.search.fuzzy_candidate_chars.cap());
    if (!note)
        return note.error();
    // Editör dosya ister: not özel geçici dosyaya çıkar, çakışma denetimiyle geri yazılır, dosya silinir.
    const auto changed =
        store.edit_in_temp_file(note->path, [&](const fs::path& file) -> std::expected<void, core::Error> {
            if (const auto edited = core::run_editor(config, file); !edited)
                return std::unexpected(edited.error());
            return {};
        });
    if (!changed)
        return fail(changed.error());
    // Kaydetmeden kapatılan editör için ağa çıkılmaz.
    if (*changed)
        start_background_sync(config, no_sync);
    return kExitOk;
}

namespace {

// [{"path","line","snippet","ranges","reason":"content"}]: eşleşen satır başına bir kayıt; ranges kesitteki
// [başlangıç, bitiş) bayt aralıklarıdır. Düz biçimdeki `yol:satır: kesit` ayrıştırmasının ':' belirsizliği
// yok. Ardından ilişkiyle bulunan not başına {"path","reason","label"} ve nedenin alanları: relation "other",
// "direction"; linked "other", "hops" (SPEC-cli).
std::string find_json(const std::vector<app::ListRow>& rows) {
    std::string out = "[";
    for (const app::ListRow& row : rows) {
        for (const search::Hit& hit : row.hits) {
            out += out.size() > 1 ? ",{\"path\":" : "{\"path\":";
            append_json_string(out, row.path.string());
            out += ",\"line\":" + std::to_string(hit.line) + ",\"snippet\":";
            append_json_string(out, hit.snippet);
            out += ",\"ranges\":[";
            for (std::size_t i = 0; i < hit.ranges.size(); ++i)
                out += (i == 0 ? "[" : ",[") + std::to_string(hit.ranges[i].first) + ',' +
                       std::to_string(hit.ranges[i].second) + ']';
            out += "],\"reason\":\"content\"}";
        }
        if (row.reason) {
            out += out.size() > 1 ? ",{\"path\":" : "{\"path\":";
            append_json_string(out, row.path.string());
            append_reason_json(out, *row.reason);
            out += '}';
        }
    }
    return out + "]\n";
}

}  // namespace

namespace {

// Boruya/dosyaya baytlar aynen gider; terminalde not baytları temizlenir (yol adı da C1 taşıyabilir).
// Vurgu yalnız renk açıkken (color_enabled: NO_COLOR, TERM=dumb, NOTEAPP_COLOR); renk kapalıyken bilgi
// kaybolmaz, eşleşme satırda zaten görünür. İlişki satırı: yol ve nedeni (reason_suffix).
void print_find_rows(const std::vector<app::ListRow>& rows) {
    const bool tty = stdout_is_tty();
    const bool color = color_enabled(Stream::Out);
    for (const app::ListRow& row : rows) {
        const std::string path = tty ? sanitize_for_terminal(row.path.string()) : row.path.string();
        for (const search::Hit& hit : row.hits) {
            const std::string snippet = color ? highlight_ranges(hit.snippet, hit.ranges)
                                        : tty ? sanitize_for_terminal(hit.snippet)
                                              : hit.snippet;
            std::cout << path << ':' << hit.line << ": " << snippet << '\n';
        }
        if (row.reason)
            std::cout << path << reason_suffix(*row.reason, tty) << '\n';
    }
}

}  // namespace

namespace {

struct FindArgs {
    std::string query;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    bool json = false;
    bool long_format = false;  // ilişki satırının nedeni
};

// Hata iletisini yazar ve çıkış kodunu döner.
std::expected<FindArgs, int> parse_find_args(const std::vector<std::string>& rest) {
    FindArgs parsed;
    bool only_terms = false;  // `--` sonrası her şey terimdir
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (only_terms || arg.size() < 2 || arg[0] != '-') {
            parsed.query += (parsed.query.empty() ? "" : " ") + arg;
        } else if (arg == "--") {
            only_terms = true;
        } else if (arg == "--json") {
            parsed.json = true;
        } else if (arg == "--long" || arg == "-l") {
            parsed.long_format = true;
        } else if (arg == "--limit" || arg == "-n") {
            if (i + 1 >= rest.size())
                return std::unexpected(missing_value_error("find", arg));
            const auto limit = parse_limit(rest[++i]);
            if (!limit)
                return std::unexpected(limit.error());
            parsed.limit = *limit;
        } else {
            return std::unexpected(unknown_option_error("find", arg));
        }
    }
    return parsed;
}

// Aranacak metin gerekir: yalnız argüman (`tag:x`) içerik araması değildir, `note ls` listeler. Bozuk argümanı
// arama hattı bildirir (çıkış 2).
bool has_search_text(std::string_view query) {
    const auto parsed = search::parse_query(query);
    return !query.empty() && (!parsed || !parsed->text.empty());
}

}  // namespace

int run_find(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_find_args(args.rest);
    if (!parsed)
        return parsed.error();
    const auto& [query, limit, json, long_format] = *parsed;
    if (!has_search_text(query))
        return usage_error("find");

    core::NoteStore store = core::NoteStore::for_config(config);
    search::ContentIndex index(store, config.tuning.max_note_bytes.cap(),
                               config.limits.search.content_hits.cap());
    index.refresh();
    app::Listing listing;
    // TUI/GUI ile aynı yol: --limit argüman süzgecinden sonra indekste keser, kesit yalnız tutulanlara üretilir.
    listing.set_content_index([&index]() -> const search::ContentIndex& { return index; });
    listing.set_content_results(limit);
    const auto rows = search_rows(listing, store, config,
                                  {query, app::SearchMode::Content, {}, shows_related(json, long_format), limit});
    if (!rows)
        return rows.error();
    if (json)
        std::cout << find_json(*rows);
    if (rows->empty()) {
        std::cerr << sanitize_for_terminal(core::tr("cli.find_no_match", {{"query", query}})) << '\n';
        return kExitUser;
    }
    if (!json)
        print_find_rows(*rows);
    return kExitOk;
}

// Satır: gösterim adı TAB sayı; --json: [{"name","display","count"}]. Sıra Tag.name.
int run_tags(const ParsedArgs& args, const core::Config& config) {
    const bool json = args.rest.size() == 1 && args.rest.front() == "--json";
    if (!args.rest.empty() && !json) {
        return usage_error("tags");
    }
    const core::NoteStore store = core::NoteStore::for_config(config);
    const std::vector<core::TagCount> tags = store.tags();
    if (!json && !stdout_is_tty()) {
        for (const core::TagCount& tag : tags)
            std::cout << tag.display << '\t' << tag.count << '\n';
        return kExitOk;
    }
    if (!json) {  // terminal: sayılar tek sütunda; boru TSV kalır
        std::size_t width = 0;
        for (const core::TagCount& tag : tags)
            width = std::max(width, code_points(sanitize_for_terminal(tag.display)));
        for (const core::TagCount& tag : tags) {
            const std::string display = sanitize_for_terminal(tag.display);
            std::cout << display << std::string(width - code_points(display) + 2, ' ') << tag.count << '\n';
        }
        return kExitOk;
    }
    std::string out = "[";
    for (const core::TagCount& tag : tags) {
        out += out.size() > 1 ? ",{\"name\":" : "{\"name\":";
        append_json_string(out, tag.name);
        out += ",\"display\":";
        append_json_string(out, tag.display);
        out += ",\"count\":" + std::to_string(tag.count) + "}";
    }
    std::cout << out << "]\n";
    return kExitOk;
}

int run_reserved(const ParsedArgs& args, const core::Config& config) {
    // Fiilli komutta bilinmeyen fiil: komutun kullanım satırı + en yakın fiil. Seçenekle başlayan sözcük
    // (`sync --background`) ve fiilsiz çağrı (`note sync`) komutun kendisine kalır.
    if (const CommandSpec* spec = find_command(args.command);
        spec != nullptr && !spec->verbs.empty() && !args.rest.empty() && !args.rest.front().starts_with('-') &&
        !std::ranges::contains(spec->verbs, args.rest.front(), &VerbSpec::name)) {
        std::vector<std::string_view> verbs;
        for (const VerbSpec& verb : spec->verbs)
            verbs.push_back(verb.name);
        std::cerr << core::tr("cli." + args.command + "_usage") << '\n';
        print_hints(args.command, suggest(args.rest.front(), verbs));
        return kExitUsage;
    }
    if (args.command == "find")
        return run_find(args, config);
    if (args.command == "config")
        return run_config(args, config);
    if (args.command == "rm")
        return run_rm(args, config);
    if (args.command == "mv")
        return run_mv(args, config);
    if (args.command == "ls")
        return run_ls(args, config);
    if (args.command == "show")
        return run_show(args, config);
    if (args.command == "edit")
        return run_edit(args, config);
    if (args.command == "sync")
        return run_sync(args, config);
    if (args.command == "export")
        return run_export(args, config);
    if (args.command == "query")
        return run_query(args, config);
    if (args.command == "graph")
        return run_graph(args, config);
    if (args.command == "tags")
        return run_tags(args, config);
    if (args.command == "view")
        return run_view(args, config);
    if (args.command == "relation")
        return run_relation(args, config);
    if (args.command == "prop")
        return run_prop(args, config);
    if (args.command == "import")
        return run_import(args, config);
    if (args.command == "update")
        return run_update(args, config);
    if (args.command == "completion")
        return run_completion(args);
    if (args.command == kCompleteCommand)
        return run_complete(args, config);
    std::cerr << core::tr("cli.not_implemented", {{"command", args.command}}) << '\n';
    return kExitUsage;
}

int run_invalid(const ParsedArgs& args) {
    const std::string option = args.error_args.empty() ? std::string() : args.error_args.front().second;
    if (args.error_key == "cli.unknown_option")
        return unknown_option_error("", option);
    if (args.error_key == "cli.missing_value")
        return missing_value_error("", option);
    std::cerr << core::tr(args.error_key, args.error_args) << '\n';
    print_hints("", std::nullopt);
    return kExitUsage;
}

}  // namespace noteapp::cli
