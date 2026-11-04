// `notes` ve `notes-gui` komut satırı: argümansız açılış aynı kalır, `--help`, `notes import`.
#include "app/ui_args.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <vector>

using noteapp::app::parse_ui_args;
using noteapp::app::UiArgs;
using Kind = UiArgs::Kind;

namespace {

UiArgs parse(std::vector<std::string> args, bool allow_import = true) {
    return parse_ui_args(args, allow_import);
}

}  // namespace

TEST_CASE("no arguments opens the interface as before; --help and -h ask for usage") {
    CHECK(parse({}).kind == Kind::Run);
    CHECK(parse({}, false).kind == Kind::Run);
    CHECK(parse({"--help"}).kind == Kind::Help);
    CHECK(parse({"-h"}, false).kind == Kind::Help);
}

TEST_CASE("notes import takes paths and the folder, name and overwrite options in any order") {
    const UiArgs args = parse({"import", "a.txt", "--folder", "inbox", "--overwrite", "b dir/"});
    CHECK(args.kind == Kind::Import);
    CHECK(args.sources == std::vector<std::filesystem::path>{"a.txt", "b dir/"});
    CHECK(args.folder == "inbox");
    CHECK(args.overwrite);
    const UiArgs named = parse({"import", "-n", "plan.md", "-f", "", "--", "-odd.txt"});
    CHECK(named.kind == Kind::Import);
    CHECK(named.name == "plan.md");
    CHECK(named.folder == "");
    CHECK(named.sources == std::vector<std::filesystem::path>{"-odd.txt"});
}

TEST_CASE("usage errors: no path, --name with two paths, missing value, unknown option or word") {
    CHECK(parse({"import"}).kind == Kind::Invalid);
    CHECK(parse({"import", "a", "b", "--name", "x"}).kind == Kind::Invalid);
    const UiArgs missing = parse({"import", "a", "--folder"});
    CHECK(missing.kind == Kind::Invalid);
    CHECK(missing.error_key == "cli.missing_value");
    const UiArgs unknown = parse({"import", "a", "--json"});
    CHECK(unknown.kind == Kind::Invalid);
    CHECK(unknown.error_key == "cli.unknown_option");
    CHECK(parse({"--bogus"}).error_key == "cli.unknown_option");
    CHECK(parse({"plan.md"}).error_key == "cli.unknown_command");
    // GUI'nin içe alması diyalogdadır: alt komut yok.
    CHECK(parse({"import", "a"}, false).kind == Kind::Invalid);
}
