// T78: REVIEW altındaki bağlantı bölümü ve `L` ilişkisi. Model saf; eylemler gerçek depoyla, çizim ekran
// metniyle.
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_session.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;
using tui::MouseButton;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;
constexpr int kFirstPaneRow = 2;

core::NotePath np(const std::string& text) {
    return core::NotePath::parse(text).value();
}

std::string row_of(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

std::string screen_text(const tui::TuiState& state, const tui::Preview& preview = {}, int width = kWidth,
                        int height = kHeight) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, preview, {"", ""}, {width, height}));
    std::string out;
    for (int y = 0; y < height; ++y)
        out += row_of(screen, y) + "\n";
    return out;
}

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

core::NoteLinks sample_links() {
    return core::NoteLinks{{np("o.md")}, {np("i1.md"), np("work/i2.md")}, {np("r.md")}, {"yok"}};
}

// Seçili not a.md, panelde örnek bağlantılar, odak REVIEW.
struct ModelFixture {
    tui::TuiState state;
    ModelFixture() {
        core::init_i18n("en");
        std::vector<core::NoteInfo> notes;
        for (const char* path : {"a.md", "i1.md", "o.md", "r.md", "work/i2.md"})
            notes.push_back(core::NoteInfo{np(path), std::filesystem::file_time_type{}, 1});
        state.set_notes(std::move(notes));
        state.set_window(kWidth, kHeight);
        REQUIRE(state.select_note(np("a.md")));
        state.set_links(sample_links(), 1, np("a.md"));
        state.handle(Key::Right);
        REQUIRE(state.focus() == Focus::Preview);
    }
    const core::NotePath* selected_link() const { return state.link_panel().selected_path(); }
};

struct StoreFixture {
    test::TempDir dir;
    test::TempDir state_home;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    int changes = 0;
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { refresh(); }, [this] { ++changes; }}};

    StoreFixture() {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "b.md", "b\n"));
        REQUIRE(test::seed_note(*store, "c.md", "c\n"));
        REQUIRE(test::seed_note(*store, "a.md", "[[b]] [[hedef]]\n"));
        state.set_window(kWidth, kHeight);
        refresh();
        REQUIRE(state.select_note(np("a.md")));
        load();
    }
    void refresh() {
        preview.invalidate();
        state.set_notes(store->list());
        state.set_folders(store->folders());
    }
    // Birleştirme süresi geçmiş gibi: sakin kullanıcı, okuma hemen olur.
    void load() {
        now += std::chrono::seconds(1);
        preview.throttled(state, *store, now);
    }
    void run(Intent intent) { actions.run(intent, confirm, rename); }
    std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
};

}  // namespace

TEST_CASE("link panel: b in REVIEW focuses the section, arrows skip headers, Esc and b return") {
    ModelFixture f;
    CHECK(f.state.handle(Key::Char, "b") == Intent::None);
    CHECK(f.state.focus() == Focus::Links);
    REQUIRE(f.selected_link() != nullptr);
    CHECK(f.selected_link()->string() == "i1.md");
    f.state.handle(Key::Down);
    CHECK(f.selected_link()->string() == "work/i2.md");
    f.state.handle(Key::Down);  // "Giden" başlığını atlar
    CHECK(f.selected_link()->string() == "o.md");
    f.state.handle(Key::Up);
    CHECK(f.selected_link()->string() == "work/i2.md");
    f.state.handle(Key::Escape);
    CHECK(f.state.focus() == Focus::Preview);
    f.state.handle(Key::Char, "b");
    CHECK(f.state.focus() == Focus::Links);
    f.state.handle(Key::Char, "b");
    CHECK(f.state.focus() == Focus::Preview);
}

TEST_CASE("link panel: b without links stays in REVIEW") {
    ModelFixture f;
    f.state.set_links({}, 2, np("a.md"));
    f.state.handle(Key::Char, "b");
    CHECK(f.state.focus() == Focus::Preview);
}

