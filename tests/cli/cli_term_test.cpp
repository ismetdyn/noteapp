// Terminal davranışı: renk politikası, pager, ilerleme satırı, TTY'ye özgü küçükler (SPEC-cli "Terminal").
#include "support/cli_sandbox.hpp"
#include "support/pty_run.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <vector>
#include <tuple>

using namespace noteapp::test;

namespace {

constexpr std::string_view kMatchOn = "\x1b[1;33m";

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("NOTEAPP_COLOR=always colors find matches even in a pipe; the default pipe output stays plain") {
    const Sandbox box("en");
    box.put("hello-world.md", "hello world\n");
    ScopedEnv always("NOTEAPP_COLOR", std::string("always"));
    const auto colored = box.run({"find", "hello"});
    CHECK(colored.exit_code == 0);
    CHECK(contains(colored.out, kMatchOn));

    ScopedEnv never("NOTEAPP_COLOR", std::string("never"));
    CHECK(box.run({"find", "hello"}).out == "hello-world.md:1: hello world\n");
}

#ifdef __linux__

TEST_CASE("find colors matches on a terminal unless NO_COLOR, TERM=dumb or NOTEAPP_COLOR=never") {
    const Sandbox box("en");
    box.put("hello-world.md", "hello world\n");
    CHECK(contains(run_in_pty(NOTEAPP_NOTE_EXE, {"find", "hello"}).terminal, kMatchOn));

    const std::vector<PtyOptions> plain{
        {.env = {{"NO_COLOR", "1"}}},
        {.env = {{"TERM", "dumb"}}},
        {.env = {{"NOTEAPP_COLOR", "never"}}},
    };
    for (const PtyOptions& options : plain) {
        CAPTURE(options.env.front().first);
        const auto result = run_in_pty(NOTEAPP_NOTE_EXE, {"find", "hello"}, options);
        CHECK(result.exit_code == 0);
        CHECK(contains(result.terminal, "hello world"));
        CHECK_FALSE(contains(result.terminal, "\x1b"));
    }
    // Kullanıcının açık seçimi NO_COLOR'dan üstündür.
    const auto forced =
        run_in_pty(NOTEAPP_NOTE_EXE, {"find", "hello"}, {.env = {{"NO_COLOR", "1"}, {"NOTEAPP_COLOR", "always"}}});
    CHECK(contains(forced.terminal, kMatchOn));
}

TEST_CASE("a quick add that looks like a mistyped command still saves and hints on a terminal") {
    const Sandbox box("en");
    const auto typed = run_in_pty(NOTEAPP_NOTE_EXE, {"lss"});
    CHECK(typed.exit_code == 0);
    CHECK(box.body("lss.md") == "lss");
    CHECK(contains(typed.terminal, "Saved: lss.md (md, suggested)\r\n"));
    CHECK(contains(typed.terminal,
                   "  hint: 'lss' was saved as a note. Meant 'note ls'? Undo: note rm lss.md\r\n"));

    // stderr terminal değilse ipucu yok; stdout aynı.
    const Sandbox piped("en");
    const auto redirected = run_in_pty(NOTEAPP_NOTE_EXE, {"shw"}, {.stderr_to_file = true});
    CHECK(redirected.exit_code == 0);
    CHECK(contains(redirected.terminal, "Saved: shw.md (md, suggested)"));
    CHECK(redirected.err.empty());

    // Birden çok sözcük ya da yakın komut yoksa ipucu yok.
    for (const std::vector<std::string>& args : {std::vector<std::string>{"lss", "x"},
                                                 std::vector<std::string>{"merhaba"}}) {
        const Sandbox other("en");
        const auto result = run_in_pty(NOTEAPP_NOTE_EXE, args);
        CHECK(result.exit_code == 0);
        CHECK_FALSE(contains(result.terminal, "hint:"));
    }
}

namespace {

// Sahte pager: her satırın başına işaret koyar; çıktının pager'dan geçtiği böyle görünür.
const std::string kMarkingPager = "sed -e s/^/P:/";

void put_notes(const Sandbox& box, int count) {
    for (int i = 0; i < count; ++i)
        box.put("n" + std::to_string(i) + ".md", "x\n", i);
}

}  // namespace

TEST_CASE("long output on a terminal goes through PAGER; short output does not") {
    const Sandbox box("en");
    put_notes(box, 20);
    const auto paged = run_in_pty(NOTEAPP_NOTE_EXE, {"ls"}, {.env = {{"PAGER", kMarkingPager}}, .rows = 10});
    CHECK(paged.exit_code == 0);
    CHECK(contains(paged.terminal, "P:n0.md"));
    CHECK(contains(paged.terminal, "P:n19.md"));

    const auto fits = run_in_pty(NOTEAPP_NOTE_EXE, {"ls"}, {.env = {{"PAGER", kMarkingPager}}, .rows = 40});
    CHECK(fits.exit_code == 0);
    CHECK(contains(fits.terminal, "n19.md"));
    CHECK_FALSE(contains(fits.terminal, "P:"));

    // NOTEAPP_PAGER PAGER'dan üstündür.
    const auto own = run_in_pty(NOTEAPP_NOTE_EXE, {"ls"},
                                {.env = {{"PAGER", kMarkingPager}, {"NOTEAPP_PAGER", "sed -e s/^/Q:/"}}, .rows = 10});
    CHECK(contains(own.terminal, "Q:n0.md"));
    CHECK_FALSE(contains(own.terminal, "P:"));

    // Yardım da sayfalanır.
    CHECK(contains(run_in_pty(NOTEAPP_NOTE_EXE, {"help"}, {.env = {{"PAGER", kMarkingPager}}, .rows = 10}).terminal,
                   "P:"));
}

TEST_CASE("NOTEAPP_PAGER=cat, --no-pager and a missing pager print plainly") {
    const Sandbox box("en");
    put_notes(box, 20);
    const std::vector<std::pair<std::vector<std::string>, PtyOptions>> plain{
        {{"ls"}, {.env = {{"PAGER", kMarkingPager}, {"NOTEAPP_PAGER", "cat"}}, .rows = 10}},
        {{"ls", "--no-pager"}, {.env = {{"PAGER", kMarkingPager}}, .rows = 10}},
        {{"help", "--no-pager"}, {.env = {{"PAGER", kMarkingPager}}, .rows = 10}},
        {{"help", "--no-pager", "ls"}, {.env = {{"PAGER", kMarkingPager}}, .rows = 10}},
        {{"ls"}, {.env = {{"PAGER", "/nonexistent/pager"}}, .rows = 10}},
    };
    for (const auto& [args, options] : plain) {
        CAPTURE(args.back());
        const auto result = run_in_pty(NOTEAPP_NOTE_EXE, args, options);
        CHECK(result.exit_code == 0);
        CHECK_FALSE(contains(result.terminal, "P:"));
        CHECK(contains(result.terminal, args.front() == "ls" ? "n19.md" : "Usage:"));
    }
}

TEST_CASE("show ends with a newline on a terminal so the prompt starts on its own line") {
    const Sandbox box("en");
    box.put("raw.md", "hello");
    CHECK(contains(run_in_pty(NOTEAPP_NOTE_EXE, {"show", "raw.md"}).terminal, "hello\r\n"));
    box.put("ends.md", "line\n");
    CHECK_FALSE(contains(run_in_pty(NOTEAPP_NOTE_EXE, {"show", "ends.md"}).terminal, "line\r\n\r\n"));
    CHECK(box.run({"show", "raw.md"}).out == "hello");  // boru: bayt bayt
}

TEST_CASE("a refused rm confirmation says how to delete") {
    for (const auto& [language, answer, hint] :
         {std::tuple{"en", "yes\n", "  hint: answer 'y' to delete, or pass -y (--yes)"},
          std::tuple{"tr", "evet\n", "  ipucu: silmek için 'e' yazın ya da -y (--yes) verin"}}) {
        CAPTURE(language);
        const Sandbox box(language);
        box.put("a.md", "a\n");
        const auto result = run_in_pty(NOTEAPP_NOTE_EXE, {"rm", "a.md"}, {.input = answer});
        CHECK(result.exit_code == 1);
        CHECK(contains(result.terminal, hint));
        CHECK(box.exists("a.md"));
    }
}

TEST_CASE("tags lines up the counts on a terminal and stays TSV in a pipe") {
    const Sandbox box("en");
    box.put("a.md", "#ev\n");
    box.put("b.md", "#çalışma #ev\n");
    const std::string terminal = run_in_pty(NOTEAPP_NOTE_EXE, {"tags"}).terminal;
    CHECK(contains(terminal, "ev       2\r\n"));
    CHECK(contains(terminal, "çalışma  1\r\n"));
    CHECK(box.run({"tags"}).out == "ev\t2\nçalışma\t1\n");
}

TEST_CASE("sync and import show a progress line on a terminal and clear it when done") {
    constexpr std::string_view kClear = "\r\x1b[K";
    const Sandbox box("en");
    const auto synced = run_in_pty(NOTEAPP_NOTE_EXE, {"sync"});
    CHECK(synced.exit_code == 4);  // uzak depo yok: bugünkü çıkış kodu
    CHECK(contains(synced.terminal, std::string("Syncing...") + std::string(kClear)));

    const TempDir source;
    {
        std::ofstream(source.path() / "a.txt", std::ios::binary) << "a";
        std::ofstream(source.path() / "b.txt", std::ios::binary) << "b";
    }
    // Sayaç app::import_paths'in ilerleme geri çağrısından: her girdi aynı satırı yeniler.
    const auto imported = run_in_pty(
        NOTEAPP_NOTE_EXE, {"import", utf8(source.path() / "a.txt"), utf8(source.path() / "b.txt")});
    CHECK(imported.exit_code == 0);
    CHECK(contains(imported.terminal, std::string("Importing 1/2...") + std::string(kClear) +
                                          "Importing 2/2..." + std::string(kClear)));

    // stderr terminal değilse ya da TERM=dumb ise ara çıktı yok.
    for (const PtyOptions& options : {PtyOptions{.stderr_to_file = true}, PtyOptions{.env = {{"TERM", "dumb"}}}}) {
        const auto quiet = run_in_pty(NOTEAPP_NOTE_EXE, {"sync"}, options);
        CHECK(quiet.exit_code == 4);
        CHECK_FALSE(contains(quiet.terminal + quiet.err, "Syncing"));
        CHECK_FALSE(contains(quiet.terminal + quiet.err, kClear));
    }
}

TEST_CASE("Ctrl-C while the pager runs does not kill note; the pager finishes and note exits 0") {
    const Sandbox box("en");
    put_notes(box, 20);
    // less gibi SIGINT'i yok sayan sahte pager: hazır olduğunu yazar, Ctrl-C'yi bekler, sonra içeriği basar.
    const std::string pager = "sh -c \"trap '' INT; echo PAGER-READY; sleep 1; sed -e s/^/P:/\"";
    const auto result = run_in_pty(NOTEAPP_NOTE_EXE, {"ls"},
                                   {.env = {{"PAGER", pager}}, .input = "\x03", .input_after = "PAGER-READY", .rows = 10});
    CHECK(result.exit_code == 0);  // SIGINT ile ölseydi WIFEXITED yanlış olurdu (-1)
    CHECK(contains(result.terminal, "P:n19.md"));
}

TEST_CASE("note does not set LESS for the pager; the user's environment reaches it unchanged") {
    const Sandbox box("en");
    put_notes(box, 20);
    const std::string pager = "sh -c 'echo \"LESS=[$LESS]\"; cat'";
    CHECK(contains(run_in_pty(NOTEAPP_NOTE_EXE, {"ls"}, {.env = {{"PAGER", pager}}, .rows = 10}).terminal,
                   "LESS=[]"));
    CHECK(contains(
        run_in_pty(NOTEAPP_NOTE_EXE, {"ls"}, {.env = {{"PAGER", pager}, {"LESS", "-S"}}, .rows = 10}).terminal,
        "LESS=[-S]"));
}

TEST_CASE("an empty NO_COLOR does not turn color off (no-color.org)") {
    const Sandbox box("en");
    box.put("hello-world.md", "hello world\n");
    CHECK(contains(run_in_pty(NOTEAPP_NOTE_EXE, {"find", "hello"}, {.env = {{"NO_COLOR", ""}}}).terminal, kMatchOn));
}

#endif  // __linux__

TEST_CASE("a pipe never starts the pager") {
    const Sandbox box("en");
    for (int i = 0; i < 50; ++i)
        box.put("n" + std::to_string(i) + ".md", "x\n", i);
    const std::string expected = box.run({"ls"}).out;
    ScopedEnv pager("PAGER", std::string("sed -e s/^/P:/"));
    ScopedEnv lines("LINES", std::string("5"));
    CHECK(box.run({"ls"}).out == expected);
    CHECK(box.run({"ls", "--no-pager"}).out == expected);
    CHECK_FALSE(contains(box.run({"help"}).out, "P:"));
}

TEST_CASE("a mistyped quick add in a pipe prints exactly what it printed before") {
    const Sandbox box("en");
    const auto result = box.run({"lss"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "Saved: lss.md (md, suggested)\n");
    CHECK(result.err.empty());
}
