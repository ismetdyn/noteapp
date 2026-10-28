#include "cli/commands.hpp"

#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/version.hpp"
#include "detect/detect.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace noteapp::cli {

namespace {

bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
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

}  // namespace

int run_add(const ParsedArgs& args, const core::Config& config) {
    std::string content;
    if (!args.text.empty()) {
        for (std::size_t i = 0; i < args.text.size(); ++i)
            content += (i == 0 ? "" : " ") + args.text[i];
    } else if (!stdin_is_tty()) {
        content = read_all_stdin();
    } else {
        // T13: editör akışı
        std::cerr << core::tr("cli.help");
        return kExitUsage;
    }
    if (is_blank(content)) {
        std::cerr << core::tr("cli.empty_input") << '\n';
        return kExitUser;
    }

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
        return fail(created.error());

    // --no-sync: T27'de arka plan senkronu bağlanınca anlam kazanır; şimdilik kabul edilir, etkisizdir.
    std::cout << core::tr("cli.saved", {{"path", created->string()},
                                        {"type", type},
                                        {"how", core::tr(chosen ? "cli.how_chosen" : "cli.how_detected")}})
              << '\n';
    return kExitOk;
}

int run_help() {
    std::cout << core::tr("cli.help");
    return kExitOk;
}

int run_version() {
    std::cout << "note " << core::version() << '\n';
    return kExitOk;
}

int run_reserved(const ParsedArgs& args) {
    std::cerr << core::tr("cli.not_implemented", {{"command", args.command}}) << '\n';
    return kExitUsage;
}

int run_invalid(const ParsedArgs& args) {
    std::cerr << core::tr(args.error_key, args.error_args) << '\n';
    return kExitUsage;
}

}  // namespace noteapp::cli
