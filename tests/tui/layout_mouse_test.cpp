#include "core/i18n.hpp"
#include "edit/editor_session.hpp"
#include "edit/note_find.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/mouse.hpp"
#include "tui/pane_layout.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Focus;
using tui::Key;
using tui::MouseButton;
using tui::MouseMotion;
using tui::PaneLayout;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;

struct Fixture {
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    tui::TuiState state;
    tui::EditorSession editor{
        tui::EditorHost{store, mutex, clipboard, tui::terminal_cell_width, 1024 * 1024,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("md"); },
                        [](const core::NotePath&) {}, [this] { state.enter_editor(); },
                        [this](std::optional<std::size_t> line) { state.leave_editor(line); },
                        [](const core::Error&) {}, [] {}}};

    Fixture() {
        core::init_i18n("en");
        for (const char* name : {"a.md", "b.md", "c.md"})
            test::upsert_note(dir.path(), name, "metin\n");
        state.set_notes(store.list());
        state.set_window(kWidth, kHeight);
        state.set_preview_line_count(100);
    }

    void mouse(int x, int y, MouseButton button, MouseMotion motion = MouseMotion::Press) {
        tui::route_mouse({x, y, button, motion}, state, editor);
    }
    int divider() const { return state.layout().list_width(kWidth); }
    void open_editor() {
        editor.open(state.selected_row()->path);
        REQUIRE(state.focus() == Focus::Editor);
    }
};

constexpr int kRowY = 2;  // ilk liste satırı

}  // namespace

// Kullanıcı isteği (NY-T): ilk açılışta iki panel eşit değil, sol liste bariz dar (tuning.tui_list_share).
TEST_CASE("pane layout: the default list is a third, clearly narrower than the right pane, also at 60 columns") {
    PaneLayout layout;
    CHECK(layout.list_width(100) == 33);
    CHECK(layout.list_width(60) == 19);
    for (const int width : {60, 80, 100, 160, 240}) {
        CAPTURE(width);
        const tui::PaneGeometry geometry = layout.geometry(width, false);
        CHECK(geometry.list_width * 3 / 2 <= geometry.right_width);
    }
    for (int i = 0; i < 20; ++i)
        layout.resize(-PaneLayout::kKeyStep, 60);
    CHECK(layout.list_width(60) == PaneLayout::kMinListWidth);
    for (int i = 0; i < 20; ++i)
        layout.resize(PaneLayout::kKeyStep, 60);
    CHECK(layout.list_width(60) == 60 - PaneLayout::kMinRightWidth);
}

TEST_CASE("pane layout: the share comes from config, stays within the bounds and a chosen width wins") {
    PaneLayout layout;
    layout.set_share(0.5);
    CHECK(layout.list_width(100) == 50);
    layout.set_share(0.0);
    CHECK(layout.list_width(100) == PaneLayout::kMinListWidth);
    layout.set_share(1.0);
    CHECK(layout.list_width(100) == 100 - PaneLayout::kMinRightWidth);
    layout.set_list_width(40, 100);  // sürükleme ya da `<` `>`
    layout.set_share(0.25);          // ayar yeniden okununca da kullanıcının genişliği kalır
    CHECK(layout.list_width(100) == 40);
}

TEST_CASE("pane layout: a shrinking window pulls a wide list back so both panes stay usable") {
    PaneLayout layout;
    layout.set_list_width(150, 200);
    CHECK(layout.list_width(200) == 150);
    CHECK(layout.list_width(60) == 60 - PaneLayout::kMinRightWidth);
}

// NY-T3: seçilen genişlik tui-state.toml'a pencere oranı olarak yazılır. Aynı pencerede aynı sütuna dönmeli
// (oran tamsayıya kesilir: kayan nokta yuvarlaması bir sütun kaydırmasın).
TEST_CASE("pane layout: a chosen width becomes a share that gives the same column back in the same window") {
    PaneLayout layout;
    CHECK_FALSE(layout.chosen_share(100));  // seçilmedi: config'in oranı geçerli
    for (const int window : {60, 80, 99, 100, 137, 240, 317}) {
        for (int width = PaneLayout::kMinListWidth; width <= window - PaneLayout::kMinRightWidth; ++width) {
            CAPTURE(window);
            CAPTURE(width);
            layout.set_list_width(width, window);
            const std::optional<double> share = layout.chosen_share(window);
            REQUIRE(share);
            PaneLayout next;
            next.set_share(*share);
            CHECK(next.list_width(window) == width);
        }
    }
    CHECK_FALSE(layout.chosen_share(0));
}

