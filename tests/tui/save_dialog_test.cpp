#include "core/editor.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "core/text.hpp"
#include "support/test_support.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <vector>

using namespace noteapp;
using tui::Key;
using tui::SaveDialogState;
using Outcome = tui::SaveDialogState::Outcome;
using Field = tui::SaveDialogState::Field;

namespace {

const auto kNow = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 365 * 50);

tui::SaveDialogInit make_init(std::string content = "Merhaba Dünya\nikinci satır") {
    tui::SaveDialogInit init;
    init.content = std::move(content);
    init.folder = "work";
    init.folders = {"work", "personal", "öğrenci/ödev", "projects", "private", "play", "plans", "pets"};
    init.types = {"md", "txt", "java", "sh"};
    init.suggestion = {"java", 0.82, {}, true};
    init.slug_max_length = 60;
    init.now = kNow;
    return init;
}

void type(SaveDialogState& d, std::string_view text) {
    // Her kod noktası ayrı olay: terminal olayları da böyle gelir.
    for (std::size_t i = 0; i < text.size();) {
        std::size_t len = 1;
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;
        d.handle(Key::Char, text.substr(i, len));
        i += len;
    }
}

void backspaces(SaveDialogState& d, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        d.handle(Key::Backspace);
}

struct I18nEn {
    I18nEn() { core::init_i18n("en"); }
};

}  // namespace

TEST_CASE("dialog is prefilled with slug, folder and detected type") {
    const I18nEn en;
    const SaveDialogState d(make_init());
    CHECK(d.name() == core::slugify("Merhaba Dünya", kNow, 60));
    CHECK(d.name() == "merhaba-dunya");
    CHECK(d.folder() == "work");
    CHECK(d.type() == "java");
    CHECK(d.type_label() == "suggested: java 82%");
    CHECK(d.focus() == Field::Name);
    CHECK_FALSE(d.confirming_discard());
}

TEST_CASE("slug comes from the first non-empty line") {
    const I18nEn en;
    const SaveDialogState d(make_init("\n  \nÇok güzel başlık\nx"));
    CHECK(d.name() == "cok-guzel-baslik");
}

TEST_CASE("undetected type has no suggestion label") {
    const I18nEn en;
    auto init = make_init();
    init.suggestion = {"md", 0.0, {}, false};
    const SaveDialogState d(init);
    CHECK(d.type() == "md");
    CHECK(d.type_label().empty());
}

TEST_CASE("focus cycles with Tab, Shift-Tab and arrows") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Tab);  // klasör alanı dolu ve tamamlanacak öneri yok -> odak ilerler
    CHECK(d.focus() == Field::Folder);
    d.handle(Key::Tab);
    CHECK(d.focus() == Field::Type);
    d.handle(Key::Tab);
    CHECK(d.focus() == Field::Name);
    d.handle(Key::ShiftTab);
    CHECK(d.focus() == Field::Type);
    d.handle(Key::Up);
    CHECK(d.focus() == Field::Folder);
    d.handle(Key::Up);  // öneri seçili değil -> odak yukarı
    CHECK(d.focus() == Field::Name);
    d.handle(Key::Down);
    CHECK(d.focus() == Field::Folder);
}

TEST_CASE("name editing handles Turkish characters and backspaces whole code points") {
    const I18nEn en;
    SaveDialogState d(make_init());
    backspaces(d, d.name().size());
    CHECK(d.name().empty());
    type(d, "şğü");
    CHECK(d.name() == "şğü");
    d.handle(Key::Backspace);
    CHECK(d.name() == "şğ");
    backspaces(d, 3);  // boşken sessiz
    CHECK(d.name().empty());
}

TEST_CASE("control characters are not inserted") {
    const I18nEn en;
    SaveDialogState d(make_init());
    const std::string before = d.name();
    d.handle(Key::Char, "\x01");
    CHECK(d.name() == before);
}

TEST_CASE("folder suggestions are fuzzy, capped at five, and include Turkish names") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Tab);  // klasör
    backspaces(d, 4);
    CHECK(d.suggestions().size() == 5);  // boş sorgu: ilk 5
    type(d, "p");
    CHECK(d.suggestions().size() == 5);  // 6 aday vardı, 5'e kesilir
    backspaces(d, 1);
    type(d, "ödev");
    REQUIRE(d.suggestions().size() == 1);
    CHECK(d.suggestions().front() == "öğrenci/ödev");
    backspaces(d, 4);
    type(d, "zzz");
    CHECK(d.suggestions().empty());
}

TEST_CASE("Tab completes the top suggestion; a second Tab moves on") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Tab);
    backspaces(d, 4);
    type(d, "ödev");
    d.handle(Key::Tab);
    CHECK(d.folder() == "öğrenci/ödev");
    CHECK(d.focus() == Field::Folder);
    d.handle(Key::Tab);
    CHECK(d.focus() == Field::Type);
}

