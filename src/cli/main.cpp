#include "cli/args.hpp"
#include "cli/command_table.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/config.hpp"
#include "core/console.hpp"
#include "core/dll_search.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/migration.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <cstdio>
#include <new>

int main(int argc, char** argv) {
    using namespace noteapp;
    core::restrict_dll_search_to_system32();
#ifdef _WIN32
    // Metin kipi \n'i \r\n yapar ve stdin'deki 0x1A'yı dosya sonu sayar; not içeriği birebir kalmalı.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const core::ConsoleUtf8Scope utf8_console;

    const core::LoadedConfig loaded = core::load_config();
    core::init_i18n(loaded.config.language);
    core::init_log(loaded.config.tuning.log_max_bytes);
    const cli::ParsedArgs args = cli::parse_args(core::utf8_arguments(argc, argv));
    // Arka plan senkronu ayrık süreçtir ve sessiz kalır (uyarıyı ön plandaki komut zaten gösterdi). Kabuk
    // tamamlaması da sessizdir: stderr her Tab'da istemin ortasına basılırdı.
    const bool reserved = args.kind == cli::ParsedArgs::Kind::Reserved;
    const bool silent =
        reserved && ((args.command == "sync" && !args.rest.empty() && args.rest.front() == "--background") ||
                     args.command == cli::kCompleteCommand);
    for (const auto& warning : loaded.warnings) {
        const std::string message = core::format_error(warning);
        core::log_warn(message);
        // Bozuk config sessizce varsayılana (~/notes) düşmesin: kullanıcı stderr'de görür. stdout/çıkış kodu
        // aynı.
        if (!silent)
            std::fprintf(stderr, "%s\n", cli::sanitize_for_terminal(message).c_str());
    }

    // Geçiş bekleyen eski notlarda liste boş görünür: kullanıcı bunu notların kaybı sanmasın. Yazmayı depo
    // reddeder.
    const bool is_graph_command = args.kind == cli::ParsedArgs::Kind::Reserved && args.command == "graph";
    if (args.kind == cli::ParsedArgs::Kind::Reserved && !silent && !is_graph_command &&
        core::legacy_notes_pending(loaded.config.notes_dir, core::data_dir()))
        std::fprintf(
            stderr, "%s\n",
            cli::sanitize_for_terminal(
                core::tr("error.migration_required", {{"path", core::path_utf8(loaded.config.notes_dir)}}))
                .c_str());

    int code = cli::kExitInternal;
    // Bellek biterse yığın sarmalanır, işlem geri alınır; çıkış terminate değil.
    try {
        switch (args.kind) {
        case cli::ParsedArgs::Kind::Add:
            code = cli::run_add(args, loaded.config);
            break;
        case cli::ParsedArgs::Kind::Help:
            code = cli::run_help(args);
            break;
        case cli::ParsedArgs::Kind::Version:
            code = cli::run_version();
            break;
        case cli::ParsedArgs::Kind::Reserved:
            code = cli::run_reserved(args, loaded.config);
            break;
        case cli::ParsedArgs::Kind::Invalid:
            code = cli::run_invalid(args);
            break;
        }
    } catch (const std::bad_alloc&) {
        std::fputs("note: out of memory\n", stderr);
        code = cli::kExitInternal;
    }
    std::fflush(stdout);
    return code;
}
