#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "support/test_support.hpp"
#include "tui/modals.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <vector>

using namespace noteapp;
using tui::Key;
using tui::SaveDialogState;

namespace {

struct I18nEn {
    I18nEn() { core::init_i18n("en"); }
};

core::NotePath path(const std::string& p) {
    return core::NotePath::parse(p).value();
}

SaveDialogState move_dialog(const std::string& p) {
    return SaveDialogState(tui::move_dialog_init(path(p), {"work", "personal"}, {"md", "txt", "java"}, 60));
}

void type(SaveDialogState& d, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i)
        d.handle(Key::Char, text.substr(i, 1));
}

ftxui::Screen render_modal(int width, int height, const SaveDialogState* dialog,
                           const tui::ConfirmState* confirm = nullptr, const tui::HelpState* help = nullptr) {
    tui::TuiState state;
    state.set_notes({});
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, {}, {"sync off", ""}, {width, height}, dialog, confirm, help));
    return screen;
}

bool has(const ftxui::Screen& screen, const std::string& needle) {
    return screen.ToString().find(needle) != std::string::npos;
}

}  // namespace

// ---- taşıma diyaloğu (Ctrl-R) ----

TEST_CASE("move dialog is prefilled from the current path") {
    const I18nEn en;
    const auto d = move_dialog("work/plans/q3.md");
    CHECK(d.name() == "q3");
    CHECK(d.folder() == "work/plans");
    CHECK(d.type() == "md");
    CHECK(d.is_move());
}

TEST_CASE("move dialog for a root note has an empty folder") {
    const I18nEn en;
    CHECK(move_dialog("q3.txt").folder().empty());
}

TEST_CASE("move dialog submit carries the edited values") {
    const I18nEn en;
    auto d = move_dialog("work/q3.md");
    type(d, "-v2");
    CHECK(d.handle(Key::Enter) == SaveDialogState::Outcome::Submit);
    CHECK(d.request().name == "q3-v2");
    CHECK(d.request().folder == "work");
    CHECK(d.request().type == "md");
}

TEST_CASE("move dialog rejects an empty name instead of slugging") {
    const I18nEn en;
    auto d = move_dialog("work/q3.md");
    d.handle(Key::Backspace);
    d.handle(Key::Backspace);
    CHECK(d.handle(Key::Enter) == SaveDialogState::Outcome::Editing);
    CHECK_FALSE(d.error().empty());
}

TEST_CASE("Esc in the move dialog cancels without a discard prompt") {
    const I18nEn en;
    auto d = move_dialog("work/q3.md");
    CHECK(d.handle(Key::Escape) == SaveDialogState::Outcome::Discard);
    CHECK_FALSE(d.confirming_discard());
}

TEST_CASE("move dialog renders its own title") {
    const I18nEn en;
    const auto d = move_dialog("work/q3.md");
    const auto screen = render_modal(80, 20, &d);
    CHECK(has(screen, "Move note"));
    CHECK_FALSE(has(screen, "Save note"));
}

// ---- silme onayı (Ctrl-D) ----

