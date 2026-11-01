#include "core/i18n.hpp"
#include "detect/detect.hpp"
#include "graph/graph.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_session.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

using namespace noteapp;
using tui::EditorSession;
using tui::Key;
namespace fs = std::filesystem;

namespace {

int plain_width(char32_t cp) {
    return cp >= 0x0300 && cp <= 0x036F ? 0 : 1;
}

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::size_t kTestLimit = 1024;
constexpr double kMinConfidence = 0.6;
constexpr std::size_t kScanBytes = 64 * 1024;
constexpr std::size_t kRoomyLimit = 1024 * 1024;  // boyut dışındaki salt-okunur nedenleri için

struct Fixture {
    explicit Fixture(std::uintmax_t max_bytes = kTestLimit) : limit(max_bytes) {
        core::init_i18n("en");
        session.layout(40, 10);
    }

    std::uintmax_t limit;
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    std::vector<std::string> saved;
    std::vector<bool> active;
    std::vector<core::Error> errors;
    int settings_opened = 0;
    std::optional<std::size_t> closed_line;
    EditorSession session{tui::EditorHost{
        store, mutex, clipboard, plain_width, limit, core::kDefaultSlugMaxLength,
        [](std::string_view content) {
            return detect::suggest(content, detect::Options{"md", kMinConfidence, kScanBytes}).type;
        },
        [this](const core::NotePath& path) { saved.push_back(path.string()); },
        [this] { active.push_back(true); },
        [this](std::optional<std::size_t> line) {
            active.push_back(false);
            closed_line = line;
        },
        [this](const core::Error& error) { errors.push_back(error); }, [this] { ++settings_opened; }}};

    fs::path file(const std::string& name) const { return dir.path() / name; }
    void put(const std::string& name, std::string_view bytes) const {
        test::upsert_note(dir.path(), name, bytes);
    }
    std::string bytes(const std::string& name) const { return test::note_body(dir.path(), name); }
    bool exists(const std::string& name) const { return test::note_exists(dir.path(), name); }
    void erase(const std::string& name) const { test::erase_note(dir.path(), name); }
    core::Revision revision(const std::string& name) const { return test::note_revision(dir.path(), name); }
    void open(const std::string& name) { session.open(core::NotePath::parse(name).value()); }
    void open_with(const std::string& name, std::string_view content) {
        put(name, content);
        open(name);
        REQUIRE(session.is_open());
    }
    void type(std::string_view text) {
        for (std::size_t i = 0; i < text.size();) {
            std::size_t length = 1;
            const auto lead = static_cast<unsigned char>(text[i]);
            if (lead >= 0xF0)
                length = 4;
            else if (lead >= 0xE0)
                length = 3;
            else if (lead >= 0xC0)
                length = 2;
            if (text[i] == '\n')
                session.handle(Key::Enter);
            else
                session.handle(Key::Char, text.substr(i, length));
            i += length;
        }
    }
    // Dış değişiklik: ayrı bağlantı, başka bir süreç gibi yazar.
    void change_externally(const std::string& name, std::string_view content) const { put(name, content); }
    std::string text() const { return session.buffer()->text(); }
    void clear_name() {
        for (std::size_t bytes_left = session.name_input().size(); bytes_left > 0; --bytes_left)
            session.handle(Key::Backspace);
    }
    void rescan() { session.on_listing(store.list()); }
    void press_leave_choice(EditorSession::LeaveChoice choice) {
        // Sınırlı: soru açılmadıysa test sonsuza dek dönmesin, başarısız olsun.
        for (int i = 0; i < 3 && session.leave_choice() != choice; ++i)
            session.handle(Key::Right);
        REQUIRE(session.mode() == EditorSession::Mode::LeavePrompt);
        session.handle(Key::Enter);
    }
    void press_conflict_choice(EditorSession::ConflictChoice choice) {
        for (int i = 0; i < 4 && session.conflict_choice() != choice; ++i)
            session.handle(Key::Down);
        REQUIRE(session.mode() == EditorSession::Mode::ConflictPrompt);
        session.handle(Key::Enter);
    }
};

}  // namespace

TEST_CASE("session: opening a note activates the editor, a missing note reports the error") {
    Fixture f;
    f.open("missing.md");
    CHECK_FALSE(f.session.is_open());
    CHECK(f.errors.size() == 1);
    CHECK(f.active.empty());
    f.open_with("a.md", "metin");
    CHECK(f.active == std::vector<bool>{true});
    CHECK(f.text() == "metin");
    CHECK_FALSE(f.session.dirty());
}

TEST_CASE("session: letters that are list commands go into the text and never act") {
    Fixture f;
    f.open_with("a.md", "");
    for (const char* letter : {"a", "n", "d", "r", "s", "q", "?", "/", "j", "k", "g", "G", "l", "h"})
        f.session.handle(Key::Char, letter);
    CHECK(f.text() == "andrsq?/jkgGlh");
    CHECK(f.session.is_open());
    CHECK(f.exists("a.md"));
    CHECK(f.saved.empty());
}

TEST_CASE("session: Turkish letters type into the buffer and control text is refused") {
    Fixture f;
    f.open_with("a.md", "");
    f.type("ığüşöçİĞ");
    f.session.handle(Key::Char, "\x1b");
    f.session.handle(Key::Char, "\xFF");
    CHECK(f.text() == "ığüşöçİĞ");
}

TEST_CASE("session: every editing key reaches the buffer") {
    Fixture f;
    f.open_with("a.md", "ab\ncd");
    f.session.handle(Key::Right);
    CHECK(f.session.buffer()->cursor() == 1);
    f.session.handle(Key::Left);
    CHECK(f.session.buffer()->cursor() == 0);
    f.session.handle(Key::End);
    CHECK(f.session.buffer()->cursor() == 2);
    f.session.handle(Key::Home);
    f.session.handle(Key::Down);
    CHECK(f.session.buffer()->position().line == 1);
    f.session.handle(Key::Up);
    f.session.handle(Key::CtrlEnd);
    CHECK(f.session.buffer()->cursor() == 5);
    f.session.handle(Key::CtrlHome);
    CHECK(f.session.buffer()->cursor() == 0);
    f.session.handle(Key::ShiftRight);
    CHECK(f.session.buffer()->selection().has_value());
    f.session.handle(Key::ShiftLeft);
    CHECK_FALSE(f.session.buffer()->selection().has_value());
    f.session.handle(Key::ShiftEnd);
    CHECK(f.session.buffer()->selection()->end == 2);
    f.session.handle(Key::Delete);
    CHECK(f.text() == "\ncd");
    f.session.handle(Key::CtrlZ);
    CHECK(f.text() == "ab\ncd");
    f.session.handle(Key::CtrlY);
    CHECK(f.text() == "\ncd");
    f.session.handle(Key::Enter);
    CHECK(f.text() == "\n\ncd");
    f.session.handle(Key::Tab);
    CHECK(f.text() == "\n\t\ncd");
    f.session.handle(Key::Backspace);
    CHECK(f.text() == "\n\ncd");
}

