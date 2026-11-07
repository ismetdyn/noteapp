// `note completion zsh|bash|fish`, gizli `note __complete TÜR [ÖNEK]` ve her komutun yardımı.
#include "cli/highlight.hpp"
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp::test;

namespace {

// Testin bağımsız kahini: kullanıcıya açık her alt komut. Tablo (command_table.hpp) buradan sapınca düşer.
constexpr std::array<std::string_view, 15> kCommands = {"ls",    "find",  "show", "edit",   "rm",
                                                        "mv",    "export", "sync", "config", "query",
                                                        "graph", "tags",  "view", "import", "completion"};

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

bool has_line(const std::string& text, std::string_view line) {
    const auto lines = lines_of(text);
    return std::ranges::find(lines, line) != lines.end();
}

void seed(const Sandbox& box) {
    box.put("iş/plan toplantı.md", "#proje gündem\n", 200);
    box.put("ev.md", "#çalışma liste\n", 100);
    box.put("arşiv/eski/not.txt", "düz\n", 50);
    const auto saved = box.run({"view", "save", "haftalık", "MATCH (n:Note) RETURN n"});
    REQUIRE_MESSAGE(saved.exit_code == 0, saved.err);
}

// Veri ve config dizinindeki her dosyanın baytları ve değişme zamanı.
std::map<std::string, std::pair<std::string, fs::file_time_type>> snapshot(const fs::path& root) {
    std::map<std::string, std::pair<std::string, fs::file_time_type>> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file())
            files[utf8(entry.path())] = {slurp(entry.path()), fs::last_write_time(entry.path())};
    }
    return files;
}

}  // namespace

TEST_CASE("note --help lists every command and help COMMAND explains each one, in both languages") {
    for (const char* language : {"en", "tr"}) {
        CAPTURE(language);
        const Sandbox box(language);
        const auto help = box.run({"--help"});
        CHECK(help.exit_code == 0);
        for (const std::string_view command : kCommands) {
            CAPTURE(command);
            CHECK(help.out.find("note " + std::string(command)) != std::string::npos);
            const auto one = box.run({"help", std::string(command)});
            CHECK(one.exit_code == 0);
            // Eksik katalog anahtarı anahtarın kendisi olarak basılırdı.
            CHECK(one.out.find("cli.help_") == std::string::npos);
            CHECK(one.out.find("note " + std::string(command)) != std::string::npos);
        }
        CHECK(help.out.find("__complete") == std::string::npos);
        CHECK(box.run({"help", "__complete"}).exit_code == 2);
    }
}

TEST_CASE("completion prints a script for zsh, bash and fish that names every command") {
    const Sandbox box("en");
    for (const char* shell : {"zsh", "bash", "fish"}) {
        CAPTURE(shell);
        const auto result = box.run({"completion", shell});
        INFO(result.err);
        REQUIRE(result.exit_code == 0);
        CHECK(result.out.find("__complete") != std::string::npos);  // dinamik değerler komuttan gelir
        for (const std::string_view command : kCommands) {
            CAPTURE(command);
            const std::string name(command);
            // zsh: _describe öğesi 'ad:açıklama'; bash: __note_words 'ad'; fish: -a 'ad'.
            const std::string entry = shell == std::string_view("zsh")    ? "'" + name + ":"
                                      : shell == std::string_view("bash") ? " '" + name + "'"
                                                                          : "-a '" + name + "'";
            CHECK(result.out.find(entry) != std::string::npos);
        }
        // Gizli yardımcı komut önerilmez.
        CHECK(result.out.find("'__complete:") == std::string::npos);
        CHECK(result.out.find("-a '__complete'") == std::string::npos);
    }
    CHECK(box.run({"completion"}).exit_code == 2);
    CHECK(box.run({"completion", "tcsh"}).exit_code == 2);
}

TEST_CASE("completion descriptions follow the user's language") {
    const auto en = Sandbox("en").run({"completion", "zsh"});
    const auto tr = Sandbox("tr").run({"completion", "zsh"});
    CHECK(en.out.find("'ls:List notes'") != std::string::npos);
    CHECK(tr.out.find("'ls:Notları listele'") != std::string::npos);
    const auto fish = Sandbox("tr").run({"completion", "fish"});
    CHECK(fish.out.find("-d 'Notları listele'") != std::string::npos);
}