TEST_CASE("dragging the divider marks the width for saving once, when the drag ends") {
    Fixture f;
    f.state.take_prefs_changed();
    const int start = f.divider();
    f.mouse(start, 5, MouseButton::Left, MouseMotion::Press);
    f.mouse(start + 4, 5, MouseButton::Left, MouseMotion::Move);
    CHECK_FALSE(f.state.take_prefs_changed());  // her hareket dosya yazmaz
    f.mouse(start + 4, 5, MouseButton::Left, MouseMotion::Release);
    CHECK(f.state.take_prefs_changed());
    REQUIRE(f.state.list_prefs().list_share);
    PaneLayout next;
    next.set_share(*f.state.list_prefs().list_share);
    CHECK(next.list_width(kWidth) == start + 5);
}

TEST_CASE("pane layout: zoom shows only the focused pane; the divider is gone") {
    PaneLayout layout;
    layout.toggle_zoom();
    const auto left = layout.geometry(100, false);
    CHECK(left.list_visible);
    CHECK_FALSE(left.right_visible);
    CHECK(left.list_width == 100);
    const auto right = layout.geometry(100, true);
    CHECK_FALSE(right.list_visible);
    CHECK(right.right_visible);
    CHECK(right.right_width == 100);
    CHECK_FALSE(layout.on_divider(40, 100));
}

namespace {

ftxui::Screen draw(const tui::TuiState& state, const tui::Preview& preview, int width, int height) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui::render(state, preview, {"", ""}, {width, height}));
    return screen;
}

tui::Preview text_preview(std::string line) {
    tui::Preview preview;
    preview.kind = tui::Preview::Kind::Text;
    preview.lines = {std::move(line)};
    return preview;
}

}  // namespace

// ui-redesign §5.5: içerik çerçeveye yapışmaz; sol kenarın içinde bir boş sütun kalır.
TEST_CASE("layout: list and right pane content start one blank column inside the left frame") {
    Fixture f;
    const tui::Preview preview = text_preview("metin");
    for (const auto& [width, height] :
         {std::pair{kWidth, kHeight}, std::pair{tui::kMinWidth, tui::kMinHeight}}) {
        CAPTURE(width);
        f.state.set_window(width, height);
        const ftxui::Screen screen = draw(f.state, preview, width, height);
        const int right_x = f.state.layout().list_width(width);
        CHECK(screen.PixelAt(0, kRowY).character == "│");
        CHECK(screen.PixelAt(1, kRowY).character == " ");
        CHECK(screen.PixelAt(2, kRowY).character == " ");  // işaret sütunu
        CHECK(screen.PixelAt(3, kRowY).character == "1");  // satır numarası
        CHECK(screen.PixelAt(right_x, kRowY).character == "│");
        CHECK(screen.PixelAt(right_x + 1, kRowY).character == " ");
        CHECK(screen.PixelAt(right_x + 2, kRowY).character == "m");
        // Sağ kenar yerinde: panel taşmaz, 60 sütunda da sığar.
        CHECK(screen.PixelAt(width - 1, kRowY).character == "│");
    }
}

TEST_CASE("find_folded: Turkish folding, byte offsets in the original text, across lines") {
    const std::string text = "Işık\nİŞIK ışık\nyok";
    const auto matches = tui::find_folded(text, "ışık");
    REQUIRE(matches.size() == 3);
    CHECK(text.substr(matches[0].begin, matches[0].end - matches[0].begin) == "Işık");
    CHECK(text.substr(matches[1].begin, matches[1].end - matches[1].begin) == "İŞIK");
    CHECK(text.substr(matches[2].begin, matches[2].end - matches[2].begin) == "ışık");
    CHECK(tui::find_folded(text, "").empty());
    CHECK(tui::find_folded(text, "zzz").empty());
    CHECK(tui::find_folded("", "x").empty());
}

TEST_CASE("find_folded: overlapping text advances past each match and CRLF stays out of the match") {
    CHECK(tui::find_folded("aaaa", "aa").size() == 2);
    const std::string crlf = "son\r\nson";
    const auto matches = tui::find_folded(crlf, "son");
    REQUIRE(matches.size() == 2);
    CHECK(matches[1].begin == 5);
}

