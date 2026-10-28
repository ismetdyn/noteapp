#include "cli/commands.hpp"

#include "cli/confirm.hpp"
#include "cli/highlight.hpp"
#include "core/editor.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/paths.hpp"
#include "core/version.hpp"
#include "detect/detect.hpp"
#include "search/content_index.hpp"
#include "search/fuzzy.hpp"

#include <algorithm>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace noteapp::cli {

namespace {

#ifdef _WIN32
// _isatty NUL gibi her karakter aygıtını TTY sayar; `note < NUL` editör açıp betiği asılı bırakırdı.
// Gerçek konsol yalnız GetConsoleMode'un başarılı olduğu tutamaçtır.
bool is_console(DWORD std_handle) {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(std_handle), &mode) != 0;
}
#endif

}  // namespace

bool stdin_is_tty() {
#ifdef _WIN32
    return is_console(STD_INPUT_HANDLE);
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

namespace {

bool stdout_is_tty() {
#ifdef _WIN32
    return is_console(STD_OUTPUT_HANDLE);
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
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

int fail(const core::Error& error) {
    std::cerr << core::format_error(error) << '\n';
    return exit_code_for(error);
}

namespace fs = std::filesystem;

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Tip/ad/klasör kuralları hızlı ekleme ile aynıdır. Başarıda tek satır yazdırır, hatada hatayı döner.
std::optional<core::Error> save_note(const ParsedArgs& args, const core::Config& config,
                                     const std::string& content) {
    const bool chosen = args.type.has_value();
    std::string type;
    if (chosen) {
        type = *args.type;
    } else {
        const detect::Options options{config.default_type, config.tuning.min_confidence,
                                      config.tuning.max_scan_bytes};
        type = detect::suggest(content, options).type;
    }

    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    const auto created = store.create(
        core::NewNote{args.folder.value_or(config.default_folder), args.name.value_or(""), type, content});
    if (!created)
        return created.error();

    std::cout << core::tr("cli.saved", {{"path", created->string()},
                                        {"type", type},
                                        {"how", core::tr(chosen ? "cli.how_chosen" : "cli.how_detected")}})
              << '\n';
    start_background_sync(config, args.no_sync);
    return std::nullopt;
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
    if (const auto error = save_note(args, config, content)) {
        const int code = fail(*error);
        report_kept();  // kayıt başarısız: yazılan metin kaybolmasın
        return code;
    }
    fs::remove(*temp, ec);
    return kExitOk;
}

// Alt komut argümanları: konumsal değerler + isteğe bağlı -f KLASÖR. Hata mesajını yazar, çıkış kodunu döner.
struct BrowseArgs {
    std::vector<std::string> positional;
    std::optional<std::string> folder;
};

std::expected<BrowseArgs, int> parse_browse_args(const std::vector<std::string>& rest, bool allow_folder) {
    BrowseArgs parsed;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (allow_folder && arg == "-f") {
            if (i + 1 >= rest.size()) {
                std::cerr << core::tr("cli.missing_value", {{"option", arg}}) << '\n';
                return std::unexpected(kExitUsage);
            }
            parsed.folder = rest[++i];
        } else if (arg.size() > 1 && arg[0] == '-') {
            std::cerr << core::tr("cli.unknown_option", {{"option", arg}}) << '\n';
            return std::unexpected(kExitUsage);
        } else {
            parsed.positional.push_back(arg);
        }
    }
    return parsed;
}

}  // namespace

// "En iyi eşleşme" (SPEC-cli): tam yol varsa o; yoksa rank_notes ilk sonucu ve seçilen yol stderr'e yazılır.
// Eşleşme yoksa mesajı yazar ve 1 döner. Tek kaynak: show/edit/export aynı kuralı kullanır.
std::expected<core::NoteInfo, int> pick_best_note(const core::NoteStore& store, const std::string& query) {
    const std::vector<core::NoteInfo> notes = store.list();
    if (const auto exact = core::NotePath::parse(query)) {
        const auto it = std::ranges::find(notes, *exact, &core::NoteInfo::path);
        if (it != notes.end())
            return *it;
    }
    const auto ranked = search::rank_notes(query, notes);
    if (ranked.empty()) {
        std::cerr << core::tr("cli.no_match", {{"query", query}}) << '\n';
        return std::unexpected(kExitUser);
    }
    const core::NoteInfo& best = notes[ranked.front().index];
    std::cerr << core::tr("cli.chosen", {{"path", best.path.string()}}) << '\n';
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
    if (const auto error = save_note(args, config, content))
        return fail(*error);
    return kExitOk;
}

int run_help(const ParsedArgs& args) {
    if (args.command.empty() || args.command == "help") {
        std::cout << core::tr("cli.help");
        return kExitOk;
    }
    // Komut başına metin katalogda `cli.help_<komut>` anahtarıdır; yalnız bilinen komut adları aranır.
    if (!is_reserved_command(args.command)) {
        std::cerr << core::tr("cli.unknown_command", {{"command", args.command}}) << '\n';
        return kExitUsage;
    }
    std::cout << core::tr("cli.help_" + args.command) << '\n';
    return kExitOk;
}

namespace {

// Token kasada (T27'de `note sync login`) tutulur; config'e yazılmaz/okunmaz (SPEC-cli kabul 8).
bool is_token_key(std::string_view key) {
    return key == "sync.token";
}

}  // namespace

std::optional<core::Config> load_config_for_update() {
    core::LoadedConfig fresh = core::load_config();
    for (const core::Error& warning : fresh.warnings) {
        if (warning.message_key == "config.syntax_error" || warning.message_key == "config.unreadable") {
            std::cerr << core::format_error(warning) << '\n' << core::tr("cli.config_not_saved") << '\n';
            return std::nullopt;
        }
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
        for (const auto& [key, value] : core::config_entries(config))
            std::cout << key << " = " << value << '\n';
        return kExitOk;
    }
    if ((verb == "get" && operands == 1) || (verb == "set" && operands == 2)) {
        const std::string& key = rest[1];
        if (is_token_key(key)) {
            std::cerr << core::tr("cli.token_not_config") << '\n';
            return kExitUsage;
        }
        const auto entries = core::config_entries(config);
        const auto entry = std::ranges::find(entries, key, &std::pair<std::string, std::string>::first);
        if (entry == entries.end()) {
            std::cerr << core::tr("cli.config_unknown_key", {{"key", key}}) << '\n';
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
        for (const auto& [saved_key, saved_value] : core::config_entries(*fresh))
            if (saved_key == key)
                std::cout << saved_key << " = " << saved_value << '\n';
        return kExitOk;
    }
    std::cerr << core::tr("cli.config_usage") << '\n';
    return kExitUsage;
}

int run_version() {
    std::cout << "note " << core::version() << '\n';
    return kExitOk;
}

int run_rm(const ParsedArgs& args, const core::Config& config) {
    bool yes = false;
    bool no_sync = false;
    std::vector<std::string> paths;
    for (const std::string& arg : take_no_sync(args.rest, no_sync)) {
        if (arg == "-y") {
            yes = true;
        } else if (arg.size() > 1 && arg[0] == '-') {
            std::cerr << core::tr("cli.unknown_option", {{"option", arg}}) << '\n';
            return kExitUsage;
        } else {
            paths.push_back(arg);
        }
    }
    if (paths.size() != 1) {
        std::cerr << core::tr("cli.rm_usage") << '\n';
        return kExitUsage;
    }

    // Yalnız tam yol: fuzzy eşleşme yok. Silmeden önce yolun var olduğu doğrulanır (onay boşuna sorulmasın).
    const auto path = core::NotePath::parse(paths.front());
    if (!path)
        return fail(path.error());
    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    if (const auto found = store.read(*path); !found)
        return fail(found.error());

    if (!yes) {
        if (!stdin_is_tty()) {
            std::cerr << core::tr("cli.rm_needs_yes", {{"path", path->string()}}) << '\n';
            return kExitUser;
        }
        std::cerr << core::tr("cli.confirm_delete", {{"path", path->string()}}) << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) || !is_delete_confirmed(answer)) {
            std::cerr << core::tr("cli.delete_not_confirmed") << '\n';
            return kExitUser;
        }
    }
    if (const auto removed = store.remove(*path); !removed)
        return fail(removed.error());
    std::cout << core::tr("cli.removed", {{"path", path->string()}}) << '\n';
    start_background_sync(config, no_sync);
    return kExitOk;
}

