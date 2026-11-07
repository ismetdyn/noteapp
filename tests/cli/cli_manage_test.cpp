// note rm / note mv (T14). Silme yolu 🔴: yanlış dosya silinmemeli, onaysız silinmemeli.
#include "cli/confirm.hpp"
#include "core/i18n.hpp"
#include "support/cli_sandbox.hpp"
#include "support/pty_run.hpp"

#include <doctest/doctest.h>
#include <fstream>

using namespace noteapp::test;

namespace {

void put(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

}  // namespace

TEST_CASE("rm -y deletes exactly the named note") {
    const Sandbox box;
    box.put("a.md", "a");
    box.put("a-2.md", "a2");

    const auto result = box.run({"rm", "a.md", "-y"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "Silindi: a.md\n");
    CHECK_FALSE(box.exists("a.md"));
    CHECK(box.body("a-2.md") == "a2");
}

TEST_CASE("rm without -y refuses when stdin is not a terminal") {
    const Sandbox box;
    box.put("a.md", "a");

    const auto result = box.run({"rm", "a.md"});
    CHECK(result.exit_code == 1);
    CHECK(result.err.find("-y") != std::string::npos);
    CHECK(box.body("a.md") == "a");
}

// Windows'ta _isatty NUL aygıtını TTY sayar; betikten `< NUL` ile çağrı soru sormadan reddedilmeli.
TEST_CASE("rm without -y refuses without prompting when stdin is the null device") {
    const Sandbox box;
    box.put("a.md", "a");

    const auto result = box.run_null_stdin({"rm", "a.md"});
    CHECK(result.exit_code == 1);
    CHECK(result.err.find("-y") != std::string::npos);
    CHECK(result.err.find("[e/H]") == std::string::npos);
    CHECK(box.body("a.md") == "a");
}

TEST_CASE("rm piped 'yes' on stdin is not a confirmation") {
    const Sandbox box;
    box.put("a.md", "a");

    CHECK(box.run({"rm", "a.md"}, "e\n").exit_code == 1);
    CHECK(box.exists("a.md"));
}

TEST_CASE("rm takes exact paths only: a fuzzy query deletes nothing") {
    const Sandbox box;
    box.put("work/plan.md", "p");

    for (const char* query : {"plan", "plan.md", "work/pla", "wrk/plan.md"}) {
        CAPTURE(query);
        const auto result = box.run({"rm", query, "-y"});
        CHECK(result.exit_code == 1);
    }
    CHECK(box.body("work/plan.md") == "p");
}

TEST_CASE("rm rejects invalid paths (exit 2) and never touches outside the notes folder") {
    const Sandbox box;
    box.put("a.md", "a");
    put(box.notes().parent_path() / "outside.md", "gizli");

    CHECK(box.run({"rm", "../outside.md", "-y"}).exit_code == 2);
    CHECK(slurp(box.notes().parent_path() / "outside.md") == "gizli");
    CHECK(box.run({"rm", "work", "-y"}).exit_code == 1);  // klasör değil, not yolu: bulunamadı
    CHECK(box.run({"rm"}).exit_code == 2);
    CHECK(box.run({"rm", "a.md", "b.md", "-y"}).exit_code == 2);
    CHECK(box.run({"rm", "a.md", "--zorla"}).exit_code == 2);
    CHECK(box.exists("a.md"));
}

TEST_CASE("rm on a folder path does not delete the folder") {
    const Sandbox box;
    box.put("d/x.md", "x");

    CHECK(box.run({"rm", "d", "-y"}).exit_code != 0);
    CHECK(box.body("d/x.md") == "x");
}

TEST_CASE("mv changes the type with the extension and creates target folders") {
    const Sandbox box;
    box.put("a.md", "class A {}");

    const auto retyped = box.run({"mv", "a.md", "a.java"});
    CHECK(retyped.exit_code == 0);
    CHECK(retyped.out == "Taşındı: a.md -> a.java\n");
    CHECK_FALSE(box.exists("a.md"));
    CHECK(box.body("a.java") == "class A {}");

    CHECK(box.run({"mv", "a.java", "work/2026/b.java"}).exit_code == 0);
    CHECK(box.body("work/2026/b.java") == "class A {}");
}

TEST_CASE("mv never overwrites an existing target and exits 1") {
    const Sandbox box;
    box.put("a.md", "kaynak");
    box.put("b.md", "hedef");

    const auto result = box.run({"mv", "a.md", "b.md"});
    CHECK(result.exit_code == 1);
    CHECK(result.err.find("b.md") != std::string::npos);
    CHECK(box.body("a.md") == "kaynak");
    CHECK(box.body("b.md") == "hedef");
}

TEST_CASE("mv errors: missing source 1, invalid path 2, wrong argument count 2") {
    const Sandbox box;
    box.put("a.md", "a");

    CHECK(box.run({"mv", "yok.md", "b.md"}).exit_code == 1);
    CHECK(box.run({"mv", "a.md", "../kacis.md"}).exit_code == 2);
    CHECK(box.run({"mv", "a.md"}).exit_code == 2);
    CHECK(box.run({"mv", "a.md", "b.md", "c.md"}).exit_code == 2);
    CHECK(box.body("a.md") == "a");
    CHECK_FALSE(fs::exists(box.notes().parent_path() / "kacis.md"));
}

TEST_CASE("rm and mv messages follow the language") {
    const Sandbox box("en");
    box.put("a.md", "a");

    CHECK(box.run({"mv", "a.md", "b.md"}).out == "Moved: a.md -> b.md\n");
    CHECK(box.run({"rm", "b.md"}).err.find("requires -y") != std::string::npos);
    CHECK(box.run({"rm", "b.md", "-y"}).out == "Deleted: b.md\n");
}

TEST_CASE("is_delete_confirmed accepts only the localized yes letters") {
    using noteapp::cli::is_delete_confirmed;
    noteapp::core::init_i18n("tr");
    for (const char* yes : {"e", "E", " e\n", "E\r\n"}) {
        CAPTURE(yes);
        CHECK(is_delete_confirmed(yes));
    }
    for (const char* no : {"", "\n", "y", "Y", "h", "n", "evet", "ee", "e x"}) {
        CAPTURE(no);
        CHECK_FALSE(is_delete_confirmed(no));
    }

    noteapp::core::init_i18n("en");
    CHECK(is_delete_confirmed("y"));
    CHECK(is_delete_confirmed("Y\n"));
    CHECK_FALSE(is_delete_confirmed("e"));
    CHECK_FALSE(is_delete_confirmed("yes"));
    CHECK_FALSE(is_delete_confirmed(""));
}

#ifdef __linux__
// SD11b review: onay sorusu açıkken senkron notu değiştirirse `rm` sorulan hâli siler, yenisini değil.
TEST_CASE("rm keeps a note that changed while the confirmation was open") {
    const Sandbox box;
    box.put("a.md", "a");
    const auto result = run_in_pty(
        NOTEAPP_NOTE_EXE, {"rm", "a.md"},
        {.input = "e\n", .input_after = "[e/H]", .before_input = [&] { box.put("a.md", "synced"); }});
    CHECK(result.exit_code != 0);
    CHECK(result.terminal.find("onaydan sonra") != std::string::npos);
    CHECK(box.body("a.md") == "synced");

    // Değişmeyen not soruyla silinir.
    const auto confirmed =
        run_in_pty(NOTEAPP_NOTE_EXE, {"rm", "a.md"}, {.input = "e\n", .input_after = "[e/H]"});
    CHECK(confirmed.exit_code == 0);
    CHECK_FALSE(box.exists("a.md"));
}
#endif