TEST_CASE("link panel: Enter jumps to the note and returns to REVIEW") {
    ModelFixture f;
    f.state.handle(Key::Char, "b");
    f.state.handle(Key::Down);
    CHECK(f.state.handle(Key::Enter) == Intent::None);
    CHECK(f.state.focus() == Focus::Preview);
    REQUIRE(f.state.selected_row() != nullptr);
    CHECK(f.state.selected_row()->path.string() == "work/i2.md");
}

TEST_CASE("link panel: Enter on a missing target goes nowhere and says why") {
    ModelFixture f;
    f.state.handle(Key::Char, "b");
    for (int i = 0; i < 10; ++i)
        f.state.handle(Key::Down);
    CHECK(f.selected_link() == nullptr);
    CHECK(f.state.handle(Key::Enter) == Intent::LinkMissing);
    CHECK(f.state.focus() == Focus::Links);
    CHECK(f.state.selected_row()->path.string() == "a.md");
}

TEST_CASE("link panel: a jump clears the filter hiding the target") {
    ModelFixture f;
    f.state.handle(Key::Left);
    f.state.handle(Key::Char, "/");
    f.state.handle(Key::Char, "a");
    f.state.handle(Key::Enter);
    f.state.handle(Key::Right);
    REQUIRE(f.state.focus() == Focus::Preview);
    f.state.handle(Key::Char, "b");
    f.state.handle(Key::Enter);
    CHECK(f.state.selected_row()->path.string() == "i1.md");
    CHECK(f.state.query().empty());
}

TEST_CASE("link panel: the section sits under the preview with counts and keeps the view around it") {
    ModelFixture f;
    const std::string shown = screen_text(f.state, {}, kWidth, 33);  // 9 satır: taşma göstergesi yok
    CHECK(has(shown, "Incoming (2)"));
    CHECK(has(shown, "Outgoing (1)"));
    CHECK(has(shown, "Related (1)"));
    CHECK(has(shown, "Missing (1)"));
    CHECK(has(shown, "work/i2.md"));
    CHECK(has(shown, "[[yok]]"));
    CHECK(has(shown, "─"));
    // Çerçeve, toplamlar ve ipucu çubuğu yerinde.
    CHECK(has(shown, "REVIEW"));
    CHECK(has(shown, "note"));
    CHECK(has(shown, "b links"));
}

TEST_CASE("link panel: hidden when there are no links, the screen is the one without a section") {
    ModelFixture f;
    f.state.set_links({}, 2, np("a.md"));
    const std::string shown = screen_text(f.state);
    CHECK_FALSE(has(shown, "Incoming"));
    CHECK(f.state.links_reserved_rows(25) == 0);
}

TEST_CASE("link panel: no section over the editor and in the trash view") {
    ModelFixture f;
    CHECK(f.state.links_reserved_rows(25) > 0);
    f.state.enter_editor();
    CHECK(f.state.links_reserved_rows(25) == 0);
}

TEST_CASE("link panel: height is bounded at every window, the preview keeps its rows") {
    tui::LinkPanel panel;
    core::NoteLinks links;
    for (int i = 0; i < 5000; ++i)
        links.incoming.push_back(np("n" + std::to_string(10000 + i) + ".md"));
    panel.set(std::move(links), false);
    CHECK(panel.lines().size() == 5001);
    CHECK(panel.visible_rows(10) == tui::LinkPanel::kMinRows);
    CHECK(panel.visible_rows(25) == 8);
    CHECK(panel.visible_rows(500) == tui::LinkPanel::kMaxRows);
    CHECK(panel.reserved_rows(25) == 9);

    tui::LinkPanel small;
    small.set(core::NoteLinks{{np("o.md")}, {}, {}, {}}, false);
    CHECK(small.visible_rows(25) == 2);  // başlık ve tek öğe: boş satır ayrılmaz
}