TEST_CASE("session: left arrow never leaves the editor, even at the start of the text") {
    Fixture f;
    f.open_with("a.md", "x");
    f.session.handle(Key::Left);
    f.session.handle(Key::Left);
    CHECK(f.session.is_open());
    CHECK(f.active == std::vector<bool>{true});
}

TEST_CASE("session: Ctrl+X, Ctrl+C and Ctrl+V use the line without a selection and survive reopening") {
    Fixture f;
    f.open_with("a.md", "bir\niki\n");
    f.session.handle(Key::CtrlC);
    CHECK(f.clipboard == "bir\n");
    CHECK(f.session.is_open());  // Ctrl+C kopyalar, çıkmaz
    f.session.handle(Key::Down);
    f.session.handle(Key::CtrlX);
    CHECK(f.clipboard == "iki\n");
    CHECK(f.text() == "bir\n");
    f.session.handle(Key::CtrlHome);
    f.session.handle(Key::CtrlV);
    CHECK(f.text() == "iki\nbir\n");
    f.session.handle(Key::CtrlZ);
    f.session.handle(Key::CtrlS);
    f.session.handle(Key::CtrlQ);
    f.open_with("b.md", "");
    f.session.handle(Key::CtrlV);
    CHECK(f.text() == "iki\n");
}

TEST_CASE("session: Ctrl+F finds with folding; Enter, F3, Up, Down and Ctrl+N/P move; letters stay out of "
          "the text") {
    Fixture f;
    f.open_with("a.md", "Ağaç bir AĞAÇ");
    f.session.handle(Key::CtrlF);
    CHECK(f.session.mode() == EditorSession::Mode::Find);
    f.type("ağaç");
    CHECK(f.session.find_query() == "ağaç");
    CHECK(f.session.find_matches().size() == 2);
    CHECK(f.session.buffer()->selection()->begin == 0);
    CHECK(f.session.find_label() == "1/2");
    CHECK(f.text() == "Ağaç bir AĞAÇ");  // arama kutusundaki harf metne girmez
    f.session.handle(Key::Enter);
    CHECK(f.session.buffer()->selection()->begin == 11);
    CHECK(f.session.find_label() == "2/2");
    f.session.handle(Key::F3);
    CHECK(f.session.buffer()->selection()->begin == 0);
    f.session.handle(Key::CtrlP);
    CHECK(f.session.buffer()->selection()->begin == 11);
    f.session.handle(Key::CtrlN);
    CHECK(f.session.buffer()->selection()->begin == 0);
    f.session.handle(Key::Up);
    CHECK(f.session.buffer()->selection()->begin == 11);
    f.session.handle(Key::Down);
    CHECK(f.session.buffer()->selection()->begin == 0);
    f.session.handle(Key::Backspace);
    CHECK(f.session.find_query() == "ağa");
    f.session.handle(Key::Escape);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.session.find_matches().empty());
    CHECK(f.session.is_open());
    f.session.handle(Key::Char, "x");
    CHECK(f.session.dirty());
}

TEST_CASE("session: find folds Turkish: I finds i, dotless i and dotted capital") {
    Fixture f;
    f.open_with("a.md", "i ı I İ");
    f.session.handle(Key::CtrlF);
    f.session.handle(Key::Char, "I");
    CHECK(f.session.find_matches().size() == 4);
}

TEST_CASE("session: find starts from the cursor and wraps") {
    Fixture f;
    f.open_with("a.md", "bir\nbir\nbir");
    f.session.handle(Key::Down);
    f.session.handle(Key::CtrlF);
    f.type("bir");
    CHECK(f.session.buffer()->selection()->begin == 4);
    CHECK(f.session.find_label() == "2/3");
}

TEST_CASE("session: editing clears matches and F3 recomputes them from the cursor") {
    Fixture f;
    f.open_with("a.md", "kök kök kök");
    f.session.handle(Key::CtrlF);
    f.type("kök");
    f.session.handle(Key::Enter);
    f.session.handle(Key::Enter);  // son eşleşmede
    REQUIRE(f.session.find_matches().size() == 3);
    f.session.handle(Key::Escape);
    CHECK(f.session.find_matches().empty());
    f.session.handle(Key::CtrlHome);
    f.session.handle(Key::CtrlF);
    f.type("kök");
    REQUIRE(f.session.find_matches().size() == 3);
    f.session.handle(Key::Escape);
    f.session.handle(Key::Char, "x");  // ilk eşleşmenin yerine yazar: eski ofsetler geçersiz
    CHECK(f.session.find_matches().empty());
    f.session.handle(Key::F3);
    CHECK(f.session.find_matches().size() == 2);
    CHECK(f.session.buffer()->selection().has_value());
    CHECK(f.text().substr(f.session.buffer()->selection()->begin, 4) == "kök");
}

TEST_CASE("session: an edit right after F3 drops the highlights so no stale offset is drawn") {
    Fixture f;
    f.open_with("a.md", "kök kök");
    f.session.handle(Key::CtrlF);
    f.type("kök");
    f.session.handle(Key::Escape);
    f.session.handle(Key::F3);
    REQUIRE_FALSE(f.session.find_matches().empty());
    f.session.handle(Key::Backspace);
    CHECK(f.session.find_matches().empty());
    f.session.handle(Key::F3);
    REQUIRE_FALSE(f.session.find_matches().empty());
    f.session.handle(Key::CtrlZ);
    CHECK(f.session.find_matches().empty());
}

TEST_CASE("session: F3 with no earlier query does nothing") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.session.handle(Key::F3);
    CHECK_FALSE(f.session.buffer()->selection().has_value());
}

TEST_CASE("session: find reports no match in the status line") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.session.handle(Key::CtrlF);
    f.type("yok");
    CHECK_FALSE(f.session.message().empty());
    CHECK_FALSE(f.session.buffer()->selection().has_value());
    CHECK(f.session.find_label().empty());
}

TEST_CASE("session: open starts at the given line with it at the top of the window") {
    Fixture f;
    std::string text;
    for (int i = 0; i < 40; ++i)
        text += "satır\n";
    f.put("a.md", text);
    f.session.open(core::NotePath::parse("a.md").value(), 20);
    REQUIRE(f.session.is_open());
    f.session.layout(40, 10);
    CHECK(f.session.buffer()->position().line == 20);
    CHECK(f.session.buffer()->scroll_top().line == 20);
    f.session.handle(Key::CtrlQ);
    CHECK(f.closed_line == 20);  // Önizleme aynı yerden sürer
}