TEST_CASE("Down picks a suggestion, Enter applies it and saves") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Tab);
    backspaces(d, 4);
    type(d, "pe");
    REQUIRE(d.suggestions().size() >= 2);
    d.handle(Key::Down);
    CHECK(d.selected_suggestion() == 0);
    d.handle(Key::Down);
    CHECK(d.selected_suggestion() == 1);
    d.handle(Key::Up);
    CHECK(d.selected_suggestion() == 0);
    const std::string picked = d.suggestions().front();
    CHECK(d.handle(Key::Enter) == Outcome::Submit);
    CHECK(d.request().folder == picked);
}

TEST_CASE("Right and Left cycle the configured types and wrap") {
    const I18nEn en;
    auto init = make_init();
    init.suggestion = {"md", 0.9, {}, true};
    SaveDialogState d(init);
    d.handle(Key::Tab);
    d.handle(Key::Tab);
    REQUIRE(d.focus() == Field::Type);
    d.handle(Key::Right);
    CHECK(d.type() == "txt");
    d.handle(Key::Right);
    d.handle(Key::Right);
    CHECK(d.type() == "sh");
    d.handle(Key::Right);
    CHECK(d.type() == "md");
    d.handle(Key::Left);
    CHECK(d.type() == "sh");
    CHECK(d.type_label().empty());  // öneri etiketi yalnız önerilen tipte
}

TEST_CASE("typing in the type field replaces it with a free extension") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Tab);
    d.handle(Key::Tab);
    type(d, ".xml");  // baştaki nokta atılır
    CHECK(d.type() == "xml");
    d.handle(Key::Backspace);
    CHECK(d.type() == "xm");
    CHECK(d.handle(Key::Enter) == Outcome::Submit);
    CHECK(d.request().type == "xm");
    CHECK(d.request().name == "merhaba-dunya");
    CHECK(d.request().folder == "work");
}

TEST_CASE("invalid name, folder and type show an inline error and keep the dialog open") {
    const I18nEn en;
    SaveDialogState d(make_init());
    type(d, "/x");  // ad: merhaba-dunya/x
    CHECK(d.handle(Key::Enter) == Outcome::Editing);
    CHECK_FALSE(d.error().empty());
    backspaces(d, 2);
    CHECK(d.error().empty());  // düzenleme hatayı siler

    d.handle(Key::Tab);
    backspaces(d, 4);
    type(d, "../kacis");
    CHECK(d.handle(Key::Enter) == Outcome::Editing);
    CHECK(d.error().find("..") != std::string::npos);
    backspaces(d, 8);
    type(d, "work");

    d.handle(Key::Tab);  // tip
    type(d, "a.b");
    CHECK(d.handle(Key::Enter) == Outcome::Editing);
    CHECK_FALSE(d.error().empty());
    backspaces(d, 3);
    type(d, "txt");
    CHECK(d.handle(Key::Enter) == Outcome::Submit);
}

TEST_CASE("a hidden name is rejected by the NotePath rules") {
    const I18nEn en;
    SaveDialogState d(make_init());
    backspaces(d, d.name().size());
    type(d, ".gizli");
    CHECK(d.handle(Key::Enter) == Outcome::Editing);
    CHECK_FALSE(d.error().empty());
}

TEST_CASE("an empty name defers to the slug at save time") {
    const I18nEn en;
    SaveDialogState d(make_init());
    backspaces(d, d.name().size());
    CHECK(d.handle(Key::Enter) == Outcome::Submit);
    CHECK(d.request().name.empty());
}

TEST_CASE("Esc asks for confirmation and the default choice is to keep the note") {
    const I18nEn en;
    SaveDialogState d(make_init());
    CHECK(d.handle(Key::Escape) == Outcome::Editing);
    CHECK(d.confirming_discard());
    CHECK_FALSE(d.discard_selected());  // "Vazgeç" varsayılan
    CHECK(d.handle(Key::Enter) == Outcome::Editing);
    CHECK_FALSE(d.confirming_discard());
    CHECK(d.name() == "merhaba-dunya");  // diyalog durumu korunur
}

TEST_CASE("Esc twice returns to the dialog; choosing discard drops the note") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Escape);
    CHECK(d.handle(Key::Escape) == Outcome::Editing);
    CHECK_FALSE(d.confirming_discard());

    d.handle(Key::Escape);
    d.handle(Key::Right);
    CHECK(d.discard_selected());
    CHECK(d.handle(Key::Enter) == Outcome::Discard);
}

TEST_CASE("typing while confirming discard does not edit the fields") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Escape);
    type(d, "zzz");
    d.handle(Key::Backspace);
    CHECK(d.confirming_discard());
    CHECK(d.name() == "merhaba-dunya");
}

