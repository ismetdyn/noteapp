#include "cli/args.hpp"

#include "cli/command_table.hpp"

#include <algorithm>
#include <span>

namespace noteapp::cli {

namespace {

ParsedArgs invalid(std::string key, std::vector<std::pair<std::string, std::string>> args) {
    ParsedArgs parsed;
    parsed.kind = ParsedArgs::Kind::Invalid;
    parsed.error_key = std::move(key);
    parsed.error_args = std::move(args);
    return parsed;
}

bool is_help_flag(std::string_view word) {
    return word == "--help" || word == "-h";
}

bool takes_value(std::string_view name, std::span<const OptionSpec> options) {
    return std::ranges::any_of(options, [&](const OptionSpec& option) {
        return option.value != ValueKind::None && (option.name == name || option.alias == name);
    });
}

// `-h`/`--help` alt komutta yardımdır, öbür argümanlar yok sayılır (clig.dev). Değer olduğu yerde değer kalır
// (eski davranış): `--` sonrası, değer alan seçeneğin ardı (`ls -f -h`), konumlu verisi olan fiilin argümanları
// (`config set editor -h`, `view save x -h`). Fiilli komutta yardım ya hemen komuttan sonra ya da verisiz fiilden
// (`view ls --help`) sonra gelir.
bool asks_for_help(const CommandSpec& command, std::span<const std::string> rest) {
    std::span<const OptionSpec> options = command.options;
    std::size_t start = 0;
    if (!command.verbs.empty() && !rest.empty() && !is_help_flag(rest.front())) {
        const auto verb = std::ranges::find(command.verbs, std::string_view(rest.front()), &VerbSpec::name);
        if (verb == command.verbs.end() || verb->operand != ValueKind::None)
            return false;
        options = verb->options;
        start = 1;
    }
    for (std::size_t i = start; i < rest.size(); ++i) {
        if (rest[i] == "--")
            return false;
        if (is_help_flag(rest[i]))
            return true;
        if (takes_value(rest[i], options))
            ++i;  // seçeneğin değeri, bayrak değil
    }
    return false;
}

bool command_takes_value(const CommandSpec& command, std::string_view name) {
    return takes_value(name, command.options) ||
           std::ranges::any_of(command.verbs, [&](const VerbSpec& verb) { return takes_value(name, verb.options); });
}

// `--ad=DEĞER` iki sözcüğe bölünür, yalnız ad tabloda değer alan bir uzun seçenekse; `--` sonrası ve başka her
// sözcük olduğu gibi kalır (`note "a=b"` metindir, `--no-sync=x` bilinmeyen seçenektir).
template <class TakesValue>
std::vector<std::string> split_inline_values(std::span<const std::string> words, TakesValue takes) {
    std::vector<std::string> out;
    bool only_text = false;
    bool is_value = false;  // önceki sözcük değer alan seçenekti: bu sözcük onun değeridir (`-n --type=x`)
    for (const std::string& word : words) {
        if (is_value) {
            out.push_back(word);
            is_value = false;
            continue;
        }
        is_value = !only_text && word.starts_with('-') && takes(word);
        const std::size_t equals = word.find('=');
        if (!only_text && word.starts_with("--") && equals != std::string::npos &&
            takes(std::string_view(word).substr(0, equals))) {
            out.push_back(word.substr(0, equals));
            out.push_back(word.substr(equals + 1));
            continue;
        }
        only_text = only_text || word == "--";
        out.push_back(word);
    }
    return out;
}

constexpr std::string_view kNoPagerFlag = "--no-pager";

// `--` öncesindeki `flag` sözcüklerini çıkarır; biri varsa `found` true olur.
std::vector<std::string> take_flag(std::vector<std::string> words, std::string_view flag, bool& found) {
    const auto end = std::ranges::find(words, "--");
    const auto removed = std::remove(words.begin(), end, flag);
    found = removed != end;
    words.erase(removed, end);
    return words;
}

// Komut (fiilliyse ilk sözcüğün fiili) tabloda `--no-pager` taşıyorsa çıktısı sayfalanabilir.
bool is_pageable(const CommandSpec& command, const std::vector<std::string>& rest) {
    const auto has_flag = [](std::span<const OptionSpec> options) {
        return std::ranges::contains(options, kNoPagerFlag, &OptionSpec::name);
    };
    if (command.verbs.empty())
        return has_flag(command.options);
    const auto verb = std::ranges::find(command.verbs, rest.empty() ? "" : std::string_view(rest.front()),
                                        &VerbSpec::name);
    return verb != command.verbs.end() && has_flag(verb->options);
}

// `args[0]` ayrılmış sözcük: sonrası komut adıyla yazılmış gibi ayrışır (`-l` = `--ls` = `ls`, `cat` = `show`).
ParsedArgs parse_command(const std::vector<std::string>& args) {
    ParsedArgs parsed;
    const std::span<const std::string> rest(args.begin() + 1, args.end());
    const CommandSpec* command = find_command(args[0]);
    const std::string command_name = command == nullptr ? args[0] : std::string(command->name);
    if (command_name == "help") {
        bool no_pager = false;
        const std::vector<std::string> words =
            take_flag(std::vector<std::string>(rest.begin(), rest.end()), kNoPagerFlag, no_pager);
        parsed.kind = ParsedArgs::Kind::Help;
        parsed.paged = !no_pager;
        if (!words.empty() && !is_help_flag(words.front()))
            parsed.command = words.front();  // `note help KOMUT`
        return parsed;
    }
    // Kabuğun gizli yardımcısı (`__complete`, tabloda yok) önek olarak `-h` alabilir: yardım değildir.
    if (command != nullptr && asks_for_help(*command, rest)) {
        parsed.kind = ParsedArgs::Kind::Help;
        parsed.command = command_name;
        parsed.paged = !std::ranges::contains(rest, kNoPagerFlag);
        return parsed;
    }
    parsed.kind = ParsedArgs::Kind::Reserved;
    parsed.command = command_name;
    if (command == nullptr) {
        parsed.rest.assign(rest.begin(), rest.end());  // `__complete`: önek olduğu gibi kalır
        return parsed;
    }
    parsed.rest = split_inline_values(
        rest, [command](std::string_view name) { return command_takes_value(*command, name); });
    if (is_pageable(*command, parsed.rest)) {
        bool no_pager = false;
        parsed.rest = take_flag(std::move(parsed.rest), kNoPagerFlag, no_pager);
        parsed.paged = !no_pager;
    }
    return parsed;
}

}  // namespace

bool is_reserved_command(std::string_view word) {
    return word == kCompleteCommand || find_command(word) != nullptr;
}

ParsedArgs parse_args(const std::vector<std::string>& args) {
    ParsedArgs parsed;

    // Alt komut (adı, eş adı ya da `-l`/`--ls` bayrağı) yalnız ilk argüman olabilir; `note -t md ls` "ls"
    // metnini ekler, `note -n -i` "-i" adını verir (değer konumu kazanır).
    if (!args.empty() && is_reserved_command(args[0]))
        return parse_command(args);

    const std::vector<std::string> words =
        split_inline_values(args, [](std::string_view name) { return takes_value(name, kAddOptions); });
    bool only_text = false;  // `--` sonrası her şey metindir
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string& arg = words[i];
        if (only_text || arg.empty() || arg[0] != '-' || arg == "-") {
            parsed.text.push_back(arg);
            continue;
        }
        if (arg == "--") {
            only_text = true;
        } else if (arg == "--help" || arg == "-h") {
            parsed.kind = ParsedArgs::Kind::Help;
            parsed.paged = !std::ranges::contains(words, kNoPagerFlag);
            return parsed;
        } else if (arg == "--version") {
            parsed.kind = ParsedArgs::Kind::Version;
            return parsed;
        } else if (arg == "-a" || arg == "--add") {
            // Ekleme zaten varsayılan; bayrak yalnız ilk argümanın komut sayılmasını önler (`note -a ls`).
        } else if (arg == "--no-sync") {
            parsed.no_sync = true;
        } else if (arg == "-e" || arg == "--editor") {
            parsed.editor = true;
        } else if (arg == "-q" || arg == "--quiet") {
            parsed.quiet = true;
        } else if (arg == "-t" || arg == "--type" || arg == "-f" || arg == "--folder" || arg == "-n" ||
                   arg == "--name") {
            if (i + 1 >= words.size())
                return invalid("cli.missing_value", {{"option", arg}});
            auto& slot = arg == "-t" || arg == "--type"     ? parsed.type
                         : arg == "-f" || arg == "--folder" ? parsed.folder
                                                            : parsed.name;
            slot = words[++i];
        } else {
            return invalid("cli.unknown_option", {{"option", arg}});
        }
    }
    return parsed;
}

}  // namespace noteapp::cli
