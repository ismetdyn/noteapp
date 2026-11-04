#include "app/ui_args.hpp"

namespace noteapp::app {

namespace {

UiArgs invalid(std::string key, std::vector<std::pair<std::string, std::string>> args) {
    UiArgs parsed;
    parsed.kind = UiArgs::Kind::Invalid;
    parsed.error_key = std::move(key);
    parsed.error_args = std::move(args);
    return parsed;
}

// `import` sonrası: seçenekler ve yollar her sırada; `--` sonrası her şey yoldur.
UiArgs parse_import(std::span<const std::string> rest) {
    UiArgs parsed;
    parsed.kind = UiArgs::Kind::Import;
    bool only_paths = false;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (only_paths || arg.size() < 2 || arg[0] != '-') {
            parsed.sources.emplace_back(std::u8string(arg.begin(), arg.end()));
        } else if (arg == "--") {
            only_paths = true;
        } else if (arg == "--overwrite") {
            parsed.overwrite = true;
        } else if (arg == "--folder" || arg == "-f" || arg == "--name" || arg == "-n") {
            if (i + 1 >= rest.size())
                return invalid("cli.missing_value", {{"option", arg}});
            const std::string& value = rest[++i];
            if (arg == "--folder" || arg == "-f")
                parsed.folder = value;
            else
                parsed.name = value;
        } else {
            return invalid("cli.unknown_option", {{"option", arg}});
        }
    }
    if (parsed.sources.empty() || (!parsed.name.empty() && parsed.sources.size() != 1))
        return invalid({}, {});
    return parsed;
}

}  // namespace

UiArgs parse_ui_args(std::span<const std::string> args, bool allow_import) {
    if (args.empty())
        return {};
    const std::string& first = args.front();
    if (first == "--help" || first == "-h")
        return {.kind = UiArgs::Kind::Help};
    if (first == "import" && allow_import)
        return parse_import(args.subspan(1));
    if (first.size() > 1 && first[0] == '-')
        return invalid("cli.unknown_option", {{"option", first}});
    return invalid("cli.unknown_command", {{"command", first}});
}

}  // namespace noteapp::app
