// Yardım ve hata biçimi (SPEC-cli "Yardım", "Hata biçimi"): ilk satır bugünkü mesaj, ardından `hint:` satırları.
#include "cli/command_table.hpp"
#include "support/cli_sandbox.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace noteapp::test;

namespace {

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string first_line(const std::string& text) {
    return text.substr(0, text.find('\n'));
}

}  // namespace

TEST_CASE("unknown command in help keeps its first line and adds hints") {
    const Sandbox box("en");
    const auto bogus = box.run({"help", "bogus"});
    CHECK(bogus.exit_code == 2);
    CHECK(first_line(bogus.err) == "Unknown command: bogus");
    CHECK(contains(bogus.err, "\n  hint: run 'note help' for the command list\n"));
    CHECK_FALSE(contains(bogus.err, "did you mean"));

    const auto near = box.run({"help", "shw"});
    CHECK(near.exit_code == 2);
    CHECK(contains(near.err, "  hint: did you mean 'show'?\n"));
}

TEST_CASE("unknown verb suggests the closest verb and the command help") {
    const Sandbox box("en");
    const auto result = box.run({"sync", "stauts"});
    CHECK(result.exit_code == 2);
    CHECK(first_line(result.err).starts_with("Usage: note sync"));
    CHECK(contains(result.err, "  hint: did you mean 'status'?\n"));
    CHECK(contains(result.err, "  hint: see 'note help sync'\n"));
}

TEST_CASE("unknown option suggests the closest option of that command") {
    const Sandbox box("en");
    const auto result = box.run({"find", "--limt", "5", "x"});
    CHECK(result.exit_code == 2);
    CHECK(first_line(result.err) == "Unknown option: --limt");
    CHECK(contains(result.err, "  hint: did you mean '--limit'?\n"));
    CHECK(contains(result.err, "  hint: see 'note help find'\n"));

    const auto quick = box.run({"--versoin"});
    CHECK(quick.exit_code == 2);
    CHECK(first_line(quick.err) == "Unknown option: --versoin");
    CHECK(contains(quick.err, "  hint: did you mean '--version'?\n"));
    CHECK(contains(quick.err, "  hint: see 'note --help'\n"));
}

TEST_CASE("every usage error points at the command help, in both languages") {
    const std::vector<std::vector<std::string>> misuses{
        {"rm"},     {"mv"},    {"show"},   {"edit"},  {"find"},       {"tags", "x"},    {"import"}, {"view"},
        {"graph"},  {"config"}, {"query"}, {"completion"}, {"ls", "a", "b"}, {"export"}, {"sync", "bogus"},
    };
    for (const auto& [language, hint] : {std::pair{"en", "  hint: see 'note help "},
                                         std::pair{"tr", "  ipucu: ayrıntı için 'note help "}}) {
        const Sandbox box(language);
        for (const auto& args : misuses) {
            CAPTURE(language);
            CAPTURE(args.front());
            const auto result = box.run(args);
            CHECK(result.exit_code == 2);
            CHECK(contains(result.err, hint + args.front() + "'\n"));
            // İlk satır ipucu değil, bugünkü mesajdır.
            CHECK_FALSE(first_line(result.err).starts_with("  "));
        }
    }
}

TEST_CASE("missing option value points at the command help") {
    const Sandbox box("en");
    const auto result = box.run({"find", "x", "--limit"});
    CHECK(result.exit_code == 2);
    CHECK(first_line(result.err) == "Option --limit needs a value");
    CHECK(contains(result.err, "  hint: see 'note help find'\n"));
}

namespace {

// Terminal sütunu: UTF-8 devam baytları sayılmaz (Türkçe harf tek sütundur).
std::size_t widest_line(const std::string& text) {
    std::size_t widest = 0;
    std::size_t current = 0;
    for (const char c : text) {
        if (c == '\n') {
            widest = std::max(widest, current);
            current = 0;
        } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++current;
        }
    }
    return std::max(widest, current);
}

}  // namespace

