#include "app/export.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::cli {

namespace {

namespace fs = std::filesystem;

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

struct ExportArgs {
    bool pdf = false;
    bool force = false;
    std::optional<std::string> all;
    std::optional<std::string> output;
    std::vector<std::string> positional;
};

// Hata mesajını yazar, başarısızsa çıkış kodunu (2) döner.
std::expected<ExportArgs, int> parse_export_args(const std::vector<std::string>& rest) {
    ExportArgs parsed;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (arg == "--pdf") {
            parsed.pdf = true;
        } else if (arg == "--force") {
            parsed.force = true;
        } else if (arg == "--all") {
            if (i + 1 >= rest.size())
                return std::unexpected(missing_value_error("export", arg));
            parsed.all = rest[++i];
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= rest.size())
                return std::unexpected(missing_value_error("export", arg));
            parsed.output = rest[++i];
        } else if (arg.size() > 1 && arg[0] == '-') {
            return std::unexpected(unknown_option_error("export", arg));
        } else {
            parsed.positional.push_back(arg);
        }
    }
    return parsed;
}

}  // namespace

int run_export(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_export_args(args.rest);
    if (!parsed)
        return parsed.error();
    if (parsed->all) {
        if (parsed->pdf || parsed->output || parsed->force || !parsed->positional.empty())
            return usage_error("export");
        return run_export_all(*parsed->all, config);
    }
    if (!parsed->pdf || parsed->positional.size() != 1)
        return usage_error("export");

    core::NoteStore store = core::NoteStore::for_config(config);
    const auto note =
        pick_best_note(store, parsed->positional.front(), config.limits.search.fuzzy_candidate_chars.cap());
    if (!note)
        return note.error();
    // store.read bağ ve kök dışı denetimlerini yapar: PDF yalnız bu baytlardan üretilir, dosya yeniden
    // açılmaz.
    const auto content = store.read(note->path);
    if (!content)
        return fail(content.error());
    // Varsayılan çıktı geçerli dizindedir (not klasörüne değil); `-o` göreli ise yine geçerli dizine göredir.
    const fs::path output =
        fs::absolute(from_utf8(parsed->output.value_or(app::default_export_name(note->path))));

    // TUI ve GUI sorar; CLI soramaz: var olan dosya (kırık bağ dahil) --force olmadan ezilmez.
    if (!parsed->force && app::export_target_exists(output)) {
        std::cerr << sanitize_for_terminal(core::tr("cli.export_exists", {{"path", to_utf8(output)}}))
                  << '\n';
        return kExitUser;
    }
    const auto exported = app::export_note_pdf(content->text, note->path, output);
    if (!exported) {
        const int code = fail(exported.error());
        // İkili not kullanıcı hatasıdır (1), yol hatası (2) değil.
        return exported.error().message_key == app::kExportBinaryKey ? kExitUser : code;
    }
    std::cout << core::tr("cli.exported", {{"path", sanitize_for_terminal(to_utf8(output))}}) << '\n';
    return kExitOk;
}

}  // namespace noteapp::cli