// ---- yeni not: ad istemi ve taslak ----

TEST_CASE("draft name: folder, extension and slug rules") {
    const auto now = std::chrono::system_clock::now();
    const auto parse = [&](std::string_view input) {
        return tui::parse_draft_name(input, "varsayılan", now, core::kDefaultSlugMaxLength);
    };
    auto name = parse("Toplantı Notları");
    REQUIRE(name);
    CHECK(name->folder == "varsayılan");
    CHECK(name->stem == core::slugify("Toplantı Notları", now));
    CHECK(name->type.empty());
    name = parse("iş/alt/Sıralama.java");
    REQUIRE(name);
    CHECK(name->folder == "iş/alt");
    CHECK(name->stem == core::slugify("Sıralama", now));
    CHECK(name->type == "java");
    name = parse("");
    REQUIRE(name);
    CHECK(name->timestamp_name);
    CHECK(name->stem.empty());
    name = parse("klasör/");
    REQUIRE(name);
    CHECK(name->folder == "klasör");
    CHECK(name->timestamp_name);
    CHECK_FALSE(parse("../dışarı"));
    CHECK_FALSE(parse("a/../b"));
}

TEST_CASE("draft name: a typed extension stays as typed, the stem keeps its dots") {
    const auto now = std::chrono::system_clock::now();
    const auto parse = [&](std::string_view input) {
        return tui::parse_draft_name(input, "", now, core::kDefaultSlugMaxLength);
    };
    const struct {
        std::string_view typed, stem, type;
    } cases[] = {
        {"deneme.cpp", "deneme", "cpp"},
        {"deneme.xyz", "deneme", "xyz"},
        {"a.tar.gz", "a.tar", "gz"},
        {"notlar.v2", "notlar", "v2"},
        {"Rapor.PDF", "rapor", "PDF"},
        {"Toplantı Notu.md", "toplanti-notu", "md"},
        {"sürüm 1.2.txt", "surum-1.2", "txt"},
        // Boşluklu son parça uzantı değildir: tip yazılmamış sayılır.
        {"v1.2 son", "v1.2-son", ""},
    };
    for (const auto& item : cases) {
        CAPTURE(item.typed);
        const auto name = parse(item.typed);
        REQUIRE(name);
        CHECK(name->stem == item.stem);
        CHECK(name->type == item.type);
    }
    // Gizli ad sessizce başka ada dönmez.
    CHECK_FALSE(parse(".env"));
}

TEST_CASE("n: the name prompt keeps a typed extension; the type and highlight follow it") {
    const struct {
        std::string_view typed, path;
        tui::SyntaxLanguage language;
    } cases[] = {
        {"deneme.cpp", "deneme.cpp", tui::SyntaxLanguage::Cpp},
        {"deneme.xyz", "deneme.xyz", tui::SyntaxLanguage::Plain},
        {"a.tar.gz", "a.tar.gz", tui::SyntaxLanguage::Plain},
        {"notlar.v2", "notlar.v2", tui::SyntaxLanguage::Plain},
        {"Kod.CPP", "kod.CPP", tui::SyntaxLanguage::Cpp},
    };
    for (const auto& item : cases) {
        CAPTURE(item.typed);
        Fixture f;
        f.session.begin_new_note("");
        f.type("# baslik");  // detect md der: yazılan uzantı yine de kazanır
        f.session.handle(Key::CtrlS);
        REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
        while (!f.session.name_input().empty())
            f.session.handle(Key::Backspace);
        f.type(item.typed);
        f.session.handle(Key::Enter);
        REQUIRE(f.session.path());
        CHECK(f.session.path()->string() == item.path);
        CHECK(f.session.syntax_language() == item.language);
    }
}

TEST_CASE("new note: a blank draft opens at once, no name prompt and no file") {
    Fixture f;
    f.session.begin_new_note("");
    CHECK(f.active == std::vector<bool>{true});
    REQUIRE(f.session.is_open());
    CHECK(f.session.is_draft());
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text().empty());
    CHECK(f.store.list().empty());
    f.type("içerik ş");
    CHECK(f.store.list().empty());
}

TEST_CASE(
    "new note: Ctrl+S asks for the name prefilled from the first line and detect, Enter stays editing") {
    Fixture f;
    f.session.begin_new_note("");
    f.clipboard = "public class Main {\n    public static void main(String[] args) {\n        "
                  "System.out.println(\"x\");\n    }\n}\n";
    f.session.handle(Key::CtrlV);
    f.session.handle(Key::CtrlS);
    CHECK(f.session.mode() == EditorSession::Mode::NamePrompt);
    CHECK(f.session.name_input() == "public-class-main.java");
    CHECK(f.store.list().empty());
    f.session.handle(Key::Enter);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    REQUIRE(f.session.is_open());
    CHECK_FALSE(f.session.is_draft());
    CHECK_FALSE(f.session.dirty());
    CHECK(f.bytes("public-class-main.java") == f.clipboard);
    CHECK(f.saved == std::vector<std::string>{"public-class-main.java"});
    f.type("x");
    f.session.handle(Key::CtrlS);  // artık normal not: soru yok, mtime denetimli yazma
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.bytes("public-class-main.java") == f.clipboard + "x");
    CHECK(f.saved.size() == 2);
}

// Değer dosya adına ve senkrona girmez; `detect` tek satırı md/txt sayar, kural tipten bağımsızdır.
constexpr std::string_view kSecretFirstLines[][2] = {
    {"AWS_SECRET=sk-live-123", "aws-secret"},
    {"export TOKEN=x", "token"},
    {"API_KEY: abc", "api-key"},
    {"# db ayarlari\nAPI_TOKEN=gizli-deger\nDB_HOST=localhost\n", "api-token"},
};

TEST_CASE("new note: the name prompt never suggests a value from the first line") {
    for (const auto& [content, stem] : kSecretFirstLines) {
        CAPTURE(content);
        Fixture f;
        f.session.begin_new_note("");
        f.type(content);
        f.session.handle(Key::CtrlS);
        REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
        CHECK(f.session.name_input().starts_with(std::string(stem) + "."));
    }
}

TEST_CASE("draft exit: a dirty draft is saved under the key, never the value") {
    for (const auto& [content, stem] : kSecretFirstLines) {
        CAPTURE(content);
        Fixture named;
        named.session.begin_new_note("");
        named.type(content);
        CHECK_FALSE(named.session.flush_on_exit().empty());
        CHECK(named.session.path()->string().starts_with(std::string(stem) + "."));

        Fixture query;
        query.session.begin_draft(std::string(content), "");
        CHECK_FALSE(query.session.flush_on_exit().empty());
        CHECK(query.session.path()->string().starts_with(std::string(stem) + "."));
    }
}

