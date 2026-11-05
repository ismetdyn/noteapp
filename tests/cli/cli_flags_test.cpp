// Uzun bayrak adları, `--ad=DEĞER` biçimi ve -q/--quiet (SPEC-cli "Bayraklar"). Eski kısa adlar aynen çalışır.
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <string>
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

TEST_CASE("quick add long flags and --flag=value equal the short flags") {
    for (const std::vector<std::string>& args : {
             std::vector<std::string>{"-t", "txt", "-f", "w", "-n", "plan", "x"},
             std::vector<std::string>{"--type", "txt", "--folder", "w", "--name", "plan", "x"},
             std::vector<std::string>{"--type=txt", "--folder=w", "--name=plan", "x"},
         }) {
        CAPTURE(args.front());
        const Sandbox box("en");
        const auto result = box.run(args);
        CHECK(result.exit_code == 0);
        CHECK(result.out == "Saved: w/plan.txt (txt, chosen)\n");
        CHECK(box.body("w/plan.txt") == "x");
    }
}

TEST_CASE("an equals sign outside a known long option stays note text") {
    const Sandbox box("en");
    CHECK(box.run({"-n", "eq", "a=b"}).exit_code == 0);
    CHECK(box.body("eq.md") == "a=b");
    CHECK(box.run({"-n", "raw", "--", "--type=md"}).exit_code == 0);
    CHECK(box.body("raw.md") == "--type=md");
    // Değer almayan seçenek `=` ile değer almaz.
    CHECK(box.run({"--no-sync=yes", "x"}).exit_code == 2);
}

TEST_CASE("quiet suppresses success lines only") {
    const Sandbox box("en");
    for (const char* flag : {"-q", "--quiet"}) {
        CAPTURE(flag);
        const auto added = box.run({flag, "-n", std::string("n") + flag, "x"});
        CHECK(added.exit_code == 0);
        CHECK(added.out.empty());
        CHECK(added.err.empty());
    }
    CHECK(box.exists("n-q.md"));
    CHECK(box.exists("n--quiet.md"));

    const auto bad = box.run({"-q", "-n", "../escape", "x"});
    CHECK(bad.exit_code == 2);
    CHECK_FALSE(bad.err.empty());

    const auto moved = box.run({"mv", "-q", "n-q.md", "m.md"});
    CHECK(moved.exit_code == 0);
    CHECK(moved.out.empty());
    CHECK(box.exists("m.md"));

    const auto removed = box.run({"rm", "--quiet", "-y", "m.md"});
    CHECK(removed.exit_code == 0);
    CHECK(removed.out.empty());
    CHECK_FALSE(box.exists("m.md"));
}

TEST_CASE("rm --yes, ls --folder and export --output are the long names of -y, -f and -o") {
    const Sandbox box("en");
    box.put("w/a.md", "a\n");
    box.put("b.md", "b\n");
    CHECK(box.run({"ls", "--folder", "w"}).out == "w/a.md\n");
    CHECK(box.run({"ls", "--folder=w"}).out == "w/a.md\n");

    const TempDir out;
    const auto exported = box.run({"export", "--pdf", "b.md", "--output", utf8(out.path() / "b.pdf")});
    CHECK(exported.exit_code == 0);
    CHECK(fs::exists(out.path() / "b.pdf"));

    const auto removed = box.run({"rm", "--yes", "b.md"});
    CHECK(removed.exit_code == 0);
    CHECK(removed.out == "Deleted: b.md\n");
    CHECK_FALSE(box.exists("b.md"));
}

TEST_CASE("import --quiet keeps problems and --json output") {
    const Sandbox box("en");
    const TempDir source;
    {
        std::ofstream(source.path() / "a.txt", std::ios::binary) << "a";
    }
    const auto quiet = box.run({"import", "-q", utf8(source.path() / "a.txt")});
    CHECK(quiet.exit_code == 0);
    CHECK(quiet.out.empty());
    const auto json = box.run({"import", "--quiet", "--json", utf8(source.path() / "a.txt")});
    CHECK(json.exit_code == 0);
    CHECK(json.out.starts_with("{\"imported\":["));
    const auto missing = box.run({"import", "-q", utf8(source.path() / "yok.txt")});
    CHECK(missing.exit_code == 1);
    CHECK_FALSE(missing.err.empty());
}