int run_mv(const ParsedArgs& args, const core::Config& config) {
    bool no_sync = false;
    const std::vector<std::string> rest = take_no_sync(args.rest, no_sync);
    for (const std::string& arg : rest) {
        if (arg.size() > 1 && arg[0] == '-') {
            std::cerr << core::tr("cli.unknown_option", {{"option", arg}}) << '\n';
            return kExitUsage;
        }
    }
    if (rest.size() != 2) {
        std::cerr << core::tr("cli.mv_usage") << '\n';
        return kExitUsage;
    }
    const auto from = core::NotePath::parse(rest[0]);
    if (!from)
        return fail(from.error());
    const auto to = core::NotePath::parse(rest[1]);
    if (!to)
        return fail(to.error());

    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    if (const auto moved = store.move(*from, *to); !moved)
        return fail(moved.error());
    std::cout << core::tr("cli.moved", {{"from", from->string()}, {"to", to->string()}}) << '\n';
    start_background_sync(config, no_sync);
    return kExitOk;
}

int run_ls(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_browse_args(args.rest, true);
    if (!parsed)
        return parsed.error();
    if (parsed->positional.size() > 1) {
        std::cerr << core::tr("cli.ls_usage") << '\n';
        return kExitUsage;
    }
    std::string prefix;
    if (parsed->folder) {
        const auto folder = core::NotePath::parse(*parsed->folder);
        if (!folder)
            return fail(folder.error());
        prefix = folder->string() + "/";  // "work/" eşleşir, "workshop.md" eşleşmez
    }

    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    std::vector<core::NoteInfo> notes = store.list();
    std::erase_if(notes, [&](const core::NoteInfo& n) { return !n.path.string().starts_with(prefix); });
    // Boş sorguda rank_notes saf mtime azalan sıradır; ayrı bir yol gerekmez.
    const std::string query = parsed->positional.empty() ? "" : parsed->positional.front();
    for (const search::Ranked& r : search::rank_notes(query, notes))
        std::cout << notes[r.index].path.string() << '\n';
    return kExitOk;
}