TEST_CASE("new note: pasted env content is suggested as name.env and the value stays out of the name") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("API_TOKEN=gizli-deger\nDB_HOST=localhost\n");
    f.session.handle(Key::CtrlS);
    CHECK(f.session.name_input() == "api-token.env");
    f.session.handle(Key::Enter);
    CHECK(f.bytes("api-token.env") == "API_TOKEN=gizli-deger\nDB_HOST=localhost\n");
}

TEST_CASE("new note: Esc at the name prompt returns to editing with nothing written") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("kalsın");
    f.session.handle(Key::CtrlS);
    REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
    f.session.handle(Key::Escape);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.session.is_draft());
    CHECK(f.text() == "kalsın");
    CHECK(f.store.list().empty());
    CHECK(f.saved.empty());
    CHECK(f.active == std::vector<bool>{true});
}

TEST_CASE("new note: letters in the name prompt only edit the name") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("x");
    f.session.handle(Key::CtrlS);
    f.clear_name();
    f.type("dqnaz");
    CHECK(f.session.name_input() == "dqnaz");
    f.session.handle(Key::Backspace);
    CHECK(f.session.name_input() == "dqna");
    CHECK(f.text() == "x");
}

TEST_CASE("new note: an invalid name stays in the prompt with an error") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("x");
    f.session.handle(Key::CtrlS);
    f.clear_name();
    f.type("../dışarı");
    f.session.handle(Key::Enter);
    CHECK(f.session.mode() == EditorSession::Mode::NamePrompt);
    CHECK(f.session.message_is_error());
    CHECK(f.session.is_draft());
    CHECK(f.store.list().empty());
}

TEST_CASE("new note: the name may carry a folder and an explicit extension wins over detect") {
    Fixture f;
    f.session.begin_new_note("");
    f.clipboard = "public class Main { public static void main(String[] a) {} }\n";
    f.session.handle(Key::CtrlV);
    f.session.handle(Key::CtrlS);
    f.clear_name();
    f.type("iş/Toplantı.txt");
    f.session.handle(Key::Enter);
    CHECK(f.bytes("iş/toplanti.txt") == f.clipboard);
    CHECK(f.saved == std::vector<std::string>{"iş/toplanti.txt"});
}

TEST_CASE("new note: a name without an extension asks detect, default type as fallback") {
    Fixture f;
    f.session.begin_new_note("");
    const std::string java = "public class Main {\n    public static void main(String[] args) {\n        "
                             "System.out.println(\"x\");\n    }\n}\n";
    f.clipboard = java;
    f.session.handle(Key::CtrlV);
    f.session.handle(Key::CtrlS);
    f.clear_name();
    f.type("kod");
    f.session.handle(Key::Enter);
    CHECK(f.bytes("kod.java") == java);

    Fixture plain;
    plain.session.begin_new_note("");
    plain.type("x");
    plain.session.handle(Key::CtrlS);
    plain.clear_name();
    plain.type("kısa");
    plain.session.handle(Key::Enter);
    CHECK(plain.bytes("kisa.md") == "x");
}

TEST_CASE("new note: a single line draft is prefilled with the default type") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("Hızlı Not");
    f.session.handle(Key::CtrlS);
    CHECK(f.session.name_input() == "hizli-not.md");
}

TEST_CASE("new note: a name collision uses the -2 suffix and never overwrites") {
    Fixture f;
    f.put("ad.md", "eski");
    f.session.begin_new_note("");
    f.type("yeni");
    f.session.handle(Key::CtrlS);
    f.clear_name();
    f.type("ad.md");
    f.session.handle(Key::Enter);
    CHECK(f.bytes("ad.md") == "eski");
    CHECK(f.bytes("ad-2.md") == "yeni");
    CHECK(f.session.path()->string() == "ad-2.md");
}

TEST_CASE(
    "new note: a name without a folder goes to the default folder, an emptied name uses the timestamp") {
    Fixture f;
    f.session.begin_new_note("work");
    f.type("ilk satır");
    f.session.handle(Key::CtrlS);
    CHECK(f.session.name_folder() == "work");
    f.clear_name();
    f.session.handle(Key::Enter);
    const auto notes = f.store.list();
    REQUIRE(notes.size() == 1);
    const std::string name = notes.front().path.string();
    CHECK(name.starts_with("work/"));
    CHECK(name.find("ilk") == std::string::npos);  // içerikten slug değil
    CHECK(f.bytes(name) == "ilk satır");
}

TEST_CASE(
    "draft: Ctrl+Q on an empty draft closes silently, a non-empty one asks and Discard leaves no file") {
    Fixture f;
    f.session.begin_new_note("");
    f.session.handle(Key::CtrlQ);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.closed_line == std::nullopt);
    CHECK(f.store.list().empty());

    f.session.begin_new_note("");
    f.type("metin");
    f.session.handle(Key::CtrlQ);
    CHECK(f.session.mode() == EditorSession::Mode::LeavePrompt);
    CHECK(f.session.leave_choice() == EditorSession::LeaveChoice::Save);
    f.press_leave_choice(EditorSession::LeaveChoice::Cancel);
    CHECK(f.text() == "metin");
    f.session.handle(Key::CtrlQ);
    f.press_leave_choice(EditorSession::LeaveChoice::Discard);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.store.list().empty());
    CHECK(f.saved.empty());
}

TEST_CASE("draft: Save at the leave prompt asks for the name, then saves and closes") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("metin");
    f.session.handle(Key::CtrlQ);
    f.session.handle(Key::Enter);
    REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
    CHECK(f.session.name_input() == "metin.md");
    CHECK(f.store.list().empty());
    f.session.handle(Key::Enter);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.bytes("metin.md") == "metin");
    CHECK(f.saved == std::vector<std::string>{"metin.md"});
}

TEST_CASE("draft: Esc at the name prompt after Save at the leave prompt keeps editing, no late close") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("metin");
    f.session.handle(Key::CtrlQ);
    f.session.handle(Key::Enter);
    REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
    f.session.handle(Key::Escape);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    REQUIRE(f.session.is_open());
    f.session.handle(Key::CtrlS);
    f.session.handle(Key::Enter);
    CHECK(f.session.is_open());  // bu kez yalnız kayıt: editör açık kalır
    CHECK(f.bytes("metin.md") == "metin");
}

TEST_CASE("draft: Ctrl+S on an empty draft creates nothing and asks nothing") {
    Fixture f;
    f.session.begin_new_note("");
    f.session.handle(Key::CtrlS);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.store.list().empty());
    CHECK(f.saved.empty());
    CHECK(f.session.is_draft());
}

