// Komutların kısa/uzun bayrakları (SPEC-cli "Kısa komut bayrakları"): ilk argüman olan `-l`/`--ls` = `ls`,
// `note cat` = `show`, `-a` düz ekleme. Ayrıştırıcı birim testleri ve ikilinin aynı çıktıyı verdiği uçtan uca testler.
#include "cli/args.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <vector>

using namespace noteapp::test;
using noteapp::cli::parse_args;
using noteapp::cli::ParsedArgs;

namespace {

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

struct Mapping {
    const char* short_flag;
    const char* long_flag;
    const char* command;
};

// Kullanıcı kararı (2026-11-05), TUI tuşlarıyla aynı harfler. Testin bağımsız kahini: tablo buradan sapınca düşer.
constexpr Mapping kMappings[] = {
    {"-l", "--ls", "ls"},     {"-s", "--find", "find"},     {"-S", "--show", "show"}, {"-E", "--edit", "edit"},
    {"-d", "--rm", "rm"},     {"-m", "--mv", "mv"},         {"-x", "--export", "export"},
    {"-T", "--tags", "tags"}, {"-i", "--import", "import"},
};

}  // namespace

TEST_CASE("every command flag as the first argument runs that command with the rest unchanged") {
    for (const Mapping& mapping : kMappings) {
        for (const char* flag : {mapping.short_flag, mapping.long_flag}) {
            CAPTURE(flag);
            const ParsedArgs bare = parse_args({flag});
            CHECK(bare.kind == ParsedArgs::Kind::Reserved);
            CHECK(bare.command == mapping.command);
            CHECK(bare.rest.empty());

            const ParsedArgs with_args = parse_args({flag, "a.txt", "--", "-y"});
            CHECK(with_args.kind == ParsedArgs::Kind::Reserved);
            CHECK(with_args.command == mapping.command);
            CHECK(with_args.rest == std::vector<std::string>{"a.txt", "--", "-y"});

            // Paging, --name=value splitting and help follow the command, as with the long form.
            const ParsedArgs plain = parse_args({mapping.command, "a.txt"});
            CHECK(parse_args({flag, "a.txt"}).paged == plain.paged);
            const ParsedArgs help = parse_args({flag, "-h"});
            CHECK(help.kind == ParsedArgs::Kind::Help);
            CHECK(help.command == mapping.command);
        }
    }
    const ParsedArgs imported = parse_args({"-i", "a.txt", "b.md", "--folder", "x"});
    CHECK(imported.command == "import");
    CHECK(imported.rest == std::vector<std::string>{"a.txt", "b.md", "--folder", "x"});
    const ParsedArgs found = parse_args({"-s", "word", "-n", "5"});
    CHECK(found.command == "find");
    CHECK(found.rest == std::vector<std::string>{"word", "-n", "5"});
    CHECK(parse_args({"--find", "word", "--limit=5"}).rest == std::vector<std::string>{"word", "--limit", "5"});
}

TEST_CASE("cat is show") {
    const ParsedArgs parsed = parse_args({"cat", "plan"});
    CHECK(parsed.kind == ParsedArgs::Kind::Reserved);
    CHECK(parsed.command == "show");
    CHECK(parsed.rest == std::vector<std::string>{"plan"});
    CHECK(noteapp::cli::is_reserved_command("cat"));
}

TEST_CASE("-a and --add are a plain add, even of a command name") {
    for (const char* flag : {"-a", "--add"}) {
        CAPTURE(flag);
        const ParsedArgs parsed = parse_args({flag, "hello", "world"});
        CHECK(parsed.kind == ParsedArgs::Kind::Add);
        CHECK(parsed.text == std::vector<std::string>{"hello", "world"});
        CHECK_FALSE(parsed.name.has_value());
        CHECK(parse_args({flag, "ls"}).text == std::vector<std::string>{"ls"});
        CHECK(parse_args({flag}).kind == ParsedArgs::Kind::Add);
        CHECK(parse_args({flag}).text.empty());
    }
    const ParsedArgs typed = parse_args({"-a", "-t", "md", "x"});
    CHECK(typed.kind == ParsedArgs::Kind::Add);
    CHECK(typed.type == "md");
    CHECK(typed.text == std::vector<std::string>{"x"});
}