TEST_CASE("link panel: thousands of links page by the window and draw only what is visible") {
    ModelFixture f;
    core::NoteLinks links;
    for (int i = 0; i < 5000; ++i)
        links.incoming.push_back(np("n" + std::to_string(10000 + i) + ".md"));
    f.state.set_links(links, 2, np("a.md"));
    f.state.handle(Key::Char, "b");
    const int page = f.state.link_panel().visible_rows(kHeight - tui::TuiState::kChromeLines);
    f.state.handle(Key::PageDown);
    CHECK(f.state.link_panel().selected() == 1 + static_cast<std::size_t>(page));
    for (int i = 0; i < 5000; ++i)
        f.state.handle(Key::PageDown);
    CHECK(f.state.link_panel().selected() == 5000);
    const std::string shown = screen_text(f.state);
    CHECK(has(shown, "n14999.md"));
    CHECK(has(shown, "5000/5000"));
    CHECK_FALSE(has(shown, "n10000.md"));
    f.state.handle(Key::PageUp);
    CHECK(f.state.link_panel().selected() == 5000 - static_cast<std::size_t>(page));
}

TEST_CASE("link panel: hostile names never reach the terminal raw") {
    ModelFixture f;
    core::NoteLinks links;
    links.missing.emplace_back("\x1b[31mkötü\xe2\x80\xae\x07\xff\xe2\x80\x8bson");
    f.state.set_links(links, 2, np("a.md"));
    const std::string shown = screen_text(f.state);
    CHECK(has(shown, "kötü"));
    CHECK_FALSE(has(shown, "\x1b"));
    CHECK_FALSE(has(shown, "\xe2\x80\xae"));
    CHECK_FALSE(has(shown, "\xe2\x80\x8b"));  // sıfır genişlikli boşluk da işaret olur
    CHECK(has(shown, "\xef\xbf\xbdson"));
    CHECK_FALSE(has(shown, "\x07"));
}

TEST_CASE("link panel: a very long name keeps its end visible on one row") {
    ModelFixture f;
    core::NoteLinks links;
    links.missing.emplace_back(std::string(400, 'x') + "son");
    f.state.set_links(links, 2, np("a.md"));
    CHECK(has(screen_text(f.state), "xson]]"));
}

TEST_CASE("link panel: the selection stays on a refresh of the same note and resets on another") {
    ModelFixture f;
    f.state.handle(Key::Char, "b");
    f.state.handle(Key::Down);
    f.state.set_links(sample_links(), 2, np("a.md"));
    CHECK(f.selected_link()->string() == "work/i2.md");
    f.state.set_links(sample_links(), 2, np("a.md"));
    f.state.set_links(sample_links(), 3, np("o.md"));
    CHECK(f.selected_link()->string() == "i1.md");
}

TEST_CASE("link panel: losing every link leaves the section focus") {
    ModelFixture f;
    f.state.handle(Key::Char, "b");
    f.state.set_links({}, 2, np("a.md"));
    CHECK(f.state.focus() == Focus::Preview);
}

TEST_CASE("link panel: L asks for a note from List and REVIEW, not on a folder") {
    ModelFixture f;
    CHECK(f.state.handle(Key::Char, "L") == Intent::LinkNote);
    f.state.handle(Key::Left);
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.handle(Key::Char, "L") == Intent::LinkNote);
    REQUIRE(f.state.select_folder("work"));
    CHECK(f.state.handle(Key::Char, "L") == Intent::None);
}

TEST_CASE("link panel: L on a related row removes, on any other row adds") {
    ModelFixture f;
    f.state.handle(Key::Char, "b");
    CHECK(f.state.handle(Key::Char, "L") == Intent::LinkNote);  // gelen satırı
    while (!f.selected_link() || f.selected_link()->string() != "r.md")
        f.state.handle(Key::Down);
    CHECK(f.state.handle(Key::Char, "L") == Intent::UnlinkSelected);
    f.state.handle(Key::Down);  // eksik satır
    CHECK(f.state.handle(Key::Char, "L") == Intent::LinkNote);
}

TEST_CASE("link panel: hint bars and help name the keys") {
    core::init_i18n("en");
    CHECK(has(core::tr("tui.key_hints_preview"), "b links"));
    CHECK(has(core::tr("tui.key_hints_preview"), "L relate"));
    CHECK(has(core::tr("tui.key_hints_list"), "L relate"));
    CHECK(has(core::tr("tui.key_hints_links"), "Enter open"));
    CHECK(has(core::tr("tui.help_body"), "Links"));
}