TEST_CASE("command --help and -h print the same help as note help COMMAND") {
    const Sandbox box("en");
    for (const std::string command : {"ls", "sync"}) {
        CAPTURE(command);
        const auto help = box.run({"help", command});
        CHECK(help.exit_code == 0);
        CHECK(contains(help.out, "Usage:"));
        CHECK(contains(help.out, "Options:"));
        CHECK(contains(help.out, "Examples:"));
        for (const char* flag : {"--help", "-h"}) {
            const auto flagged = box.run({command, flag});
            CHECK(flagged.exit_code == 0);
            CHECK(flagged.out == help.out);
            CHECK(flagged.err.empty());
        }
    }
    // Yardım bayrağı diğer argümanları yok sayar.
    CHECK(box.run({"ls", "tag:x", "-f", "w", "--help"}).out == box.run({"help", "ls"}).out);
    CHECK(contains(box.run({"help", "sync"}).out, "Commands:"));
}

TEST_CASE("every command help has sections, fits 80 columns and names its options, in both languages") {
    for (const auto& [language, usage, options] :
         {std::tuple{"en", "Usage:", "Options:"}, std::tuple{"tr", "Kullanım:", "Seçenekler:"}}) {
        const Sandbox box(language);
        for (const auto& command : noteapp::cli::kCommands) {
            CAPTURE(language);
            CAPTURE(command.name);
            const auto help = box.run({"help", std::string(command.name)});
            CHECK(help.exit_code == 0);
            if (command.name == "help")
                continue;  // `note help help` genel yardımdır (ayrı test)
            CHECK(widest_line(help.out) <= 80);
            CHECK_FALSE(contains(help.out, "cli."));  // eksik katalog anahtarı
            CHECK(help.out.starts_with("note " + std::string(command.name)));
            CHECK(contains(help.out, usage));
            CHECK(contains(help.out, options));
            for (const auto& option : command.options)
                CHECK(contains(help.out, std::string(option.name)));
            for (const auto& verb : command.verbs)
                CHECK(contains(help.out, "  " + std::string(verb.name)));
        }
    }
}

TEST_CASE("help flags after -- are text, and the completion helper never prints help") {
    const Sandbox box("en");
    box.put("a.md", "x\n");
    const auto term = box.run({"find", "--", "-h"});
    CHECK(term.exit_code == 1);
    CHECK_FALSE(contains(term.out, "Usage:"));
    const auto completion = box.run({"__complete", "query", "-h"});
    CHECK(completion.exit_code == 0);
    CHECK_FALSE(contains(completion.out, "Usage:"));
}

TEST_CASE("top help groups every command in four groups within 80 columns, in both languages") {
    for (const auto& [language, groups] :
         {std::pair{"en", std::vector<std::string>{"Notes:", "Sync:", "Advanced:", "Other:"}},
          std::pair{"tr", std::vector<std::string>{"Notlar:", "Senkron:", "Gelişmiş:", "Diğer:"}}}) {
        CAPTURE(language);
        const Sandbox box(language);
        const auto help = box.run({"help"});
        CHECK(help.exit_code == 0);
        CHECK(widest_line(help.out) <= 80);
        CHECK_FALSE(contains(help.out, "cli."));
        CHECK(box.run({"--help"}).out == help.out);
        CHECK(box.run({"-h"}).out == help.out);
        CHECK(box.run({"help", "--help"}).out == help.out);
        std::size_t previous = 0;
        for (const std::string& group : groups) {
            CAPTURE(group);
            const std::size_t at = help.out.find("\n" + group + "\n");
            REQUIRE(at != std::string::npos);
            CHECK(at > previous);
            previous = at;
        }
        for (const auto& command : noteapp::cli::kCommands)
            CHECK(contains(help.out, "\n  note " + std::string(command.name) + " "));
        CHECK(contains(help.out, "note help"));
        CHECK(contains(help.out, "--version"));
    }
}

TEST_CASE("Turkish command help uses Turkish headings and examples") {
    const Sandbox box("tr");
    const auto help = box.run({"ls", "--help"});
    CHECK(help.exit_code == 0);
    CHECK(contains(help.out, "Kullanım:"));
    CHECK(contains(help.out, "Seçenekler:"));
    CHECK(contains(help.out, "Örnekler:"));
    CHECK(contains(help.out, "Ayrıca bakınız:"));
}