TEST_CASE("draft: nvALT query is the unsaved first line and names the note from the content") {
    Fixture f;
    f.session.begin_draft("Alışveriş Listesi\n", "");
    REQUIRE(f.session.is_open());
    CHECK(f.active == std::vector<bool>{true});
    CHECK(f.text() == "Alışveriş Listesi\n");
    CHECK(f.session.dirty());
    CHECK(f.store.list().empty());
    f.session.handle(Key::CtrlS);
    const auto notes = f.store.list();
    REQUIRE(notes.size() == 1);
    CHECK(notes.front().path.string().starts_with("alisveris-listesi"));
    CHECK(f.bytes(notes.front().path.string()) == "Alışveriş Listesi\n");
}

TEST_CASE("draft: a rescan does not touch it and on exit it is written, not dropped") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("kalsın");
    f.rescan();
    CHECK(f.session.is_draft());
    CHECK(f.text() == "kalsın");
    CHECK_FALSE(f.session.flush_on_exit().empty());
    CHECK(f.bytes("kalsin.md") == "kalsın");
}

TEST_CASE("draft: exit with the name prompt open writes the text, the value stays out of the file name") {
    Fixture f;
    f.session.begin_new_note("");
    f.type("API_TOKEN=gizli\nB=1\n");
    f.session.handle(Key::CtrlS);
    REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
    CHECK_FALSE(f.session.flush_on_exit().empty());
    CHECK(f.bytes("api-token.env") == "API_TOKEN=gizli\nB=1\n");
}

TEST_CASE("draft: an empty draft on exit creates nothing") {
    Fixture f;
    f.session.begin_new_note("");
    CHECK(f.session.flush_on_exit().empty());
    CHECK(f.store.list().empty());
}

TEST_CASE("session: Ctrl+Q on a clean note returns to the list without a prompt or write") {
    Fixture f;
    f.open_with("a.md", "metin");
    const auto before = f.revision("a.md");
    f.session.handle(Key::CtrlQ);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.active == std::vector<bool>{true, false});
    CHECK(f.closed_line == 0);  // Önizlemeye döner
    CHECK(f.saved.empty());
    CHECK(f.revision("a.md") == before);
}

TEST_CASE("session: Ctrl+Q on a dirty note asks with Save as the default") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.type("x");
    f.session.handle(Key::CtrlQ);
    CHECK(f.session.mode() == EditorSession::Mode::LeavePrompt);
    CHECK(f.session.leave_choice() == EditorSession::LeaveChoice::Save);
    CHECK(f.session.is_open());
    f.session.handle(Key::Char, "q");  // açık soruda harf metne de gitmez, soruyu da kapatmaz
    CHECK(f.text() == "xmetin");
    CHECK(f.session.mode() == EditorSession::Mode::LeavePrompt);
    f.session.handle(Key::Enter);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.bytes("a.md") == "xmetin");
    CHECK(f.saved == std::vector<std::string>{"a.md"});
}

TEST_CASE("session: Discard leaves the file alone, Cancel and Esc stay in the editor") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.type("x");
    f.session.handle(Key::CtrlQ);
    f.press_leave_choice(EditorSession::LeaveChoice::Cancel);
    CHECK(f.session.is_open());
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text() == "xmetin");
    f.session.handle(Key::CtrlQ);
    f.session.handle(Key::Escape);
    CHECK(f.session.is_open());
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    f.session.handle(Key::CtrlQ);
    f.press_leave_choice(EditorSession::LeaveChoice::Discard);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.bytes("a.md") == "metin");
    CHECK(f.saved.empty());
}

TEST_CASE("session: F9 on a clean note leaves and opens settings, dirty goes through the prompt") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.session.handle(Key::F9);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.settings_opened == 1);

    f.open_with("b.md", "metin");
    f.type("x");
    f.session.handle(Key::F9);
    CHECK(f.settings_opened == 1);
    CHECK(f.session.mode() == EditorSession::Mode::LeavePrompt);
    f.press_leave_choice(EditorSession::LeaveChoice::Cancel);
    CHECK(f.settings_opened == 1);
    CHECK(f.session.is_open());
    f.session.handle(Key::F9);
    f.session.handle(Key::Enter);  // varsayılan Kaydet
    CHECK(f.bytes("b.md") == "xmetin");
    CHECK(f.settings_opened == 2);
    CHECK_FALSE(f.session.is_open());
}

TEST_CASE("session: Ctrl+S writes once through the store, notifies and clears dirty") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.type("ş");
    CHECK(f.session.dirty());
    f.session.handle(Key::CtrlS);
    CHECK(f.bytes("a.md") == "şmetin");
    CHECK_FALSE(f.session.dirty());
    CHECK(f.saved == std::vector<std::string>{"a.md"});
    CHECK_FALSE(f.session.message().empty());
    CHECK_FALSE(f.session.message_is_error());
}

TEST_CASE("session: Ctrl+S on a clean buffer writes nothing and does not notify") {
    Fixture f;
    f.open_with("a.md", "metin");
    const auto before = f.revision("a.md");
    f.session.handle(Key::CtrlS);
    f.type("x");
    f.session.handle(Key::Backspace);
    f.session.handle(Key::CtrlS);
    CHECK(f.saved.empty());
    CHECK(f.revision("a.md") == before);
}

TEST_CASE("session: consecutive saves do not conflict with themselves") {
    Fixture f;
    f.open_with("a.md", "");
    f.type("a");
    f.session.handle(Key::CtrlS);
    f.type("b");
    f.session.handle(Key::CtrlS);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.saved.size() == 2);
    CHECK(f.bytes("a.md") == "ab");
}

TEST_CASE("session: CRLF, BOM, control bytes and a missing final newline survive an edit byte for byte") {
    Fixture f;
    const std::string body = "bir\r\nkarışık\niki\x1b[31m\x07\r\xC2\x9Bson";
    f.open_with("a.md", std::string(kBom) + body);
    CHECK(f.text() == body);  // BOM tamponda görünmez
    f.session.handle(Key::CtrlEnd);
    f.type("!");
    f.session.handle(Key::CtrlS);
    CHECK(f.bytes("a.md") == std::string(kBom) + body + "!");
    f.session.handle(Key::Backspace);
    f.session.handle(Key::CtrlS);
    CHECK(f.bytes("a.md") == std::string(kBom) + body);
}

TEST_CASE("session: Enter in a CRLF note adds CRLF") {
    Fixture f;
    f.open_with("a.md", "a\r\nb");
    f.session.handle(Key::CtrlEnd);
    f.session.handle(Key::Enter);
    f.session.handle(Key::CtrlS);
    CHECK(f.bytes("a.md") == "a\r\nb\r\n");
}

