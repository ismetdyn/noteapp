#include "cli/commands.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/process.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::cli {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* kGuiName = "notes-gui.exe";
#else
constexpr const char* kGuiName = "notes-gui";
#endif

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

// `notes-gui` `note`'un yanındadır (link bağımlılığı yok). NOTEAPP_GUI_EXE yalnız TEST DİKİŞİDİR:
// GUI'nin kurulu olmadığı durumu (SPEC-cli kabul 10) ikilileri kopyalamadan simüle eder.
fs::path locate_gui() {
    if (const auto override_path = core::get_env_utf8("NOTEAPP_GUI_EXE"))
        return from_utf8(*override_path);
    return core::current_executable().parent_path() / kGuiName;
}

struct ExportArgs {
    bool pdf = false;
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
    if (!parsed->pdf || parsed->positional.size() != 1) {
        std::cerr << core::tr("cli.export_usage") << '\n';
        return kExitUsage;
    }

    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    const auto note = pick_best_note(store, parsed->positional.front());
    if (!note)
        return note.error();
    // Okunamayan not burada elenmez: hatayı dışa aktaran süreç bildirir (önceki davranış).
    if (const auto content = store.read(note->path); content && core::looks_binary(content->text)) {
        std::cerr << core::tr("cli.binary_note", {{"path", note->path.string()}}) << '\n';
        return kExitUser;
    }

    const fs::path gui = locate_gui();
    std::error_code ec;
    if (!fs::exists(gui, ec)) {
        std::cerr << core::tr("cli.gui_not_installed") << '\n';
        return kExitInternal;
    }

    const fs::path input = fs::absolute(config.notes_dir / note->path.value());
    // Varsayılan çıktı geçerli dizindedir (not klasörüne değil); `-o` göreli ise yine geçerli dizine göredir.
    fs::path default_name = note->path.value().stem();
    default_name += ".pdf";  // replace_extension "a.b.md" -> "a.pdf" yapardı
    const fs::path output = fs::absolute(parsed->output ? from_utf8(*parsed->output) : default_name);

    // Çocuğun stderr'i devralınır: yerelleştirilmiş hata satırı doğrudan kullanıcıya ulaşır.
    const auto code = core::run_and_wait({to_utf8(gui), "--export-pdf", to_utf8(input), to_utf8(output)},
                                         {{"QT_QPA_PLATFORM", "offscreen"}});
    if (!code) {
        std::cerr << core::format_error(code.error()) << '\n';
        return kExitInternal;
    }
    if (*code != 0)
        return kExitInternal;
    std::cout << core::tr("cli.exported", {{"path", to_utf8(output)}}) << '\n';
    return kExitOk;
}

}  // namespace noteapp::cli