TEST_CASE("__complete lists notes, folders, types, tags, views and config keys of a temp store") {
    const Sandbox box;
    seed(box);

    const auto notes = box.run({"__complete", "note"});
    CHECK(notes.exit_code == 0);
    CHECK(notes.err.empty());
    CHECK(has_line(notes.out, "iş/plan toplantı.md"));
    CHECK(has_line(notes.out, "ev.md"));
    CHECK(has_line(notes.out, "arşiv/eski/not.txt"));
    CHECK(lines_of(box.run({"__complete", "note", "iş/"}).out) == std::vector<std::string>{"iş/plan toplantı.md"});

    const auto folders = box.run({"__complete", "folder"}).out;
    CHECK(has_line(folders, "iş"));
    CHECK(has_line(folders, "arşiv/eski"));

    const auto types = box.run({"__complete", "type"}).out;
    CHECK(has_line(types, "md"));
    CHECK(has_line(types, "py"));

    const auto tags = box.run({"__complete", "tag"}).out;
    CHECK(has_line(tags, "proje"));
    CHECK(has_line(tags, "çalışma"));

    CHECK(lines_of(box.run({"__complete", "view"}).out) == std::vector<std::string>{"haftalık"});

    const auto keys = box.run({"__complete", "config-key"}).out;
    CHECK(has_line(keys, "language"));
    CHECK(has_line(keys, "limits.search.content_hits"));
    CHECK_FALSE(has_line(keys, "sync.token"));

    CHECK(box.run({"__complete", "bogus"}).exit_code == 2);
    CHECK(box.run({"__complete"}).exit_code == 2);
}

TEST_CASE("__complete query offers argument names, then values after tag:, type:, in: and view:") {
    const Sandbox box;
    seed(box);
    const auto names = lines_of(box.run({"__complete", "query"}).out);
    for (const char* key : {"tag:", "type:", "in:", "size:", "updated:", "created:", "origin:", "sort:", "sync:",
                            "view:"}) {
        CAPTURE(key);
        CHECK(std::ranges::find(names, key) != names.end());
    }
    CHECK(has_line(box.run({"__complete", "query", "tag:pr"}).out, "tag:proje"));
    CHECK(has_line(box.run({"__complete", "query", "tag:"}).out, "tag:çalışma"));
    CHECK(has_line(box.run({"__complete", "query", "type:"}).out, "type:md"));
    CHECK(has_line(box.run({"__complete", "query", "in:"}).out, "in:arşiv/eski"));
    CHECK(has_line(box.run({"__complete", "query", "view:"}).out, "view:haftalık"));
    // Tırnaklı çok sözcüklü sorguda yalnız son sözcük tamamlanır, öncesi aynen kalır.
    CHECK(has_line(box.run({"__complete", "query", "plan tag:"}).out, "plan tag:proje"));
    CHECK(has_line(box.run({"__complete", "query", "plan t"}).out, "plan tag:"));
    CHECK(box.run({"__complete", "query", "size:"}).out.empty());
}

TEST_CASE("__complete writes nothing to the store or the config") {
    const Sandbox box;
    seed(box);
    const auto data_before = snapshot(box.data());
    const auto config_before = slurp(box.config_file());
    const auto config_time = fs::last_write_time(box.config_file());
    for (const char* kind : {"note", "folder", "type", "tag", "view", "config-key", "query"})
        CHECK(box.run({"__complete", kind}).exit_code == 0);
    CHECK(box.run({"__complete", "query", "tag:"}).exit_code == 0);
    CHECK(snapshot(box.data()) == data_before);
    CHECK(slurp(box.config_file()) == config_before);
    CHECK(fs::last_write_time(box.config_file()) == config_time);
}

TEST_CASE("__complete without a store or with a damaged store prints nothing") {
    {
        const Sandbox box;
        const auto result = box.run({"__complete", "note"});
        CHECK(result.exit_code == 0);
        CHECK(result.out.empty());
        CHECK(result.err.empty());
        CHECK_FALSE(fs::exists(box.data() / "graph.db"));
    }
    {
        const Sandbox box;
        fs::create_directories(box.data());
        std::ofstream(box.data() / "graph.db", std::ios::binary) << "not a database at all";
        const auto result = box.run({"__complete", "note"});
        CHECK(result.exit_code == 0);
        CHECK(result.out.empty());
        CHECK(result.err.empty());
        CHECK(slurp(box.data() / "graph.db") == "not a database at all");
    }
}

TEST_CASE("completion candidates carry no control characters: one candidate is one safe line") {
    using noteapp::cli::is_completion_safe;
    CHECK(is_completion_safe("iş/plan toplantı.md"));
    CHECK_FALSE(is_completion_safe("a\nb"));
    CHECK_FALSE(is_completion_safe("a\tb"));
    CHECK_FALSE(is_completion_safe("a\rb"));
    CHECK_FALSE(is_completion_safe("a\x1b]52;c;x\x07"));
    CHECK_FALSE(is_completion_safe("a\xC2\x9B"));  // C1 CSI
    CHECK_FALSE(is_completion_safe("a\xFF"));      // geçersiz UTF-8
    CHECK_FALSE(is_completion_safe(""));
}