TEST_CASE("session: read-only notes refuse every edit and are never written") {
    struct Case {
        std::string content;
        EditorSession::ReadOnly reason;
    };
    const std::string oversized(kTestLimit + 1, 'x');
    const std::vector<Case> cases = {
        {std::string("ikili\0dosya", 11), EditorSession::ReadOnly::Binary},
        {std::string(9000, 'a') + std::string(1, '\0') + "son", EditorSession::ReadOnly::EmbeddedNul},
        {"bozuk \xFF\xFE utf8", EditorSession::ReadOnly::InvalidUtf8},
    };
    const auto check = [](Fixture& f, const std::string& name, const std::string& content) {
        const auto before = f.revision(name);
        CHECK_FALSE(f.session.message().empty());  // nedeni görünür
        for (const Key key : {Key::Backspace, Key::Delete, Key::Enter, Key::Tab, Key::CtrlX, Key::CtrlV,
                              Key::CtrlZ, Key::CtrlY})
            f.session.handle(key);
        f.session.handle(Key::Char, "x");
        CHECK_FALSE(f.session.dirty());
        f.session.handle(Key::CtrlS);
        CHECK(f.saved.empty());
        CHECK(f.bytes(name) == content);
        CHECK(f.revision(name) == before);
        CHECK(f.session.flush_on_exit().empty());
        CHECK(f.bytes(name) == content);
        f.session.handle(Key::Down);  // kaydırma çalışır
        f.session.handle(Key::CtrlQ);
        CHECK_FALSE(f.session.is_open());
    };
    for (const auto& item : cases) {
        CAPTURE(item.content.size());
        Fixture f(kRoomyLimit);
        f.clipboard = "yapıştır";
        f.open_with("r.md", item.content);
        CHECK(f.session.read_only() == item.reason);
        check(f, "r.md", item.content);
    }
    Fixture f;
    f.open_with("big.md", oversized);
    CHECK(f.session.read_only() == EditorSession::ReadOnly::TooLarge);
    CHECK(f.text().size() == kTestLimit);
    check(f, "big.md", oversized);
}

TEST_CASE("session: the size limit is inclusive and a note at the limit stays editable") {
    Fixture f;
    f.open_with("edge.md", std::string(kTestLimit, 'x'));
    CHECK(f.session.read_only() == EditorSession::ReadOnly::No);
}

TEST_CASE("session: a read-only view holds valid text so the cursor math stays safe") {
    Fixture f;
    f.open_with("r.md", "a\xFF"
                        "b\xC3");
    REQUIRE(f.session.read_only() == EditorSession::ReadOnly::InvalidUtf8);
    f.session.handle(Key::CtrlEnd);
    f.session.layout(10, 5);
    CHECK(f.text() == "a\xEF\xBF\xBD"
                      "b\xEF\xBF\xBD");
}

TEST_CASE("session: a modified file opens the conflict dialog, default copy, and nothing is written yet") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.change_externally("a.md", "dış");
    f.session.handle(Key::CtrlS);
    CHECK(f.session.mode() == EditorSession::Mode::ConflictPrompt);
    CHECK(f.session.conflict_choice() == EditorSession::ConflictChoice::SaveCopy);
    CHECK(f.bytes("a.md") == "dış");
    CHECK(f.saved.empty());
    CHECK(f.text() == "xeski");
}

TEST_CASE("session: conflict: save as copy keeps both versions and edits the copy") {
    Fixture f;
    f.open_with("work/a.md", "eski\r\n");
    f.type("x");
    f.change_externally("work/a.md", "dış");
    f.session.handle(Key::CtrlS);
    f.session.handle(Key::Enter);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.bytes("work/a.md") == "dış");
    CHECK(f.bytes("work/a (copy).md") == "xeski\r\n");
    CHECK(f.saved == std::vector<std::string>{"work/a (copy).md"});
    CHECK(f.session.path()->string() == "work/a (copy).md");
    CHECK_FALSE(f.session.dirty());
    f.type("y");
    f.session.handle(Key::CtrlS);
    CHECK(f.bytes("work/a (copy).md") == "xyeski\r\n");
}

TEST_CASE("session: conflict: load the disk version drops the buffer only after that explicit choice") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.change_externally("a.md", "dış sürüm");
    f.session.handle(Key::CtrlS);
    f.press_conflict_choice(EditorSession::ConflictChoice::LoadDisk);
    CHECK(f.text() == "dış sürüm");
    CHECK_FALSE(f.session.dirty());
    CHECK(f.bytes("a.md") == "dış sürüm");
    CHECK(f.saved.empty());
    f.type("y");
    f.session.handle(Key::CtrlS);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);  // yeni mtime benimsendi
    CHECK(f.bytes("a.md").find('y') != std::string::npos);
}

TEST_CASE("session: conflict: write mine overwrites on purpose") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.change_externally("a.md", "dış");
    f.session.handle(Key::CtrlS);
    f.press_conflict_choice(EditorSession::ConflictChoice::Overwrite);
    CHECK(f.bytes("a.md") == "xeski");
    CHECK(f.saved == std::vector<std::string>{"a.md"});
    CHECK_FALSE(f.session.dirty());
}

TEST_CASE("session: conflict: Cancel and Esc keep the buffer and the disk untouched") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.change_externally("a.md", "dış");
    f.session.handle(Key::CtrlS);
    f.press_conflict_choice(EditorSession::ConflictChoice::Cancel);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    f.session.handle(Key::CtrlS);
    f.session.handle(Key::Escape);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text() == "xeski");
    CHECK(f.session.dirty());
    CHECK(f.bytes("a.md") == "dış");
    CHECK(f.saved.empty());
}

TEST_CASE("session: Save at the leave prompt with a conflict leaves only after it is resolved") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.change_externally("a.md", "dış");
    f.session.handle(Key::CtrlQ);
    f.session.handle(Key::Enter);
    REQUIRE(f.session.mode() == EditorSession::Mode::ConflictPrompt);
    CHECK(f.session.is_open());
    f.press_conflict_choice(EditorSession::ConflictChoice::Cancel);
    CHECK(f.session.is_open());
    CHECK(f.text() == "xeski");

    f.session.handle(Key::CtrlQ);
    f.session.handle(Key::Enter);
    f.session.handle(Key::Enter);  // varsayılan: kopya
    CHECK_FALSE(f.session.is_open());
    CHECK(f.bytes("a (copy).md") == "xeski");
    CHECK(f.bytes("a.md") == "dış");
}