TEST_CASE("initial_folder prefers the selected note's folder, then the default") {
    const auto nested = core::NotePath::parse("work/2026/a.md").value();
    const auto root = core::NotePath::parse("root.md").value();
    CHECK(tui::initial_folder(&nested, "inbox") == "work/2026");
    CHECK(tui::initial_folder(&root, "inbox") == "inbox");
    CHECK(tui::initial_folder(nullptr, "inbox") == "inbox");
}

// ---- render ----

namespace {

ftxui::Screen render_with_dialog(const SaveDialogState& dialog, int width = 80, int height = 20) {
    tui::TuiState state;
    state.set_notes({});
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, {}, {"sync off", ""}, {width, height}, &dialog));
    return screen;
}

bool screen_has(const ftxui::Screen& screen, const std::string& needle) {
    return screen.ToString().find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("render shows the modal with fields, type label and folder suggestions") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Tab);  // klasör
    backspaces(d, 4);
    type(d, "ö");
    const auto screen = render_with_dialog(d);
    CHECK(screen_has(screen, "Save note"));
    CHECK(screen_has(screen, "Name"));
    CHECK(screen_has(screen, "merhaba-dunya"));
    CHECK(screen_has(screen, "Folder"));
    CHECK(screen_has(screen, "öğrenci/ödev"));  // öneri satırı
    CHECK(screen_has(screen, "Type"));
    CHECK(screen_has(screen, "suggested: java 82%"));
    CHECK(screen_has(screen, "Enter save"));
}

TEST_CASE("render shows the inline error") {
    const I18nEn en;
    SaveDialogState d(make_init());
    type(d, "/x");
    d.handle(Key::Enter);
    CHECK(screen_has(render_with_dialog(d), "Invalid path"));
}

TEST_CASE("render shows the discard confirmation with both choices") {
    const I18nEn en;
    SaveDialogState d(make_init());
    d.handle(Key::Escape);
    const auto screen = render_with_dialog(d);
    CHECK(screen_has(screen, "Discard this note?"));
    CHECK(screen_has(screen, "Cancel"));
    CHECK(screen_has(screen, "Discard"));
}

TEST_CASE("render uses the Turkish catalog") {
    core::init_i18n("tr");
    SaveDialogState d(make_init());
    d.handle(Key::Escape);
    const auto screen = render_with_dialog(d);
    CHECK(screen_has(screen, "Not atılsın mı?"));
    CHECK(screen_has(screen, "Vazgeç"));
    CHECK(screen_has(screen, "At"));
    core::init_i18n("en");
}

TEST_CASE("dialog does not replace the too-small notice") {
    const I18nEn en;
    const SaveDialogState d(make_init());
    CHECK(screen_has(render_with_dialog(d, 40, 10), "Window too small"));
}

// ---- kaydetme ----

TEST_CASE("save_new_note creates the note and deletes the temp file") {
    const I18nEn en;
    const test::TempDir home;
    const test::ScopedEnv env("NOTEAPP_HOME", test::utf8(home.path()));
    const test::TempDir notes;
    core::NoteStore store(notes.path());

    const auto temp = core::create_private_temp_file("md", "merhaba\nçok güzel");
    REQUIRE(temp.has_value());
    const auto saved = tui::save_new_note(store, *temp, {"work", "plan", "java"}, "merhaba\nçok güzel");
    REQUIRE(saved.has_value());
    CHECK(saved->string() == "work/plan.java");
    CHECK(test::slurp(notes.path() / "work" / "plan.java") == "merhaba\nçok güzel");
    CHECK_FALSE(std::filesystem::exists(*temp));
}

TEST_CASE("a failing create keeps the temp file and reports the error") {
    const I18nEn en;
    const test::TempDir home;
    const test::ScopedEnv env("NOTEAPP_HOME", test::utf8(home.path()));
    const test::TempDir notes;
    core::NoteStore store(notes.path());

    const auto temp = core::create_private_temp_file("md", "veri kaybolmasın");
    REQUIRE(temp.has_value());
    const auto saved = tui::save_new_note(store, *temp, {"", "x", "a.b"}, "veri kaybolmasın");
    REQUIRE_FALSE(saved.has_value());
    CHECK(saved.error().code == core::Error::Code::InvalidPath);
    CHECK(std::filesystem::exists(*temp));
    CHECK(test::slurp(*temp) == "veri kaybolmasın");
    CHECK(store.list().empty());
    std::filesystem::remove(*temp);
}

TEST_CASE("is_blank treats whitespace-only text as empty") {
    CHECK(tui::is_blank(""));
    CHECK(tui::is_blank(" \t\r\n "));
    CHECK_FALSE(tui::is_blank(" a "));
}
