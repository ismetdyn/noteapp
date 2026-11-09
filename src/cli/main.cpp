#include "app/update_worker.hpp"
#include "cli/args.hpp"
#include "cli/command_table.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "cli/pager.hpp"
#include "core/config.hpp"
#include "core/console.hpp"
#include "core/dll_search.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_store_impl.hpp"
#include "core/paths.hpp"
#include "pdfview/engine/worker.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <cstdio>
#include <new>
#include <string_view>

int main(int argc, char** argv) {
    using namespace noteapp;
    core::restrict_dll_search_to_system32();
#ifdef _WIN32
    // Metin kipi \n'i \r\n yapar ve stdin'deki 0x1A'yı dosya sonu sayar; not içeriği birebir kalmalı.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    // PDF işçisi (pdfview::PdfDocument başlatır): ikili stdio, config/log/argüman ayrıştırma yok. Gizli alt komut;
    // yardımda ve tamamlamada görünmez.
    if (argc == 2 && std::string_view(argv[1]) == "__pdf-worker")
        return pdfview::run_pdf_worker();
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

    // Önceki güncellemenin artıkları (Windows *.old, bayat geçici dizinler). Kabuk tamamlaması her Tab'da
    // çalışır: kurulum dizinine dokunmaz.
    if (!(reserved && args.command == cli::kCompleteCommand))
        app::cleanup_previous_update();

    int code = cli::kExitInternal;
    // Bellek biterse yığın sarmalanır, işlem geri alınır; çıkış terminate değil.
    try {
        const auto dispatch = [&]() -> int {
            switch (args.kind) {
            case cli::ParsedArgs::Kind::Add:
                return cli::run_add(args, loaded.config);
            case cli::ParsedArgs::Kind::Help:
                return cli::run_help(args);
            case cli::ParsedArgs::Kind::Version:
                return cli::run_version();
            case cli::ParsedArgs::Kind::Reserved:
                return cli::run_reserved(args, loaded.config);
            case cli::ParsedArgs::Kind::Invalid:
                return cli::run_invalid(args);
            }
            return cli::kExitInternal;
        };
        code = args.paged ? cli::run_paged(dispatch) : dispatch();
    } catch (const std::bad_alloc&) {
        std::fputs("note: out of memory\n", stderr);
        code = cli::kExitInternal;
    }
    std::fflush(stdout);
    return code;
}
