#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "core/config.hpp"
#include "core/console.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <cstdio>

int main(int argc, char** argv) {
    using namespace noteapp;
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
    for (const auto& warning : loaded.warnings)
        core::log_warn(core::format_error(warning));

    const cli::ParsedArgs args = cli::parse_args(cli::utf8_arguments(argc, argv));
    int code = cli::kExitInternal;
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
    std::fflush(stdout);
    return code;
}