TEST_CASE("link panel: mouse click jumps, wheel scrolls the section, the preview above is untouched") {
    ModelFixture f;
    test::TempDir dir;
    core::NoteStore store(dir.path());
    std::mutex mutex;
    std::string clipboard;
    tui::EditorSession editor{tui::EditorHost{
        store, mutex, clipboard, tui::terminal_cell_width, 1024 * 1024, core::kDefaultSlugMaxLength,
        [](std::string_view) { return std::string("md"); }, [](const core::NotePath&) {}, [] {},
        [](std::optional<std::size_t>) {}, [](const core::Error&) {}, [] {}}};
    const int pane = kHeight - tui::TuiState::kChromeLines;
    const int reserved = f.state.links_reserved_rows(pane);
    REQUIRE(reserved > 0);
    const int first_link_y = kFirstPaneRow + pane - reserved + 1;  // ayırıcının altı
    const int x = kWidth - 5;

    // Önizleme satırına tıklama yalnız odaklar.
    tui::route_mouse({x, kFirstPaneRow, MouseButton::Left, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.selected_row()->path.string() == "a.md");
    CHECK(f.state.focus() == Focus::Preview);

    // Tekerlek bölümde seçimi gezer, önizlemeyi kaydırmaz.
    f.state.set_preview_line_count(100);
    tui::route_mouse({x, first_link_y, MouseButton::WheelDown, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.preview_scroll() == 0);
    CHECK(f.state.link_panel().selected() > 1);
    tui::route_mouse({x, first_link_y, MouseButton::WheelUp, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.link_panel().selected() == 1);

    // Başlık satırı gitmez, öğe satırı gider.
    tui::route_mouse({x, first_link_y, MouseButton::Left, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.selected_row()->path.string() == "a.md");
    CHECK(f.state.focus() == Focus::Links);
    tui::route_mouse({x, first_link_y + 2, MouseButton::Left, tui::MouseMotion::Press}, f.state, editor);
    CHECK(f.state.selected_row()->path.string() == "work/i2.md");
    CHECK(f.state.focus() == Focus::Preview);
}

TEST_CASE("link panel: the store's links arrive with the deferred preview read, not per key") {
    StoreFixture f;
    REQUIRE(f.state.link_panel().links().outgoing.size() == 1);
    CHECK(f.state.link_panel().links().outgoing[0].string() == "b.md");
    CHECK(f.state.link_panel().links().missing == std::vector<std::string>{"hedef"});

    // Hızlı gezinme: önizleme de bağlantı da eski kalır, son seçim tek okumayla gelir.
    REQUIRE(f.state.select_note(np("b.md")));
    f.preview.throttled(f.state, *f.store, f.now);
    REQUIRE(f.preview.deferred());
    CHECK(f.state.link_panel().links().outgoing.size() == 1);
    f.load();
    CHECK(f.state.link_panel().links().incoming.size() == 1);
    CHECK(f.state.link_panel().links().incoming[0].string() == "a.md");
    CHECK(f.state.link_panel().links().outgoing.empty());
}

TEST_CASE("link panel: L adds a relation through the input with Tab completion, errors stay in the input") {
    StoreFixture f;
    f.run(Intent::LinkNote);
    REQUIRE(f.rename.has_value());
    CHECK(f.rename->is_link());
    CHECK(f.rename->text().empty());

    f.rename->handle(Key::Char, "c");
    f.rename->handle(Key::Tab);
    CHECK(f.rename->text() == "c.md");

    f.rename->handle(Key::Char, "x");
    f.actions.handle_rename_key(f.rename, Key::Enter, {});
    REQUIRE(f.rename.has_value());
    CHECK(has(f.rename->error(), "c.mdx"));
    CHECK(f.changes == 0);

    f.rename->handle(Key::Backspace);
    f.actions.handle_rename_key(f.rename, Key::Enter, {});
    CHECK_FALSE(f.rename.has_value());
    CHECK(f.changes == 1);
    f.load();
    CHECK(f.state.link_panel().links().related.size() == 1);
    CHECK(f.state.link_panel().links().related[0].string() == "c.md");
}

TEST_CASE("link panel: Esc in the L input writes nothing") {
    StoreFixture f;
    f.run(Intent::LinkNote);
    f.rename->handle(Key::Char, "c");
    f.actions.handle_rename_key(f.rename, Key::Escape, {});
    CHECK_FALSE(f.rename.has_value());
    CHECK(f.store->links(np("a.md"))->related.empty());
}

TEST_CASE("link panel: removing a relation asks first and Cancel is the default") {
    StoreFixture f;
    REQUIRE(f.store->add_relation(np("a.md"), np("c.md")).has_value());
    f.refresh();
    f.load();
    f.state.handle(Key::Right);
    f.state.handle(Key::Char, "b");
    while (!f.state.link_panel().selected_path() ||
           f.state.link_panel().selected_line()->group != core::LinkGroup::Related)
        f.state.handle(Key::Down);
    REQUIRE(f.state.handle(Key::Char, "L") == Intent::UnlinkSelected);

    f.run(Intent::UnlinkSelected);
    REQUIRE(f.confirm.has_value());
    CHECK_FALSE(f.confirm->yes_selected());
    CHECK(has(f.confirm->message(), "c.md"));
    CHECK(f.confirm->handle(Key::Enter) == tui::ConfirmState::Outcome::Cancelled);
    f.confirm.reset();
    CHECK(f.store->links(np("a.md"))->related.size() == 1);

    // Vazgeçilen hedef sonraki onaya taşınmaz: `D` notu silmeye gider, ilişkiyi değil.
    f.state.handle(Key::Escape);
    f.state.handle(Key::Left);
    f.run(Intent::DeleteSelected);
    REQUIRE(f.confirm.has_value());
    f.actions.finish_delete(f.confirm);
    CHECK_FALSE(f.store->read(np("a.md")).has_value());
}

TEST_CASE("link panel: a confirmed removal deletes the relation and notifies sync") {
    StoreFixture f;
    REQUIRE(f.store->add_relation(np("c.md"), np("a.md")).has_value());
    f.refresh();
    f.load();
    f.state.handle(Key::Right);
    f.state.handle(Key::Char, "b");
    while (!f.state.link_panel().selected_path() ||
           f.state.link_panel().selected_line()->group != core::LinkGroup::Related)
        f.state.handle(Key::Down);
    f.run(Intent::UnlinkSelected);
    REQUIRE(f.confirm.has_value());
    f.actions.finish_delete(f.confirm);
    CHECK_FALSE(f.confirm.has_value());
    CHECK(f.store->links(np("a.md"))->related.empty());
    CHECK(f.changes == 1);
    f.load();
    CHECK(f.state.link_panel().links().related.empty());
}

TEST_CASE("link panel: the status line explains a missing target") {
    StoreFixture f;
    f.run(Intent::LinkMissing);
    CHECK_FALSE(f.status.message.empty());
}

TEST_CASE("link panel: a trashed selection has no section") {
    StoreFixture f;
    REQUIRE_FALSE(f.state.link_panel().empty());
    f.state.enter_trash({});
    f.load();
    CHECK(f.state.link_panel().empty());
}

// Yalnız ilişki değiştiren senkron: liste, çöp ve klasörler aynıdır ama bölüm eski kalmamalı. Yeniden tarama
// bu karşılaştırmayla önizlemeyi (bağlantılarıyla) tazeler.
TEST_CASE("link panel: the rescan sees a sync that only changes relations") {
    const test::TempDir dir;
    const core::NoteStore tui_store(dir.path());
    {
        core::NoteStore seed(dir.path());
        REQUIRE(test::seed_note(seed, "a.md", "a\n"));
        REQUIRE(test::seed_note(seed, "b.md", "b\n"));
    }
    const tui::StoreScan before = tui::scan_store(tui_store);
    CHECK(tui::same_scan(before, tui::scan_store(tui_store)));  // değişmeyen depo tazelenmez
    {
        core::NoteStore sync_process(dir.path());  // `note sync` ayrı süreçtir: ayrı bağlantı
        REQUIRE(sync_process.add_relation(np("a.md"), np("b.md")).has_value());
    }
    const tui::StoreScan after = tui::scan_store(tui_store);
    CHECK_FALSE(tui::same_scan(before, after));
    CHECK(tui::same_scan(after, tui::scan_store(tui_store)));
}
