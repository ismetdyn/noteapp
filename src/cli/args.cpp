#include "cli/args.hpp"

#include "cli/command_table.hpp"

#include <algorithm>

namespace noteapp::cli {

namespace {

ParsedArgs invalid(std::string key, std::vector<std::pair<std::string, std::string>> args) {
    ParsedArgs parsed;
    parsed.kind = ParsedArgs::Kind::Invalid;
    parsed.error_key = std::move(key);
    parsed.error_args = std::move(args);
    return parsed;
}

}  // namespace

bool is_reserved_command(std::string_view word) {
    return word == kCompleteCommand || std::ranges::contains(kCommands, word, &CommandSpec::name);
}

ParsedArgs parse_args(const std::vector<std::string>& args) {
    ParsedArgs parsed;

    // Alt komut yalnız ilk argüman olabilir; `note -t md ls` "ls" metnini ekler.
    if (!args.empty() && is_reserved_command(args[0])) {
        if (args[0] == "help") {
            parsed.kind = ParsedArgs::Kind::Help;
            if (args.size() > 1)
                parsed.command = args[1];  // `note help KOMUT`
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
        } else if (arg == "-e") {
            parsed.editor = true;
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

}  // namespace noteapp::cli
