#include "cli/commands.hpp"

#include "core/atomic_file.hpp"
#include "core/editor.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/version.hpp"
#include "detect/detect.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
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

namespace fs = std::filesystem;

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Editör için geçici dosya: OS temp dizininde noteapp-<rastgele>.<tip>; yalnız yoksa oluşturulur.
std::expected<fs::path, core::Error> create_temp_file(const std::string& type, const std::string& initial) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    if (ec)
        return std::unexpected(
            core::Error{core::Error::Code::Io, "error.io", {{"path", "(temp)"}, {"detail", ec.message()}}});
    std::random_device random;
    constexpr int kMaxAttempts = 100;  // çakışma döngüsünün sonsuza gitmesine karşı sigorta
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const fs::path file = dir / ("noteapp-" + std::to_string(random()) + "." + type);
        const core::FileWriteResult written = core::write_new_file(file, initial);
        if (written.already_exists)
            continue;
        if (written.ec) {
            fs::remove(file, ec);
            return std::unexpected(core::Error{core::Error::Code::Io,
                                               "error.io",
                                               {{"path", to_utf8(file)}, {"detail", written.ec.message()}}});
        }
        return file;
    }
    return std::unexpected(core::Error{
        core::Error::Code::Io, "error.io", {{"path", to_utf8(dir)}, {"detail", "no free temp file name"}}});
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

    // --no-sync: T27'de arka plan senkronu bağlanınca anlam kazanır; şimdilik kabul edilir, etkisizdir.
    std::cout << core::tr("cli.saved", {{"path", created->string()},
                                        {"type", type},
                                        {"how", core::tr(chosen ? "cli.how_chosen" : "cli.how_detected")}})
              << '\n';
    return std::nullopt;
}

// Editör akışı (SPEC-cli "Hızlı ekleme kuralları"): geçici dosya yalnız veri kaybı riski yoksa silinir.
int run_editor_flow(const ParsedArgs& args, const core::Config& config, const std::string& initial) {
    const auto temp = create_temp_file(config.default_type, initial);
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

}  // namespace

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
