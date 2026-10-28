#include "core/config.hpp"
#include "core/editor.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "support/test_support.hpp"

#include <doctest/doctest.h>

using namespace noteapp::core;
using namespace noteapp::test;
using Args = std::vector<std::string>;

namespace {

const std::string kFakeEditor = NOTEAPP_FAKE_EDITOR;

Config make_config(std::string editor = "") {
    Config config = parse_config("").config;
    config.editor = std::move(editor);
    return config;
}

std::string quoted_fake_editor() {
    return "\"" + kFakeEditor + "\"";
}

}  // namespace

TEST_CASE("split_command_line splits on whitespace and honours quotes") {
    CHECK(split_command_line("code --wait") == Args{"code", "--wait"});
    CHECK(split_command_line("  code \t --wait  ") == Args{"code", "--wait"});
    CHECK(split_command_line("\"C:\\Program Files\\x\\y.exe\" -n") ==
          Args{"C:\\Program Files\\x\\y.exe", "-n"});
    CHECK(split_command_line("'my editor' -x \"a b\"") == Args{"my editor", "-x", "a b"});
    CHECK(split_command_line("ed \"say \\\"hi\\\"\"") == Args{"ed", "say \"hi\""});
    CHECK(split_command_line("C:\\bin\\ed.exe \"\"") == Args{"C:\\bin\\ed.exe", ""});
    CHECK(split_command_line("it'\"s\"'") == Args{"it\"s\""});
    CHECK(split_command_line("").empty());
    CHECK(split_command_line("   ").empty());
}

TEST_CASE("resolve_editor honours config, VISUAL, EDITOR in order") {
    ScopedEnv visual("VISUAL");
    ScopedEnv editor("EDITOR");
    visual.set("vis --wait");
    editor.set("ed");

    CHECK(resolve_editor(make_config("cfg -x")).value() == Args{"cfg", "-x"});
    CHECK(resolve_editor(make_config()).value() == Args{"vis", "--wait"});
    visual.unset();
    CHECK(resolve_editor(make_config()).value() == Args{"ed"});
    CHECK(resolve_editor(make_config("  ")).value() == Args{"ed"});  // boşluk-only yapılandırma yok sayılır
}

TEST_CASE("resolve_editor falls back to a platform default found on PATH") {
    ScopedEnv visual("VISUAL");
    ScopedEnv editor("EDITOR");
    visual.unset();
    editor.unset();
    const auto result = resolve_editor(make_config());
    if (result.has_value()) {  // makinede edit/notepad ya da nano/vi var
        REQUIRE(result->size() == 1);
        CHECK(find_program(result->front()).has_value());
    } else {
        CHECK(result.error().code == Error::Code::EditorNotFound);
    }
}

TEST_CASE("resolve_editor lists the candidates when nothing can be found") {
    ScopedEnv visual("VISUAL");
    ScopedEnv editor("EDITOR");
    visual.unset();
    editor.unset();
    TempDir empty_dir;
    ScopedEnv path("PATH", utf8(empty_dir.path()));

    const auto result = resolve_editor(make_config());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::EditorNotFound);
    CHECK(result.error().message_key == "error.editor_not_found");
    REQUIRE(result.error().args.size() == 1);
    CHECK(result.error().args[0].first == "candidates");
#ifdef _WIN32
    CHECK(result.error().args[0].second == "edit, notepad");
#else
    CHECK(result.error().args[0].second == "nano, vi");
#endif
}

TEST_CASE("run_editor lets the editor write into a path with Turkish characters and spaces") {
    TempDir dir(u8" ğüşıöç dizin");
    const auto file = dir.path() / u8"not ığ üş.md";
    ScopedEnv content("FAKE_EDITOR_CONTENT", "# Başlık\nçğüşıö\n");

    const auto result = run_editor(make_config(quoted_fake_editor()), file);
    REQUIRE(result.has_value());
    CHECK(slurp(file) == "# Başlık\nçğüşıö\n");
}

TEST_CASE("run_editor passes configured arguments and appends the file last") {
    TempDir dir(u8" ğüş");
    const auto file = dir.path() / u8"n ö.md";
    const auto args_out = dir.path() / "args.txt";
    ScopedEnv out("FAKE_EDITOR_ARGS_OUT", utf8(args_out));

    REQUIRE(run_editor(make_config(quoted_fake_editor() + " --wait \"two words\""), file).has_value());
    CHECK(slurp(args_out) == "--wait\ntwo words\n" + utf8(file) + "\n");
}

TEST_CASE("run_editor maps a non-zero exit to EditorFailed") {
    TempDir dir;
    ScopedEnv exit_code("FAKE_EDITOR_EXIT", "3");
    const auto result = run_editor(make_config(quoted_fake_editor()), dir.path() / "n.md");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::EditorFailed);
    CHECK(result.error().message_key == "error.editor_failed");
    CHECK(result.error().args.at(1) == std::pair<std::string, std::string>{"code", "3"});
}

TEST_CASE("run_editor maps a missing editor program to EditorNotFound") {
    TempDir dir;
    const auto result = run_editor(make_config("noteapp-no-such-editor-xyz"), dir.path() / "n.md");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::EditorNotFound);
    CHECK(result.error().args.at(0).second == "noteapp-no-such-editor-xyz");
}

TEST_CASE("create_private_temp_file lives under the user-private state dir with owner-only permissions") {
    const TempDir home;
    const ScopedEnv env("NOTEAPP_HOME", utf8(home.path()));

    const auto first = create_private_temp_file("md", "ön içerik");
    const auto second = create_private_temp_file("md", "");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);
    CHECK(first->parent_path() == state_dir() / "tmp");
    CHECK(first->filename().string().starts_with("noteapp-"));
    CHECK(first->extension() == ".md");
    CHECK(slurp(*first) == "ön içerik");
#ifndef _WIN32
    // Yalnız POSIX'te anlamlı (Windows'ta izinler üst dizinden miras kalır).
    using fs::perms;
    CHECK(fs::status(*first).permissions() == (perms::owner_read | perms::owner_write));
    CHECK(fs::status(state_dir() / "tmp").permissions() == perms::owner_all);
#endif
}