TEST_CASE("-n NAME TEXT stays a named add") {
    const ParsedArgs parsed = parse_args({"-n", "deneme", "a", "b"});
    CHECK(parsed.kind == ParsedArgs::Kind::Add);
    CHECK(parsed.name == "deneme");
    CHECK(parsed.text == std::vector<std::string>{"a", "b"});
}

TEST_CASE("a value position wins over a command flag") {
    // -n takes the next word as its value, whatever it looks like.
    const ParsedArgs named = parse_args({"-n", "-i"});
    CHECK(named.kind == ParsedArgs::Kind::Add);
    CHECK(named.name == "-i");
    CHECK(named.text.empty());
    const ParsedArgs typed = parse_args({"-t", "-l", "x"});
    CHECK(typed.kind == ParsedArgs::Kind::Add);
    CHECK(typed.type == "-l");
    // Inside a command the flag letters keep that command's meaning: `ls -l` is the long listing.
    const ParsedArgs listed = parse_args({"-l", "-l"});
    CHECK(listed.command == "ls");
    CHECK(listed.rest == std::vector<std::string>{"-l"});
}

TEST_CASE("a command flag that is not the first argument is not a command") {
    const ParsedArgs later = parse_args({"-t", "md", "-i", "x"});
    CHECK(later.kind == ParsedArgs::Kind::Invalid);
    CHECK(later.error_key == "cli.unknown_option");
    CHECK(parse_args({"hello", "-S"}).kind == ParsedArgs::Kind::Invalid);
    // `--` ends the options: the text may start with a command flag.
    const ParsedArgs literal = parse_args({"--", "-i", "x"});
    CHECK(literal.kind == ParsedArgs::Kind::Add);
    CHECK(literal.text == std::vector<std::string>{"-i", "x"});
    // Existing options keep their meaning.
    CHECK(parse_args({"-e", "x"}).editor);
    CHECK(parse_args({"-q", "x"}).quiet);
    CHECK(parse_args({"-f", "w", "x"}).folder == "w");
    CHECK(parse_args({"-h"}).kind == ParsedArgs::Kind::Help);
    CHECK(parse_args({"-h"}).command.empty());
    CHECK(parse_args({"--help", "x"}).command.empty());
    // Commands without a flag have no long flag either.
    CHECK(parse_args({"--sync"}).kind == ParsedArgs::Kind::Invalid);
    CHECK(parse_args({"--help"}).kind == ParsedArgs::Kind::Help);
}

TEST_CASE("short command flags print what the long command prints, exit codes and JSON included") {
    const Sandbox box("en");
    box.put("w/plan.md", "budget line\n", 100);
    box.put("deneme.md", "#tag x\nsecond\n");
    for (const std::vector<std::string>& pair : {
             std::vector<std::string>{"-l", "ls"},
             std::vector<std::string>{"--ls", "ls"},
             std::vector<std::string>{"-T", "tags"},
         }) {
        CAPTURE(pair.front());
        const auto flagged = box.run({pair.front()});
        const auto named = box.run({pair.back()});
        CHECK(flagged.exit_code == named.exit_code);
        CHECK(flagged.out == named.out);
        CHECK(box.run({pair.front(), "--json"}).out == box.run({pair.back(), "--json"}).out);
    }
    CHECK(box.run({"-s", "budget", "-n", "5"}).out == box.run({"find", "budget", "-n", "5"}).out);
    CHECK(box.run({"-s", "nothing-here"}).exit_code == 1);
    const auto shown = box.run({"-S", "deneme"});
    CHECK(shown.exit_code == 0);
    CHECK(shown.out == "#tag x\nsecond\n");
    CHECK(box.run({"cat", "deneme"}).out == shown.out);
    CHECK(box.run({"--show", "zzzz-none"}).exit_code == 1);

    CHECK(box.run({"-m", "w/plan.md", "w/plan2.md"}).exit_code == 0);
    CHECK(box.exists("w/plan2.md"));
    CHECK(box.run({"-d", "w/plan2.md", "-y"}).exit_code == 0);
    CHECK_FALSE(box.exists("w/plan2.md"));
    CHECK(box.run({"-d", "w/plan2.md"}).exit_code == box.run({"rm", "w/plan2.md"}).exit_code);

    const TempDir out;
    CHECK(box.run({"-x", "--all", utf8(out.path() / "dump")}).exit_code == 0);
    CHECK(fs::exists(out.path() / "dump" / "deneme.md"));
}