TEST_CASE("mouse: dragging the divider resizes within bounds and release ends the drag") {
    Fixture f;
    const int start = f.divider();
    f.mouse(start, 5, MouseButton::Left);
    REQUIRE(f.state.layout().dragging());
    f.mouse(50, 5, MouseButton::Left, MouseMotion::Move);
    CHECK(f.divider() == 51);
    f.mouse(0, 5, MouseButton::Left, MouseMotion::Move);
    CHECK(f.divider() == PaneLayout::kMinListWidth);
    f.mouse(99, 5, MouseButton::Left, MouseMotion::Move);
    CHECK(f.divider() == kWidth - PaneLayout::kMinRightWidth);
    f.mouse(99, 5, MouseButton::Left, MouseMotion::Release);
    CHECK_FALSE(f.state.layout().dragging());
    const int kept = f.divider();
    f.mouse(30, 5, MouseButton::Other, MouseMotion::Move);
    CHECK(f.divider() == kept);
    CHECK(f.state.focus() == Focus::List);
}

// Release, modal açıkken main'de düşer ya da terminalde kaybolur: sürükleme asılı kalıp bölücüyü taşımasın.
TEST_CASE("mouse: a move with no button after a lost release ends the drag and does not resize") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    f.mouse(50, 5, MouseButton::Left, MouseMotion::Move);
    REQUIRE(f.divider() == 51);
    f.mouse(30, 5, MouseButton::Other, MouseMotion::Move);
    CHECK_FALSE(f.state.layout().dragging());
    CHECK(f.divider() == 51);
}

TEST_CASE("mouse: a press after a lost release ends the drag and acts as a normal click") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    const int width = f.divider();
    const core::NotePath first = f.state.selected_row()->path;
    f.mouse(5, kRowY + 1, MouseButton::Left);
    CHECK_FALSE(f.state.layout().dragging());
    CHECK(f.divider() == width);
    CHECK(f.state.selected_row()->path != first);
}

TEST_CASE("mouse: a press on the divider after a lost release starts a fresh drag without resizing") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    const int width = f.divider();
    f.mouse(width, 5, MouseButton::Left);
    CHECK(f.state.layout().dragging());
    CHECK(f.divider() == width);
}

TEST_CASE("mouse: the wheel after a lost release does not resize") {
    Fixture f;
    f.mouse(f.divider(), 5, MouseButton::Left);
    const int width = f.divider();
    f.mouse(5, 5, MouseButton::WheelDown);
    CHECK_FALSE(f.state.layout().dragging());
    CHECK(f.divider() == width);
}

TEST_CASE("mouse: the divider can be dragged from the editor without touching the text") {
    Fixture f;
    f.open_editor();
    f.editor.handle(Key::Char, "x");
    f.mouse(f.divider(), 5, MouseButton::Left);
    f.mouse(30, 5, MouseButton::Left, MouseMotion::Move);
    f.mouse(30, 5, MouseButton::Left, MouseMotion::Release);
    CHECK(f.divider() == 31);
    CHECK(f.state.focus() == Focus::Editor);
    CHECK(f.editor.buffer()->text() == "xmetin\n");
}

TEST_CASE("mouse: wheel over the list moves the selection, over the right pane scrolls the preview") {
    Fixture f;
    f.mouse(5, 5, MouseButton::WheelDown);
    CHECK(f.state.selected() == 1);
    CHECK(f.state.focus() == Focus::List);
    f.mouse(5, 5, MouseButton::WheelUp);
    CHECK(f.state.selected() == 0);
    f.mouse(70, 5, MouseButton::WheelDown);
    CHECK(f.state.preview_scroll() == 3);
    CHECK(f.state.selected() == 0);
    f.mouse(70, 5, MouseButton::WheelUp);
    CHECK(f.state.preview_scroll() == 0);
}

TEST_CASE("mouse: wheel in the editor moves in the text, over the list it does not change the selection") {
    Fixture f;
    f.open_editor();
    f.mouse(5, 5, MouseButton::WheelDown);
    CHECK(f.state.selected() == 0);
    f.mouse(70, 5, MouseButton::WheelDown);
    CHECK(f.editor.buffer()->position().line == 1);  // metin iki satır: sonda durur
}

TEST_CASE("mouse: clicking a list row selects it and focuses the list; a click below the rows is harmless") {
    Fixture f;
    f.state.handle(Key::Right);
    REQUIRE(f.state.focus() == Focus::Preview);
    f.mouse(5, kRowY + 2, MouseButton::Left);
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.selected() == 2);
    f.mouse(5, kRowY + 20, MouseButton::Left);
    CHECK(f.state.selected() == 2);
    f.mouse(5, 1, MouseButton::Left);  // çerçeve
    CHECK(f.state.selected() == 2);
}