TEST_CASE("session: a failed write keeps the buffer, shows the error and does not leave") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    // Yazma kilidi başka bağlantıda: busy_timeout sonunda düşen gerçek yazma hatası, kök kullanıcıda da.
    auto holder = noteapp::graph::Graph::open(f.dir.path() / "graph.db");
    REQUIRE(holder.has_value());
    auto lock = holder->write();
    REQUIRE(lock.has_value());
    f.session.handle(Key::CtrlS);
    CHECK(f.session.is_open());
    CHECK(f.session.dirty());
    CHECK(f.text() == "xeski");
    CHECK(f.session.message_is_error());
    CHECK_FALSE(f.session.message().empty());
    CHECK(f.saved.empty());

    f.session.handle(Key::CtrlQ);
    f.session.handle(Key::Enter);  // Kaydet yine düşer: çıkılmaz
    CHECK(f.session.is_open());
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text() == "xeski");
}

TEST_CASE("session: a rescan never replaces a dirty buffer") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.change_externally("a.md", "dış");
    f.rescan();
    CHECK(f.text() == "xeski");
    CHECK(f.session.dirty());
    CHECK(f.session.changed_on_disk());
    CHECK(f.bytes("a.md") == "dış");
}

TEST_CASE("session: a rescan reloads a clean buffer and keeps the cursor line and column") {
    Fixture f;
    f.open_with("a.md", "bir\niki\n");
    f.session.handle(Key::Down);
    f.session.handle(Key::Right);
    f.change_externally("a.md", "üç\ndört\nbeş\n");
    f.rescan();
    CHECK(f.text() == "üç\ndört\nbeş\n");
    CHECK_FALSE(f.session.dirty());
    CHECK(f.session.buffer()->position().line == 1);
    CHECK(f.session.buffer()->position().column == 1);
    CHECK(f.saved.empty());
}

TEST_CASE("session: a clean reload picks up a new BOM and read-only state from the disk") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.change_externally("a.md", std::string("ikili\0", 6));
    f.rescan();
    CHECK(f.session.read_only() == EditorSession::ReadOnly::Binary);
    f.change_externally("a.md", std::string(kBom) + "metin");
    f.rescan();
    CHECK(f.session.read_only() == EditorSession::ReadOnly::No);
    CHECK(f.text() == "metin");
    f.type("x");
    f.session.handle(Key::CtrlS);
    CHECK(f.bytes("a.md") == std::string(kBom) + "xmetin");
}

TEST_CASE("session: a rescan with no change, or a stale listing after our own save, does nothing") {
    Fixture f;
    f.open_with("a.md", "metin");
    const auto stale = f.store.list();
    f.type("x");
    f.session.handle(Key::CtrlS);
    f.type("y");
    f.session.on_listing(stale);  // kayıttan önce alınmış liste: mtime farklı ama disk bizim yazdığımız
    CHECK(f.text() == "xymetin");
    CHECK_FALSE(f.session.changed_on_disk());
    f.rescan();
    CHECK(f.text() == "xymetin");
}

TEST_CASE("session: deleted on disk while clean closes the editor") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.erase("a.md");
    f.rescan();
    CHECK_FALSE(f.session.is_open());
    CHECK(f.active == std::vector<bool>{true, false});
    CHECK(f.closed_line == std::nullopt);
    CHECK_FALSE(f.exists("a.md"));
}

TEST_CASE("session: deleted on disk while dirty keeps the text and Ctrl+S saves it as a new note") {
    Fixture f;
    f.open_with("work/a.md", "eski\r\n");
    f.type("x");
    f.erase("work/a.md");
    f.rescan();
    CHECK(f.session.is_open());
    CHECK(f.session.deleted_on_disk());
    CHECK(f.text() == "xeski\r\n");
    f.session.handle(Key::CtrlS);
    CHECK(f.bytes("work/a.md") == "xeski\r\n");
    CHECK(f.saved == std::vector<std::string>{"work/a.md"});
    CHECK_FALSE(f.session.deleted_on_disk());
    CHECK_FALSE(f.session.dirty());
}

TEST_CASE("session: a save that finds the note gone also saves it as new without a rescan") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.erase("a.md");
    f.session.handle(Key::CtrlQ);
    f.session.handle(Key::Enter);
    CHECK_FALSE(f.session.is_open());
    CHECK(f.bytes("a.md") == "xeski");
}

TEST_CASE("session: on exit a dirty buffer is written through the normal path") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    CHECK_FALSE(f.session.flush_on_exit().empty());
    CHECK(f.bytes("a.md") == "xeski");
    CHECK(f.saved == std::vector<std::string>{"a.md"});
}

TEST_CASE("session: on exit a clean buffer writes nothing and says nothing") {
    Fixture f;
    f.open_with("a.md", "eski");
    const auto before = f.revision("a.md");
    CHECK(f.session.flush_on_exit().empty());
    CHECK(f.revision("a.md") == before);
    Fixture closed;
    CHECK(closed.session.flush_on_exit().empty());
}

TEST_CASE("session: on exit a conflict never overwrites: the text goes to a copy") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.change_externally("a.md", "dış");
    CHECK_FALSE(f.session.flush_on_exit().empty());
    CHECK(f.bytes("a.md") == "dış");
    CHECK(f.bytes("a (copy).md") == "xeski");
}

TEST_CASE("session: on exit a deleted note is created again") {
    Fixture f;
    f.open_with("a.md", "eski");
    f.type("x");
    f.erase("a.md");
    CHECK_FALSE(f.session.flush_on_exit().empty());
    CHECK(f.bytes("a.md") == "xeski");
}

TEST_CASE("session: on exit, when the store refuses, the text is kept in a private temp file") {
    Fixture f;
    const test::TempDir state;
    const test::ScopedEnv home("NOTEAPP_HOME", test::utf8(state.path()));
    f.open_with("a.md", "eski");
    f.type("x");
    auto holder = noteapp::graph::Graph::open(f.dir.path() / "graph.db");
    REQUIRE(holder.has_value());
    auto lock = holder->write();
    REQUIRE(lock.has_value());
    const std::string said = f.session.flush_on_exit();
    REQUIRE_FALSE(said.empty());
    const auto marker = said.rfind(": ");
    REQUIRE(marker != std::string::npos);
    const fs::path kept = said.substr(marker + 2);
    CHECK(test::slurp(kept) == "xeski");
    fs::remove(kept);
}

