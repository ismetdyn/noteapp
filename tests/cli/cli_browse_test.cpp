// note ls / show / edit (T15): en iyi eşleşme kuralı, sıralama ve çıktı sözleşmesi.
#include "support/cli_sandbox.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <optional>
#include <sstream>

using namespace noteapp::test;

namespace {

// `age_seconds`: dosyanın mtime'ı şimdiden bu kadar eski olur (sıralama testleri deterministik kalsın).
void put(const fs::path& file, const std::string& bytes, int age_seconds = 0) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::seconds(age_seconds));
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

class EditorEnv {
public:
    EditorEnv() : editor_("EDITOR", std::string(NOTEAPP_FAKE_EDITOR)), visual_("VISUAL", std::nullopt) {}

private:
    ScopedEnv editor_;
    ScopedEnv visual_;
};

}  // namespace

TEST_CASE("ls without a query lists relative paths by mtime descending") {
    const Sandbox box;
    put(box.notes() / "old.md", "o", 300);
    put(box.notes() / "work" / "mid.txt", "m", 200);
    put(box.notes() / "new.md", "n", 100);

    const auto result = box.run({"ls"});
    CHECK(result.exit_code == 0);
    CHECK(lines_of(result.out) == std::vector<std::string>{"new.md", "work/mid.txt", "old.md"});
}

TEST_CASE("ls with no notes prints nothing and exits 0") {
    const Sandbox box;
    const auto result = box.run({"ls"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.empty());
}

TEST_CASE("ls with a query follows rank_notes order and drops non-matches (SPEC-cli acceptance 6)") {
    const Sandbox box;
    put(box.notes() / "my-long-text-generator.md", "x");
    put(box.notes() / "meeting.md", "x");
    put(box.notes() / "unrelated.md", "x");

    const auto result = box.run({"ls", "mtg"});
    CHECK(result.exit_code == 0);
    CHECK(lines_of(result.out) == std::vector<std::string>{"my-long-text-generator.md", "meeting.md"});
}

TEST_CASE("ls folds Turkish letters: ogrenci finds Öğrenci.md") {
    const Sandbox box;
    put(box.notes() / "Öğrenci.md", "x");
    put(box.notes() / "baska.md", "x");

    const auto result = box.run({"ls", "ogrenci"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "Öğrenci.md\n");
}

TEST_CASE("ls -f restricts to the folder subtree and validates the folder") {
    const Sandbox box;
    put(box.notes() / "work" / "a.md", "a", 30);
    put(box.notes() / "work" / "2026" / "b.md", "b", 20);
    put(box.notes() / "workshop.md", "w", 10);
    put(box.notes() / "other" / "c.md", "c", 5);

    const auto scoped = box.run({"ls", "-f", "work"});
    CHECK(scoped.exit_code == 0);
    CHECK(lines_of(scoped.out) == std::vector<std::string>{"work/2026/b.md", "work/a.md"});

    const auto both = box.run({"ls", "b", "-f", "work"});
    CHECK(both.out == "work/2026/b.md\n");

    CHECK(box.run({"ls", "-f", "../x"}).exit_code == 2);
    CHECK(box.run({"ls", "-f"}).exit_code == 2);
    CHECK(box.run({"ls", "--zorla"}).exit_code == 2);
    CHECK(box.run({"ls", "a", "b"}).exit_code == 2);
}

TEST_CASE("show prints the raw content byte-exact and names the chosen path on stderr") {
    const Sandbox box;
    const std::string raw = "satır 1\r\nçğ\n\n";
    put(box.notes() / "work" / "plan.md", raw);

    const auto result = box.run({"show", "pln"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == raw);
    CHECK(result.err == "Seçilen: work/plan.md\n");
}

TEST_CASE("an exact relative path beats a better fuzzy score and prints no chosen line") {
    const Sandbox box;
    put(box.notes() / "plan.md", "tam");
    put(box.notes() / "work" / "plan.md.md", "bulanik");
    put(box.notes() / "work" / "plan.md", "hedef");

    const auto exact = box.run({"show", "work/plan.md"});
    CHECK(exact.exit_code == 0);
    CHECK(exact.out == "hedef");
    CHECK(exact.err.empty());
}

TEST_CASE("no match exits 1 for show and edit; bad usage exits 2") {
    const Sandbox box;
    const EditorEnv env;
    put(box.notes() / "a.md", "a");

    const auto shown = box.run({"show", "zzzz"});
    CHECK(shown.exit_code == 1);
    CHECK(shown.out.empty());
    CHECK(shown.err.find("zzzz") != std::string::npos);
    CHECK(box.run({"edit", "zzzz"}).exit_code == 1);
    CHECK(box.run({"show"}).exit_code == 2);
    CHECK(box.run({"edit", "a", "b"}).exit_code == 2);
}

TEST_CASE("show refuses a binary note with exit 1") {
    const Sandbox box;
    put(box.notes() / "blob.bin", std::string("ab\0cd", 5));

    const auto result = box.run({"show", "blob.bin"});
    CHECK(result.exit_code == 1);
    CHECK(result.out.empty());
}

TEST_CASE("edit opens the best match in the editor and the file changes") {
    const Sandbox box;
    const EditorEnv env;
    const ScopedEnv content("FAKE_EDITOR_CONTENT", "yeni içerik");
    put(box.notes() / "work" / "plan.md", "eski");

    const auto result = box.run({"edit", "plan"});
    CHECK(result.exit_code == 0);
    CHECK(result.err == "Seçilen: work/plan.md\n");
    CHECK(slurp(box.notes() / "work" / "plan.md") == "yeni içerik");
}

TEST_CASE("edit maps editor failures to exit 3 and leaves the note alone") {
    const Sandbox box;
    const EditorEnv env;
    const ScopedEnv exit_code("FAKE_EDITOR_EXIT", "7");
    put(box.notes() / "a.md", "a");

    CHECK(box.run({"edit", "a.md"}).exit_code == 3);
    CHECK(slurp(box.notes() / "a.md") == "a");
}

TEST_CASE("ls and show messages follow the language") {
    const Sandbox box("en");
    put(box.notes() / "a.md", "a");

    CHECK(box.run({"show", "a"}).err == "Chosen: a.md\n");
    CHECK(box.run({"show", "zzz"}).err.find("No note matches") != std::string::npos);
}
