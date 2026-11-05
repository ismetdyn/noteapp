#include "app/import_files.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/i18n.hpp"
#include "core/json_text.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::cli {

namespace {

namespace fs = std::filesystem;

int usage() {
    return usage_error("import");
}

void print_json(const app::ImportReport& report) {
    std::string out = "{\"imported\":[";
    for (const auto& item : report.imported) {
        out += out.ends_with('[') ? "{\"source\":" : ",{\"source\":";
        core::append_json_string(out, item.source);
        out += ",\"note\":";
        core::append_json_string(out, item.note.string());
        out += std::string(",\"renamed\":") + (item.renamed ? "true" : "false") +
               ",\"overwritten\":" + (item.overwritten ? "true" : "false") + "}";
    }
    out += "],\"skipped\":[";
    for (const auto& item : report.skipped) {
        out += out.ends_with('[') ? "{\"source\":" : ",{\"source\":";
        core::append_json_string(out, item.source);
        out += ",\"reason\":";
        core::append_json_string(out, item.reason);
        out += "}";
    }
    out += "],\"failed\":[";
    for (const auto& item : report.failed) {
        out += out.ends_with('[') ? "{\"source\":" : ",{\"source\":";
        core::append_json_string(out, item.source);
        out += ",\"error\":";
        core::append_json_string(out, core::format_error(item.error));
        out += "}";
    }
    out += "],\"hidden\":" + std::to_string(report.hidden) + "}\n";
    std::cout << out;
}

// Yollar dosya sisteminden gelir, notlar başka cihazdan gelmiş olabilir: terminale temizlenerek yazılır.
// quiet: yalnız sorun satırları (stderr) yazılır.
void print_text(const app::ImportReport& report, bool quiet) {
    for (const app::ImportLine& line : app::import_report_lines(report)) {
        if (line.problem)
            std::cerr << sanitize_for_terminal(line.text) << '\n';
        else if (!quiet)
            std::cout << sanitize_for_terminal(line.text) << '\n';
    }
}

}  // namespace

int run_import(const ParsedArgs& args, const core::Config& config) {
    app::ImportOptions options = app::import_options(config, config.default_folder);
    bool json = false;
    bool quiet = false;
    bool no_sync = false;
    bool only_paths = false;  // `--` sonrası her şey yoldur
    std::optional<std::string> type;
    std::vector<fs::path> sources;
    const std::vector<std::string>& rest = args.rest;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (only_paths || arg.size() < 2 || arg[0] != '-') {
            sources.emplace_back(std::u8string(arg.begin(), arg.end()));
        } else if (arg == "--") {
            only_paths = true;
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--no-sync") {
            no_sync = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg == "--folder" || arg == "-f" || arg == "--type" || arg == "-t" || arg == "--name" ||
                   arg == "-n") {
            if (i + 1 >= rest.size())
                return missing_value_error("import", arg);
            const std::string& value = rest[++i];
            if (arg == "--folder" || arg == "-f")
                options.folder = value;
            else if (arg == "--type" || arg == "-t")
                type = value;
            else
                options.name = value;
        } else {
            return unknown_option_error("import", arg);
        }
    }
    if (sources.empty() || (!options.name.empty() && sources.size() != 1))
        return usage();
    if (!options.folder.empty()) {
        const auto folder = core::NotePath::parse(options.folder);
        if (!folder)
            return fail(folder.error());
        options.folder = folder->string();
    }
    if (type) {
        // Kural depoyla aynı (validate_new_note); depo her dosyada öğe hatası verirdi (çıkış 1). Burada
        // kullanım hatasıdır: çıkış 2, hiçbir şey yazılmaz.
        if (type->empty() || type->find_first_of("./\\") != std::string::npos ||
            !core::NotePath::parse("x." + *type))
            return fail({core::Error::Code::InvalidPath,
                         "error.invalid_path",
                         {{"reason", "invalid_type"}, {"path", *type}}});
        options.type = *type;
    }

    core::NoteStore store = core::NoteStore::for_config(config);
    if (const auto health = store.health())
        return fail(*health);
    // ponytail: tek "içe alınıyor" satırı; n/m sayacı app::import_paths'e ilerleme geri çağrısı ister.
    const app::ImportReport report = [&] {
        const ProgressLine progress(core::tr("cli.import_progress"));
        return app::import_paths(store, sources, options);
    }();
    if (json)
        print_json(report);
    else
        print_text(report, quiet);
    if (report.wrote)
        start_background_sync(config, no_sync);
    return report.complete() ? kExitOk : kExitUser;
}

}  // namespace noteapp::cli