TEST_CASE("delete confirmation defaults to cancel; Enter on it cancels") {
    tui::ConfirmState c("x");
    CHECK_FALSE(c.yes_selected());
    CHECK(c.handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
}

TEST_CASE("only an explicit move to yes and Enter confirms") {
    tui::ConfirmState c("x");
    CHECK(c.handle(Key::Right) == tui::ConfirmState::Outcome::Pending);
    CHECK(c.yes_selected());
    CHECK(c.handle(Key::Left) == tui::ConfirmState::Outcome::Pending);
    CHECK_FALSE(c.yes_selected());
    c.handle(Key::Tab);
    CHECK(c.handle(Key::Enter) == tui::ConfirmState::Outcome::Confirmed);
}

TEST_CASE("Esc and stray keys never confirm") {
    tui::ConfirmState c("x");
    c.handle(Key::Right);
    CHECK(c.handle(Key::Char, "y") == tui::ConfirmState::Outcome::Pending);
    CHECK(c.handle(Key::CtrlD) == tui::ConfirmState::Outcome::Pending);
    CHECK(c.handle(Key::Escape) == tui::ConfirmState::Outcome::Cancelled);
}

TEST_CASE("delete confirmation renders the path and both choices") {
    const I18nEn en;
    const tui::ConfirmState c(core::tr("tui.confirm_delete", {{"path", "work/q3.md"}}));
    const auto screen = render_modal(80, 20, nullptr, &c);
    CHECK(has(screen, "Permanently delete 'work/q3.md'?"));
    CHECK(has(screen, "Cancel"));
    CHECK(has(screen, "Delete"));
}

TEST_CASE("delete confirmation uses the Turkish catalog") {
    core::init_i18n("tr");
    const tui::ConfirmState c(core::tr("tui.confirm_delete", {{"path", "a.md"}}));
    const auto screen = render_modal(80, 20, nullptr, &c);
    CHECK(has(screen, "Vazgeç"));
    CHECK(has(screen, "Sil"));
    core::init_i18n("en");
}

// ---- seçimi komşu satırda tutma ----

TEST_CASE("select_index clamps to the last row") {
    tui::TuiState s;
    s.set_notes({core::NoteInfo{path("a.md"), {}, 1}, core::NoteInfo{path("b.md"), {}, 1}});
    s.select_index(5);
    CHECK(s.selected() == 1);
    s.select_index(0);
    CHECK(s.selected() == 0);
}

// ---- yardım (F1) ----

TEST_CASE("help closes with Esc, F1 and q; other keys keep it open") {
    CHECK(tui::HelpState(20).handle(Key::Escape) == false);
    CHECK(tui::HelpState(20).handle(Key::F1) == false);
    CHECK(tui::HelpState(20).handle(Key::Char, "q") == false);
    CHECK(tui::HelpState(20).handle(Key::Char, "x") == true);
}

TEST_CASE("help scrolls within its bounds") {
    tui::HelpState h(20);
    h.handle(Key::Up);
    CHECK(h.scroll() == 0);
    h.handle(Key::Down);
    CHECK(h.scroll() == 1);
    for (int i = 0; i < 5; ++i)
        h.handle(Key::PageDown);
    CHECK(h.scroll() == h.max_scroll());
    for (int i = 0; i < 5; ++i)
        h.handle(Key::PageUp);
    CHECK(h.scroll() == 0);
}

TEST_CASE("help lists every key from the catalog at the minimum size") {
    const I18nEn en;
    const auto lines = tui::help_lines();
    CHECK(lines.size() >= 10);
    tui::HelpState h(lines.size());
    const auto top = render_modal(60, 15, nullptr, nullptr, &h);
    CHECK(has(top, "Keys"));
    CHECK(has(top, "filter notes"));
    CHECK(has(top, "Esc/F1/q close"));
    for (int i = 0; i < 20; ++i)
        h.handle(Key::PageDown);
    const auto bottom = render_modal(60, 15, nullptr, nullptr, &h);
    CHECK(has(bottom, "quit"));  // en alttaki satıra kaydırılınca ulaşılır
    CHECK(has(bottom, "Esc/F1/q close"));
}

TEST_CASE("help is available in Turkish") {
    core::init_i18n("tr");
    const auto lines = tui::help_lines();
    CHECK(lines.size() >= 10);
    tui::HelpState h(lines.size());
    CHECK(has(render_modal(60, 15, nullptr, nullptr, &h), "Tuşlar"));
    core::init_i18n("en");
}

// ---- eylemler (gerçek NoteStore) ----

TEST_CASE("move_note moves to a new folder and type") {
    const test::TempDir notes;
    core::NoteStore store(notes.path());
    REQUIRE(store.create({"work", "plan", "md", "içerik"}).has_value());
    const auto moved = tui::move_note(store, path("work/plan.md"), {"archive/2026", "plan-v2", "txt"});
    REQUIRE(moved.has_value());
    CHECK(moved->string() == "archive/2026/plan-v2.txt");
    CHECK(test::slurp(notes.path() / "archive" / "2026" / "plan-v2.txt") == "içerik");
    CHECK_FALSE(std::filesystem::exists(notes.path() / "work" / "plan.md"));
}

TEST_CASE("move_note onto an existing note fails and leaves both files") {
    const I18nEn en;
    const test::TempDir notes;
    core::NoteStore store(notes.path());
    REQUIRE(store.create({"", "a", "md", "A"}).has_value());
    REQUIRE(store.create({"", "b", "md", "B"}).has_value());
    const auto moved = tui::move_note(store, path("a.md"), {"", "b", "md"});
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error().code == core::Error::Code::AlreadyExists);
    CHECK(test::slurp(notes.path() / "a.md") == "A");
    CHECK(test::slurp(notes.path() / "b.md") == "B");
}

TEST_CASE("move_note to the same path is a no-op") {
    const test::TempDir notes;
    core::NoteStore store(notes.path());
    REQUIRE(store.create({"w", "a", "md", "A"}).has_value());
    const auto moved = tui::move_note(store, path("w/a.md"), {"w", "a", "md"});
    REQUIRE(moved.has_value());
    CHECK(*moved == path("w/a.md"));
    CHECK(test::slurp(notes.path() / "w" / "a.md") == "A");
}

TEST_CASE("delete removes only the confirmed note") {
    const test::TempDir notes;
    core::NoteStore store(notes.path());
    REQUIRE(store.create({"", "a", "md", "A"}).has_value());
    REQUIRE(store.create({"", "b", "md", "B"}).has_value());
    REQUIRE(store.remove(path("a.md")).has_value());
    CHECK_FALSE(std::filesystem::exists(notes.path() / "a.md"));
    CHECK(test::slurp(notes.path() / "b.md") == "B");
}
