#include "cli/args.hpp"

#include <algorithm>
#include <array>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <shellapi.h>  // CommandLineToArgvW; windows.h'tan sonra gelmeli
// clang-format on
#endif

namespace noteapp::cli {

namespace {

constexpr std::array<std::string_view, 9> kReserved = {"ls", "find",   "show", "edit",  "rm",
                                                       "mv", "export", "sync", "config"};

ParsedArgs invalid(std::string key, std::vector<std::pair<std::string, std::string>> args) {
    ParsedArgs parsed;
    parsed.kind = ParsedArgs::Kind::Invalid;
    parsed.error_key = std::move(key);
    parsed.error_args = std::move(args);
    return parsed;
}

}  // namespace

bool is_reserved_command(std::string_view word) {
    return word == "help" || std::ranges::find(kReserved, word) != kReserved.end();
}

ParsedArgs parse_args(const std::vector<std::string>& args) {
    ParsedArgs parsed;

    // Alt komut yalnız ilk argüman olabilir; `note -t md ls` "ls" metnini ekler.
    if (!args.empty() && is_reserved_command(args[0])) {
        if (args[0] == "help") {
            parsed.kind = ParsedArgs::Kind::Help;
            return parsed;
        }
        parsed.kind = ParsedArgs::Kind::Reserved;
        parsed.command = args[0];
        parsed.rest.assign(args.begin() + 1, args.end());
        return parsed;
    }

    bool only_text = false;  // `--` sonrası her şey metindir
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (only_text || arg.empty() || arg[0] != '-' || arg == "-") {
            parsed.text.push_back(arg);
            continue;
        }
        if (arg == "--") {
            only_text = true;
        } else if (arg == "--help" || arg == "-h") {
            parsed.kind = ParsedArgs::Kind::Help;
            return parsed;
        } else if (arg == "--version") {
            parsed.kind = ParsedArgs::Kind::Version;
            return parsed;
        } else if (arg == "--no-sync") {
            parsed.no_sync = true;
        } else if (arg == "-t" || arg == "-f" || arg == "-n") {
            if (i + 1 >= args.size())
                return invalid("cli.missing_value", {{"option", arg}});
            auto& slot = arg == "-t" ? parsed.type : arg == "-f" ? parsed.folder : parsed.name;
            slot = args[++i];
        } else {
            return invalid("cli.unknown_option", {{"option", arg}});
        }
    }
    return parsed;
}

std::vector<std::string> utf8_arguments(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::vector<std::string> out;
    int wide_argc = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide == nullptr)
        return out;
    for (int i = 1; i < wide_argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(size > 0 ? static_cast<std::size_t>(size - 1) : 0, '\0');
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), size, nullptr, nullptr);
        out.push_back(std::move(utf8));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

}  // namespace noteapp::cli