TEST_CASE("-i imports the remaining paths with their options") {
    const Sandbox box("en");
    const TempDir files;
    std::ofstream(files.path() / "a.txt", std::ios::binary) << "a\n";
    std::ofstream(files.path() / "b.md", std::ios::binary) << "b\n";
    const auto result = box.run({"-i", utf8(files.path() / "a.txt"), utf8(files.path() / "b.md"), "--folder", "x"});
    INFO(result.err);
    CHECK(result.exit_code == 0);
    CHECK(box.body("x/a.txt") == "a\n");
    CHECK(box.body("x/b.md") == "b\n");
}

TEST_CASE("-a adds plain text, -n adds a named note from stdin, -- adds text that starts with a flag") {
    const Sandbox box("en");
    const auto plain = box.run({"-a", "ls"});
    CHECK(plain.exit_code == 0);
    CHECK(box.body("ls.md") == "ls");
    CHECK(box.run({"-n", "deneme", "-t", "txt"}, "from a file\n").exit_code == 0);
    CHECK(box.body("deneme.txt") == "from a file\n");
    CHECK(box.run({"-n", "dash", "--", "-i"}).exit_code == 0);
    CHECK(box.body("dash.md") == "-i");
    // A flag that is not first stays an unknown option, as before.
    const auto later = box.run({"x", "-i"});
    CHECK(later.exit_code == 2);
    CHECK(contains(later.err, "Unknown option: -i"));
}

TEST_CASE("help shows the short forms: command help, top help, and help by alias") {
    for (const char* language : {"en", "tr"}) {
        CAPTURE(language);
        const Sandbox box(language);
        const auto show = box.run({"help", "show"});
        CHECK(show.exit_code == 0);
        CHECK(contains(show.out, "note -S"));
        CHECK(contains(show.out, "note --show"));
        CHECK(contains(show.out, "note cat"));
        CHECK(box.run({"-S", "--help"}).out == show.out);
        CHECK(box.run({"cat", "-h"}).out == show.out);
        CHECK(box.run({"help", "cat"}).out == show.out);
        CHECK(box.run({"help", "-S"}).out == show.out);
        for (const Mapping& mapping : kMappings) {
            CAPTURE(mapping.command);
            CHECK(contains(box.run({"help", mapping.command}).out, std::string("note ") + mapping.short_flag));
        }
        CHECK_FALSE(contains(box.run({"help", "sync"}).out, "note --sync"));

        const auto top = box.run({"--help"});
        CHECK(contains(top.out, "  note ls (-l)"));
        CHECK(contains(top.out, "  note show (-S, cat)"));
        CHECK(contains(top.out, "-a, --add"));
        CHECK(contains(top.out, "note -- -i"));
    }
}

TEST_CASE("completion scripts offer the command flags and the cat alias") {
    const Sandbox box("en");
    const auto zsh = box.run({"completion", "zsh"}).out;
    CHECK(contains(zsh, "'-S:Print the best match'"));
    CHECK(contains(zsh, "'--show:Print the best match'"));
    CHECK(contains(zsh, "'cat:Print the best match'"));
    CHECK(contains(zsh, "'-a:"));
    const auto bash = box.run({"completion", "bash"}).out;
    CHECK(contains(bash, " '-i'"));
    CHECK(contains(bash, " '--import'"));
    CHECK(contains(bash, " 'cat'"));
    const auto fish = box.run({"completion", "fish"}).out;
    CHECK(contains(fish, "-s S -l show"));
    CHECK(contains(fish, "-s i -l import"));
    CHECK(contains(fish, "-a 'cat'"));
}