TEST_CASE("mouse: a click on the right pane focuses Preview") {
    Fixture f;
    f.mouse(70, 5, MouseButton::Left);
    CHECK(f.state.focus() == Focus::Preview);
    f.state.handle(Key::Left);
    f.state.handle(Key::Char, "/");
    REQUIRE(f.state.focus() == Focus::Search);
    f.mouse(70, 5, MouseButton::Left);
    CHECK(f.state.focus() == Focus::Preview);
}

// GUI ve Ctrl+K gibi: tıklama editörü kapatmaz, soru da sormaz; kirli tampon notuna yazılır.
TEST_CASE("mouse: clicking the list keeps the editor open and focuses the list, a dirty buffer is saved") {
    Fixture f;
    f.open_editor();
    const core::NotePath opened = f.state.selected_row()->path;
    f.mouse(5, kRowY + 1, MouseButton::Left);
    CHECK(f.editor.is_open());
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.selected() == 1);

    f.state.enter_editor();
    f.editor.handle(Key::Char, "x");
    f.mouse(5, kRowY, MouseButton::Left);
    CHECK(f.editor.is_open());
    CHECK(f.editor.mode() == tui::EditorSession::Mode::Editing);
    CHECK_FALSE(f.editor.dirty());
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.selected() == 0);
    CHECK(f.store.read(opened)->text == "xmetin\n");
}

// Liste satırına tıklama gibi: bekleyen düzenleme notuna yazılır, tuşlar sonra listeye gider.
TEST_CASE("mouse: a breadcrumb click from the editor saves the note and moves keyboard focus to the list") {
    Fixture f;
    f.open_editor();
    const core::NotePath opened = f.state.selected_row()->path;
    f.editor.handle(Key::Char, "x");
    int x = 0;
    while (x < kWidth && !tui::breadcrumb_folder_at(f.state, x))
        ++x;
    REQUIRE(x < kWidth);
    f.mouse(x, 0, MouseButton::Left);
    CHECK(f.editor.is_open());
    CHECK_FALSE(f.editor.dirty());
    CHECK(f.store.read(opened)->text == "xmetin\n");
    CHECK(f.state.focus() == Focus::List);
}

TEST_CASE(
    "mouse: a trash indicator click from the editor saves the note and moves keyboard focus to the list") {
    Fixture f;
    f.state.set_trash_count(2);
    f.open_editor();
    const core::NotePath opened = f.state.selected_row()->path;
    f.editor.handle(Key::Char, "x");
    REQUIRE_FALSE(tui::trash_indicator(f.state).empty());
    CHECK(tui::route_mouse({0, 0, MouseButton::Left, MouseMotion::Press}, f.state, f.editor) ==
          tui::MouseResult::OpenTrash);
    CHECK(f.editor.is_open());
    CHECK_FALSE(f.editor.dirty());
    CHECK(f.store.read(opened)->text == "xmetin\n");
    // Çöp okunamasa da (ListActions iletiyi gösterir) tuşlar editöre dönmez.
    CHECK(f.state.focus() == Focus::List);

    // Kaydedilemeyen tampon (soru açık) tıklamayı yutar: odak editörde kalır.
    Fixture busy;
    busy.state.set_trash_count(1);
    busy.open_editor();
    busy.editor.handle(Key::CtrlF);
    REQUIRE(busy.editor.mode() != tui::EditorSession::Mode::Editing);
    CHECK(tui::route_mouse({0, 0, MouseButton::Left, MouseMotion::Press}, busy.state, busy.editor) ==
          tui::MouseResult::None);
    CHECK(busy.state.focus() == Focus::Editor);
}

TEST_CASE("mouse: an open prompt or input ignores the pointer") {
    Fixture f;
    f.open_editor();
    f.editor.handle(Key::Char, "x");
    f.editor.handle(Key::CtrlQ);
    f.mouse(5, kRowY + 1, MouseButton::Left);
    f.mouse(70, 5, MouseButton::WheelDown);
    CHECK(f.editor.mode() == tui::EditorSession::Mode::LeavePrompt);
    CHECK(f.state.selected() == 0);
}

TEST_CASE("mouse: zoomed, the whole width belongs to the focused pane") {
    Fixture f;
    f.state.layout().toggle_zoom();
    f.mouse(90, kRowY + 1, MouseButton::Left);  // Liste odaklı ve tam genişlik
    CHECK(f.state.selected() == 1);
    CHECK(f.state.focus() == Focus::List);
}

