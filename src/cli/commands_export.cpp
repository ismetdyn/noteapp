#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "pdf/pdf_export.hpp"

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
        } else if (arg == "--all") {
            if (i + 1 >= rest.size()) {
                std::cerr << core::tr("cli.missing_value", {{"option", arg}}) << '\n';
                return std::unexpected(kExitUsage);
            }
            parsed.all = rest[++i];
        } else if (arg == "-o") {
            if (i + 1 >= rest.size()) {
                std::cerr << core::tr("cli.missing_value", {{"option", arg}}) << '\n';
                return std::unexpected(kExitUsage);
            }
            parsed.output = rest[++i];
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

int run_export(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_export_args(args.rest);
    if (!parsed)
        return parsed.error();
    if (parsed->all) {
        if (parsed->pdf || parsed->output || !parsed->positional.empty()) {
            std::cerr << core::tr("cli.export_usage") << '\n';
            return kExitUsage;
        }
        return run_export_all(*parsed->all, config);
    }
    if (!parsed->pdf || parsed->positional.size() != 1) {
        std::cerr << core::tr("cli.export_usage") << '\n';
        return kExitUsage;
    }

    core::NoteStore store = core::NoteStore::for_config(config);
    const auto note = pick_best_note(store, parsed->positional.front());
    if (!note)
        return note.error();
    // store.read bağ ve kök dışı denetimlerini yapar: PDF yalnız bu baytlardan üretilir, dosya yeniden
    // açılmaz.
    const auto content = store.read(note->path);
    if (!content)
        return fail(content.error());
    if (core::looks_binary(content->text)) {
        std::cerr << sanitize_for_terminal(core::tr("cli.binary_note", {{"path", note->path.string()}}))
                  << '\n';
        return kExitUser;
    }
    // Varsayılan çıktı geçerli dizindedir (not klasörüne değil); `-o` göreli ise yine geçerli dizine göredir.
    fs::path default_name = note->path.value().stem();
    default_name += ".pdf";  // replace_extension "a.b.md" -> "a.pdf" yapardı
    const fs::path output = fs::absolute(parsed->output ? from_utf8(*parsed->output) : default_name);

    const auto exported = pdf::export_pdf(content->text, pdf::format_for(note->path.value()), output);
    if (!exported)
        return fail(exported.error());
    std::cout << core::tr("cli.exported", {{"path", sanitize_for_terminal(to_utf8(output))}}) << '\n';
    return kExitOk;
}

}  // namespace noteapp::cli