TEST_CASE("completion scripts offer the new long names") {
    const Sandbox box("en");
    for (const char* shell : {"zsh", "bash", "fish"}) {
        CAPTURE(shell);
        const auto script = box.run({"completion", shell}).out;
        for (const std::string name : {"yes", "quiet", "folder", "type", "name", "output", "editor"})
            CHECK(contains(script, std::string_view(shell) == "fish" ? "-l " + name : "--" + name));
    }
}

TEST_CASE("a word after a value-taking option is its value even if it looks like --option=value") {
    const Sandbox box("en");
    const auto result = box.run({"-n", "--type=x", "hello"});
    CHECK(result.exit_code == 0);
    CHECK(box.body("--type=x.md") == "hello");
    CHECK_FALSE(box.exists("--type.md"));
}

// Davranış eski ikiliyle (d08f276) karşılaştırıldı: -h/--help bir seçeneğin ya da konumlu verili fiilin değeri
// olduğunda değer kalır, yardım basılmaz.
TEST_CASE("-h and --help as an option value or verb data stay values") {
    const Sandbox box("en");
    CHECK(box.run({"-n", "-h", "x"}).exit_code == 0);
    CHECK(box.body("-h.md") == "x");

    for (const char* value : {"-h", "--help"}) {
        CAPTURE(value);
        const auto set = box.run({"config", "set", "editor", value});
        CHECK(set.exit_code == 0);
        CHECK(set.out == std::string("editor = ") + value + "\n");
        CHECK(box.run({"config", "get", "editor"}).out == std::string(value) + "\n");
    }

    const auto view = box.run({"view", "save", "x", "-h"});
    CHECK(view.exit_code == 2);
    CHECK(view.out.empty());
    CHECK(first_line(view.err).starts_with("Usage: note view"));

    const auto folder = box.run({"ls", "-f", "-h"});
    CHECK(folder.exit_code == 0);
    CHECK(folder.out.empty());

    const auto limit = box.run({"find", "--limit", "-h", "x"});
    CHECK(limit.exit_code == 2);
    CHECK(first_line(limit.err) == "--limit needs a positive whole number, got '-h'");

    const auto param = box.run({"query", "--param", "-h"});
    CHECK(param.exit_code == 2);
    CHECK(param.out.empty());
}

TEST_CASE("-h and --help right after a command or a verb without data still show help") {
    const Sandbox box("en");
    for (const std::vector<std::string>& args : {std::vector<std::string>{"config", "-h"},
                                                 std::vector<std::string>{"view", "ls", "--help"},
                                                 std::vector<std::string>{"sync", "status", "-h"},
                                                 std::vector<std::string>{"ls", "-f", "w", "-h"}}) {
        CAPTURE(args.front());
        const auto result = box.run(args);
        CHECK(result.exit_code == 0);
        CHECK(result.out == box.run({"help", args.front()}).out);
    }
}

TEST_CASE("argument text echoed in error messages is cleaned of control bytes") {
    const Sandbox box("en");
    box.put("a.md", "a\n");
    const std::string evil = "\x1b]0;owned\x07z";
    const std::vector<std::pair<std::vector<std::string>, int>> cases{
        {{"find", "x", "--limit", evil}, 2},
        {{"show", evil}, 1},
        {{"find", evil}, 1},
        {{"config", "get", evil}, 2},
    };
    for (const auto& [args, code] : cases) {
        CAPTURE(args.front());
        const auto result = box.run(args);
        CHECK(result.exit_code == code);
        CHECK_FALSE(result.err.empty());
        CHECK(result.err.find('\x1b') == std::string::npos);
        CHECK(result.err.find('\x07') == std::string::npos);
    }
}