namespace {

// Tamponun diske yeni dosya olarak yazıldığı üç yol; aynı adımla yeniden denenebilirler.
struct SaveAsNew {
    std::string name;
    std::string target;
    std::vector<std::string> left_after_failure;
    void (*prepare)(Fixture&);
    void (*save)(Fixture&);
};

const std::vector<SaveAsNew>& save_as_new_paths() {
    static const std::vector<SaveAsNew> paths = {
        {"save as copy",
         "work/a (copy).md",
         {"work/a.md"},
         [](Fixture& f) { f.change_externally("work/a.md", "dış"); },
         [](Fixture& f) {
             f.session.handle(Key::CtrlS);
             REQUIRE(f.session.mode() == EditorSession::Mode::ConflictPrompt);
             f.session.handle(Key::Enter);
         }},
        {"deleted on disk",
         "work/a.md",
         {},
         [](Fixture& f) {
             f.erase("work/a.md");
             f.rescan();
             REQUIRE(f.session.deleted_on_disk());
         },
         [](Fixture& f) { f.session.handle(Key::CtrlS); }},
        {"recreate on exit",
         "work/a.md",
         {},
         [](Fixture& f) { f.erase("work/a.md"); },
         [](Fixture& f) { static_cast<void>(f.session.flush_on_exit()); }},
    };
    return paths;
}

const std::string kCrlfBomBody = std::string(kBom) + "xeski\r\nson\r\n";

}  // namespace

TEST_CASE("session: saving a CRLF and BOM buffer as a new note writes the exact bytes") {
    for (const SaveAsNew& path : save_as_new_paths()) {
        CAPTURE(path.name);
        Fixture f;
        const test::TempDir state;
        const test::ScopedEnv home("NOTEAPP_HOME", test::utf8(state.path()));
        f.open_with("work/a.md", std::string(kBom) + "eski\r\nson\r\n");
        f.type("x");
        path.prepare(f);
        path.save(f);
        CHECK(f.bytes(path.target) == kCrlfBomBody);
        CHECK(f.session.path()->string() == path.target);
        CHECK_FALSE(f.session.dirty());
        CHECK(f.saved == std::vector<std::string>{path.target});
    }
}

#ifndef _WIN32
namespace {

// Dizindeki bütün dosyalar (gizli geçici dosyalar dahil), sıralı.
std::vector<std::string> files_under(const fs::path& root) {
    std::vector<std::string> names;
    for (const core::NoteInfo& note : core::NoteStore(root).list())
        names.push_back(note.path.string());
    std::ranges::sort(names);
    return names;
}

// Yazımı gerçekten düşürür: konteynerde root olarak da çalışır, izin biti çalışmazdı. SIGXFSZ yok sayılmazsa
// süreci öldürür; write() o zaman EFBIG döner.
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t bytes) : handler_(std::signal(SIGXFSZ, SIG_IGN)) {
        REQUIRE(::getrlimit(RLIMIT_FSIZE, &original_) == 0);
        rlimit limited = original_;
        limited.rlim_cur = bytes;
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &original_);
        std::signal(SIGXFSZ, handler_);
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;

private:
    rlimit original_{};
    void (*handler_)(int);
};

}  // namespace

// Sınır LF'ye indirilmiş metne yeter, tam baytlara yetmez: eski iki adımlı kayıtta ilk adım geçip ikincisi
// düşerdi.
TEST_CASE("session: a failed save as new note leaves no file and the retry makes no duplicate") {
    for (const SaveAsNew& path : save_as_new_paths()) {
        CAPTURE(path.name);
        Fixture f;
        const test::TempDir state;
        const test::ScopedEnv home("NOTEAPP_HOME", test::utf8(state.path()));
        f.open_with("work/a.md", std::string(kBom) + "eski\r\nson\r\n");
        f.type("x");
        path.prepare(f);
        {
            const FileSizeLimit limit(kCrlfBomBody.size() - 1);
            path.save(f);
        }
        CHECK(files_under(f.dir.path()) == path.left_after_failure);
        CHECK(f.session.dirty());
        CHECK(f.saved.empty());
        CHECK(f.text() == "xeski\r\nson\r\n");

        path.save(f);
        std::vector<std::string> expected = path.left_after_failure;
        expected.emplace_back(path.target);
        std::ranges::sort(expected);
        CHECK(files_under(f.dir.path()) == expected);
        CHECK(f.bytes(path.target) == kCrlfBomBody);
        CHECK(f.session.path()->string() == path.target);
        CHECK_FALSE(f.session.dirty());
        CHECK(f.saved == std::vector<std::string>{path.target});
    }
}
#endif

TEST_CASE("exit: Esc alone never leaves the editor and never discards") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.session.handle(Key::Escape);
    CHECK(f.session.is_open());
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    f.type("x");
    f.session.handle(Key::Escape);
    CHECK(f.session.is_open());
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.session.dirty());
    CHECK(f.text() == "xmetin");
    CHECK(f.bytes("a.md") == "metin");

    Fixture draft;
    draft.session.begin_new_note("");
    draft.session.handle(Key::Escape);
    CHECK(draft.session.is_open());  // boş taslak da Esc ile kapanmaz
}

TEST_CASE("exit: Esc clears a selection and keeps the cursor, the next Esc does nothing") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.session.handle(Key::ShiftRight);
    f.session.handle(Key::ShiftRight);
    REQUIRE(f.session.buffer()->selection());
    const std::size_t cursor = f.session.buffer()->cursor();
    f.session.handle(Key::Escape);
    CHECK_FALSE(f.session.buffer()->selection());
    CHECK(f.session.buffer()->cursor() == cursor);
    f.session.handle(Key::Escape);
    CHECK(f.session.is_open());
}

TEST_CASE("exit: Esc closes the find input and stays in the editor") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.session.handle(Key::CtrlF);
    REQUIRE(f.session.mode() == EditorSession::Mode::Find);
    f.session.handle(Key::Escape);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.session.is_open());
}

TEST_CASE("exit: Ctrl+Q does nothing inside the find input or the name prompt") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.session.handle(Key::CtrlF);
    f.session.handle(Key::CtrlQ);
    CHECK(f.session.mode() == EditorSession::Mode::Find);
    f.session.handle(Key::Escape);

    Fixture draft;
    draft.session.begin_new_note("");
    draft.type("x");
    draft.session.handle(Key::CtrlS);
    REQUIRE(draft.session.mode() == EditorSession::Mode::NamePrompt);
    draft.session.handle(Key::CtrlQ);
    CHECK(draft.session.mode() == EditorSession::Mode::NamePrompt);
}

TEST_CASE("exit: Esc at the leave prompt cancels, Ctrl+Q there changes nothing") {
    Fixture f;
    f.open_with("a.md", "metin");
    f.type("x");
    f.session.handle(Key::CtrlQ);
    REQUIRE(f.session.mode() == EditorSession::Mode::LeavePrompt);
    f.session.handle(Key::CtrlQ);
    CHECK(f.session.mode() == EditorSession::Mode::LeavePrompt);
    f.session.handle(Key::Escape);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);
    CHECK(f.text() == "xmetin");
}