int run_show(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_browse_args(args.rest, false);
    if (!parsed)
        return parsed.error();
    if (parsed->positional.size() != 1) {
        std::cerr << core::tr("cli.show_usage") << '\n';
        return kExitUsage;
    }
    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    const auto note = pick_best_note(store, parsed->positional.front());
    if (!note)
        return note.error();
    const auto content = store.read(note->path);
    if (!content)
        return fail(content.error());
    if (core::looks_binary(content->text)) {
        std::cerr << core::tr("cli.binary_note", {{"path", note->path.string()}}) << '\n';
        return kExitUser;
    }
    std::cout.write(content->text.data(), static_cast<std::streamsize>(content->text.size()));
    return kExitOk;
}

int run_edit(const ParsedArgs& args, const core::Config& config) {
    bool no_sync = false;
    const auto parsed = parse_browse_args(take_no_sync(args.rest, no_sync), false);
    if (!parsed)
        return parsed.error();
    if (parsed->positional.size() != 1) {
        std::cerr << core::tr("cli.edit_usage") << '\n';
        return kExitUsage;
    }
    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    const auto note = pick_best_note(store, parsed->positional.front());
    if (!note)
        return note.error();
    const fs::path file = config.notes_dir / note->path.value();
    std::error_code ec;
    const auto before = fs::last_write_time(file, ec);
    if (const auto edited = core::run_editor(config, file); !edited)
        return fail(edited.error());
    // Kaydetmeden kapatılan editör için ağa çıkılmaz.
    if (const auto after = fs::last_write_time(file, ec); ec || after != before)
        start_background_sync(config, no_sync);
    return kExitOk;
}

int run_find(const ParsedArgs& args, const core::Config& config) {
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    std::string query;
    bool only_terms = false;  // `--` sonrası her şey terimdir
    for (std::size_t i = 0; i < args.rest.size(); ++i) {
        const std::string& arg = args.rest[i];
        if (only_terms || arg.size() < 2 || arg[0] != '-') {
            query += (query.empty() ? "" : " ") + arg;
        } else if (arg == "--") {
            only_terms = true;
        } else if (arg == "--limit") {
            if (i + 1 >= args.rest.size()) {
                std::cerr << core::tr("cli.missing_value", {{"option", arg}}) << '\n';
                return kExitUsage;
            }
            const std::string& value = args.rest[++i];
            std::size_t used = 0;
            unsigned long long parsed = 0;
            try {
                parsed = std::stoull(value, &used);
            } catch (const std::exception&) {  // sayı değil / taşma: kullanım hatası
                used = 0;
            }
            if (used != value.size() || value.empty() || value[0] == '-' || parsed == 0) {
                std::cerr << core::tr("cli.invalid_limit", {{"value", value}}) << '\n';
                return kExitUsage;
            }
            limit = static_cast<std::size_t>(parsed);
        } else {
            std::cerr << core::tr("cli.unknown_option", {{"option", arg}}) << '\n';
            return kExitUsage;
        }
    }
    if (query.empty()) {
        std::cerr << core::tr("cli.find_usage") << '\n';
        return kExitUsage;
    }

    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    search::ContentIndex index(store, config.tuning.max_note_bytes);
    index.refresh();
    const auto results = index.search(query, limit);
    if (results.empty()) {
        std::cerr << core::tr("cli.find_no_match", {{"query", query}}) << '\n';
        return kExitUser;
    }

    const bool color = stdout_is_tty();  // boruya/dosyaya kaçış kodu yazılmaz
    for (const search::ContentResult& result : results) {
        for (const search::Hit& hit : result.first_hits) {
            std::cout << result.path.string() << ':' << hit.line << ": "
                      << (color ? highlight_ranges(hit.snippet, hit.ranges) : hit.snippet) << '\n';
        }
    }
    return kExitOk;
}

int run_reserved(const ParsedArgs& args, const core::Config& config) {
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
    std::cerr << core::tr("cli.not_implemented", {{"command", args.command}}) << '\n';
    return kExitUsage;
}

int run_invalid(const ParsedArgs& args) {
    std::cerr << core::tr(args.error_key, args.error_args) << '\n';
    return kExitUsage;
}

}  // namespace noteapp::cli