TEST_CASE("mouse: a click in the editor puts the cursor at that cell, the gutter is not text") {
    Fixture f;
    test::upsert_note(f.dir.path(), "a.md", "birinci\nikinci\nson\n");
    // Seçili not mtime sırasına, o da saat tıkına bağlıdır: not adıyla açılır.
    f.editor.open(core::NotePath::parse("a.md").value());
    REQUIRE(f.state.focus() == Focus::Editor);
    const auto viewport = tui::editor_viewport({kWidth, kHeight}, f.state.layout());
    f.editor.layout(viewport.columns, viewport.rows);
    const int text_x = f.divider() + 2 + f.editor.gutter_width();  // kenar ve iç boşluk
    f.mouse(text_x + 2, kRowY + 1, MouseButton::Left);
    CHECK(f.editor.buffer()->position().line == 1);
    CHECK(f.editor.buffer()->position().column == 2);
    CHECK(f.state.focus() == Focus::Editor);
    // Numara sütununa tıklama satır başıdır, sayı metin sütunu sayılmaz.
    f.mouse(f.divider() + 2, kRowY + 2, MouseButton::Left);
    CHECK(f.editor.buffer()->position().line == 2);
    CHECK(f.editor.buffer()->position().column == 0);
    // Kenarın içindeki boşluk da satır başıdır, tıklama yutulmaz.
    f.mouse(f.divider() + 1, kRowY, MouseButton::Left);
    CHECK(f.editor.buffer()->position().line == 0);
    CHECK(f.editor.buffer()->position().column == 0);
    // Dosya sonunun ötesi son satıra iner, metin değişmez.
    f.mouse(text_x + 40, kRowY + 20, MouseButton::Left);
    CHECK(f.editor.buffer()->position().line == 3);
    CHECK_FALSE(f.editor.dirty());
}

TEST_CASE("mouse: the editor's two bottom bars are not text rows") {
    Fixture f;
    f.open_editor();
    const auto viewport = tui::editor_viewport({kWidth, kHeight}, f.state.layout());
    f.editor.layout(viewport.columns, viewport.rows);
    const auto before = f.editor.buffer()->cursor();
    f.mouse(f.divider() + 5, kHeight - 2, MouseButton::Left);  // durum çubuğu
    f.mouse(f.divider() + 5, kHeight - 3, MouseButton::Left);  // panelin alt kenarı
    CHECK(f.editor.buffer()->cursor() == before);
}

namespace {

std::set<std::string_view> menu_ids(const tui::TuiState& state) {
    std::set<std::string_view> ids;
    for (const tui::PaletteRow& row : state.palette_rows())
        ids.insert(row.command->id);
    return ids;
}

}  // namespace

TEST_CASE("mouse: right click on a note row opens the GUI's note menu for that row") {
    Fixture f;
    f.mouse(10, kRowY + 1, MouseButton::Right);
    REQUIRE(f.state.palette_open());
    CHECK(f.state.selected() == 1);
    CHECK(f.state.focus() == Focus::List);
    CHECK(menu_ids(f.state) == std::set<std::string_view>{"tab.open", "move", "trash", "delete"});
    while (f.state.palette_selected()->id != "trash")
        f.state.handle(Key::Down);
    CHECK(f.state.handle(Key::Enter) == tui::Intent::TrashSelected);
    CHECK_FALSE(f.state.palette_open());
    // Yazılan süzgeç menünün dışına çıkmaz; Esc kapatır.
    f.mouse(10, kRowY, MouseButton::Right);
    f.state.handle(Key::Char, "s");
    f.state.handle(Key::Char, "e");
    for (const std::string_view id : menu_ids(f.state))
        CHECK((id == "move" || id == "trash" || id == "delete"));
    f.state.handle(Key::Escape);
    CHECK_FALSE(f.state.palette_open());
    f.state.handle(Key::Char, ":");
    CHECK(menu_ids(f.state).size() > 10);  // sonraki palet yine tümünü gösterir
}

TEST_CASE("mouse: right click on a folder row opens the folder menu; an unmarked row drops the marks") {
    Fixture f;
    test::upsert_note(f.dir.path(), "work/d.md", "x\n");
    f.state.set_notes(f.store.list());
    f.state.set_folders(f.store.folders());
    REQUIRE(f.state.rows().front().is_folder);
    f.state.select_index(1);
    f.state.handle(Key::Char, "v");
    REQUIRE(f.state.marked_count() == 1);
    f.mouse(10, kRowY, MouseButton::Right);
    REQUIRE(f.state.palette_open());
    CHECK(f.state.selected() == 0);
    CHECK(f.state.marked_count() == 0);
    CHECK(menu_ids(f.state) == std::set<std::string_view>{"folder.new", "rename", "trash"});
}
