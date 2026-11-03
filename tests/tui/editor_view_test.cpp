#include "core/config.hpp"
#include "core/i18n.hpp"
#include "edit/editor_session.hpp"
#include "edit/syntax.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/modals.hpp"
#include "tui/pane_layout.hpp"
#include "tui/styled_view.hpp"
#include "tui/termination.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <csignal>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::EditorSession;
using tui::Focus;
using tui::Key;
namespace fs = std::filesystem;

namespace {

constexpr int kWidth = 80;
constexpr int kHeight = 24;
constexpr std::size_t kLimit = 4 * 1024 * 1024;

struct Fixture {
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    tui::TuiState state;
    EditorSession session{
        tui::EditorHost{store, mutex, clipboard, tui::terminal_cell_width, kLimit,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("md"); },
                        [](const core::NotePath&) {}, [this] { state.enter_editor(); },
                        [this](std::optional<std::size_t> line) { state.leave_editor(line); },
                        [](const core::Error&) {}, [] {}}};

    explicit Fixture(std::string_view content, const char* language = "en",
                     const std::string& path = "a.md") {
        core::init_i18n(language);
        test::upsert_note(dir.path(), path, content);
        state.set_notes(store.list());
        session.open(state.selected_row()->path);
        REQUIRE(session.is_open());
    }

    ftxui::Screen frame(int width = kWidth, int height = kHeight) {
        const auto viewport = tui::editor_viewport({width, height}, state.layout());
        session.layout(viewport.columns, viewport.rows);
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, tui::render(state, {}, {"sync off", ""}, {width, height}, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, &session));
        return screen;
    }
};

std::string row_text(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    return out;
}

bool contains(const ftxui::Screen& screen, std::string_view needle) {
    for (int y = 0; y < screen.dimy(); ++y)
        if (row_text(screen, y).find(needle) != std::string::npos)
            return true;
    return false;
}

// Sağ panelin ilk metin hücresi: sol panel + çerçeve kenarı + numara sütunu; üst çubuk ve üst kenar iki
// satır.
constexpr int kTestGutter = 3;
int pane_x(int width) {
    return width * 2 / 5 + 1 + kTestGutter;
}
constexpr int kPaneY = 2;

}  // namespace

TEST_CASE("editor view: the focused pane is heavy, the list is light, the selection is not inverted") {
    Fixture f("metin");
    const auto screen = f.frame();
    CHECK(screen.PixelAt(0, 1).character == "┌");
    CHECK(screen.PixelAt(kWidth * 2 / 5, 1).character == "┏");
    CHECK(contains(screen, " 1   a.md"));
    CHECK_FALSE(screen.PixelAt(2, kPaneY).inverted);
}

TEST_CASE("editor view: the terminal cursor follows multi-byte text and no cell is inverted") {
    Fixture f("ığş");
    auto screen = f.frame();
    CHECK(screen.PixelAt(pane_x(kWidth), kPaneY).character == "ı");
    CHECK(screen.cursor().x == pane_x(kWidth));
    CHECK_FALSE(screen.PixelAt(pane_x(kWidth), kPaneY).inverted);
    f.session.handle(Key::Right);
    screen = f.frame();
    CHECK(screen.PixelAt(pane_x(kWidth) + 1, kPaneY).character == "ğ");
    CHECK(screen.cursor().x == pane_x(kWidth) + 1);
    f.session.handle(Key::CtrlEnd);
    screen = f.frame();
    CHECK(screen.cursor().x == pane_x(kWidth) + 3);  // satır sonu imleci
}

TEST_CASE("editor view: a combining mark stays on its base cell") {
    Fixture f("s\xCC\xA7x");
    const auto screen = f.frame();
    CHECK(screen.PixelAt(pane_x(kWidth), kPaneY).character == "s\xCC\xA7");
    CHECK(screen.PixelAt(pane_x(kWidth) + 1, kPaneY).character == "x");
}

TEST_CASE("editor view: selection cells are inverted") {
    Fixture f("abcdef");
    f.session.handle(Key::ShiftRight);
    f.session.handle(Key::ShiftRight);
    const auto screen = f.frame();
    CHECK(screen.PixelAt(pane_x(kWidth), kPaneY).inverted);
    CHECK(screen.PixelAt(pane_x(kWidth) + 1, kPaneY).inverted);
    CHECK_FALSE(screen.PixelAt(pane_x(kWidth) + 3, kPaneY).inverted);
}

TEST_CASE("editor view: the status bar shows line:col, the dirty mark and hints") {
    Fixture f("bir\niki");
    auto screen = f.frame();
    CHECK(contains(screen, "1:1"));
    CHECK(contains(screen, "Ctrl+S save"));
    CHECK_FALSE(contains(screen, "a.md *"));
    f.session.handle(Key::Down);
    f.session.handle(Key::Char, "ş");
    screen = f.frame();
    CHECK(contains(screen, "2:2"));
    CHECK(contains(screen, "a.md *"));
}

TEST_CASE("editor view: control bytes never reach a screen cell and the file stays byte-exact") {
    const std::string content = "a\x1b[31mX\x07\x7F\xC2\x9B\r\nb\tc";
    Fixture f(content);
    const auto screen = f.frame();
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x < screen.dimx(); ++x)
            for (const char c : screen.PixelAt(x, y).character) {
                const auto byte = static_cast<unsigned char>(c);
                CHECK((byte >= 0x20 && byte != 0x7F));
            }
    CHECK(contains(screen, "[31mX"));
    CHECK(f.session.buffer()->text() == content);
    CHECK_FALSE(f.session.dirty());
    f.session.handle(Key::CtrlS);
    CHECK(test::note_body(f.dir.path(), "a.md") == content);
}

TEST_CASE("editor view: bidi and zero-width characters show as a warning marker, named under the cursor") {
    const std::string content = "a\xE2\x80\xAE"
                                "b\xE2\x80\x8B"
                                "c\n";  // a U+202E b U+200B c
    Fixture f(content);
    auto screen = f.frame();
    const int x = pane_x(kWidth);
    const std::string kMarker = "\xEF\xBF\xBD";
    CHECK(screen.PixelAt(x, kPaneY).character == "a");
    CHECK(screen.PixelAt(x + 1, kPaneY).character == kMarker);
    CHECK(screen.PixelAt(x + 2, kPaneY).character == "b");
    CHECK(screen.PixelAt(x + 3, kPaneY).character == kMarker);
    CHECK(screen.PixelAt(x + 4, kPaneY).character == "c");
    // Uyarı rengi: satırın arka planından ve düz metinden ayrı.
    CHECK(screen.PixelAt(x + 1, kPaneY).background_color == ftxui::Color(ftxui::Color::Red));
    CHECK(screen.PixelAt(x + 2, kPaneY).background_color != ftxui::Color(ftxui::Color::Red));
    for (int y = 0; y < screen.dimy(); ++y) {
        CHECK(row_text(screen, y).find("\xE2\x80\xAE") == std::string::npos);
        CHECK(row_text(screen, y).find("\xE2\x80\x8B") == std::string::npos);
    }
    CHECK_FALSE(contains(screen, "U+202E"));

    f.session.handle(Key::Right);
    screen = f.frame();
    CHECK(screen.cursor().x == x + 1);
    CHECK(contains(screen, "U+202E RIGHT-TO-LEFT OVERRIDE"));
    f.session.handle(Key::Right);
    f.session.handle(Key::Right);
    screen = f.frame();
    CHECK(screen.cursor().x == x + 3);
    CHECK(contains(screen, "U+200B ZERO WIDTH SPACE"));
    CHECK(contains(screen, "1:4"));
    f.session.handle(Key::Right);
    screen = f.frame();
    CHECK(screen.cursor().x == x + 4);
    CHECK_FALSE(contains(screen, "U+200B"));

    // Kaydetmek baytlara dokunmaz; işareti silmek yalnız onun baytlarını siler.
    f.session.handle(Key::CtrlS);
    CHECK(test::note_body(f.dir.path(), "a.md") == content);
    f.session.handle(Key::Home);
    f.session.handle(Key::Right);
    f.session.handle(Key::ShiftRight);
    f.session.handle(Key::Delete);
    f.session.handle(Key::CtrlS);
    CHECK(test::note_body(f.dir.path(), "a.md") == "ab\xE2\x80\x8B"
                                                   "c\n");
}

TEST_CASE("editor view: an emoji ZWJ sequence and a ZWNJ word draw without hidden-character markers") {
    const std::string content =
        "aile \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
        " \xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE\n";
    Fixture f(content);
    auto screen = f.frame();
    for (int y = 0; y < screen.dimy(); ++y) {
        CHECK(row_text(screen, y).find("\xEF\xBF\xBD") == std::string::npos);
        for (int x = 0; x < screen.dimx(); ++x)
            CHECK(screen.PixelAt(x, y).background_color != ftxui::Color(ftxui::Color::Red));
    }
    for (int step = 0; step < 12; ++step) {
        f.session.handle(Key::Right);
        CHECK_FALSE(contains(f.frame(), "Hidden character"));
    }
    f.session.handle(Key::CtrlS);
    CHECK(test::note_body(f.dir.path(), "a.md") == content);
}

// Yol senkrondan gelir: içerik gibi adı da güvenilmez. Sağdan sola geçersiz kılma ve UTF-8 C1 CSI.
constexpr std::string_view kHostileName = "gpj\xE2\x80\xAE"
                                          "x\xC2\x9B"
                                          "31m";

bool screen_has_raw(const ftxui::Screen& screen) {
    for (int y = 0; y < screen.dimy(); ++y) {
        const std::string row = row_text(screen, y);
        if (row.find("\xE2\x80\xAE") != std::string::npos || row.find("\xC2\x9B") != std::string::npos)
            return true;
    }
    return false;
}

TEST_CASE("editor view: a hostile note path never reaches the info bar, messages or the prompts raw") {
    const std::string path = std::string(kHostileName) + ".md";
    Fixture f("x", "en", path);
    auto screen = f.frame();
    CHECK(contains(screen, "gpj"));  // bilgi çubuğu yolu gösteriyor
    CHECK_FALSE(screen_has_raw(screen));

    f.session.handle(Key::Char, "y");
    f.session.handle(Key::CtrlQ);
    REQUIRE(f.session.mode() == EditorSession::Mode::LeavePrompt);
    CHECK_FALSE(screen_has_raw(f.frame()));
    f.session.handle(Key::Escape);

    test::upsert_note(f.dir.path(), path, "dış");
    f.session.handle(Key::CtrlS);
    REQUIRE(f.session.mode() == EditorSession::Mode::ConflictPrompt);
    CHECK_FALSE(screen_has_raw(f.frame()));
    f.session.handle(Key::Enter);  // kopya olarak kaydet: ileti yeni yolu söyler
    screen = f.frame();
    CHECK(contains(screen, "Saved as copy"));
    CHECK_FALSE(screen_has_raw(screen));
}

TEST_CASE("editor view: a hostile folder name never reaches the name prompt label raw") {
    Fixture f("metin");
    f.session.handle(Key::CtrlQ);
    f.state.open_new_note_view();
    f.session.begin_new_note(std::string(kHostileName));
    f.session.handle(Key::Char, "a");
    f.session.handle(Key::CtrlS);
    REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
    const auto screen = f.frame();
    CHECK(contains(screen, "gpj"));
    CHECK_FALSE(screen_has_raw(screen));
}

TEST_CASE("editor view: the cursor stays visible through a resize, also at the minimum size") {
    std::string text;
    for (int i = 0; i < 200; ++i)
        text += "satır-" + std::to_string(i) + "\n";
    Fixture f(text);
    f.session.handle(Key::CtrlEnd);
    f.session.handle(Key::Up);
    CHECK(contains(f.frame(120, 40), "satır-199"));
    auto small = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(contains(small, "satır-199"));
    CHECK(contains(small, "200:1"));
    CHECK_FALSE(contains(small, core::tr("tui.too_small")));
    CHECK(contains(f.frame(100, 30), "satır-199"));
}

TEST_CASE("editor view: long lines wrap inside the pane and the cursor follows") {
    Fixture f(std::string(98, 'x') + "son");  // 34 sütunda 34+34+33: "son" bölünmez
    f.session.handle(Key::CtrlEnd);
    const auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(contains(screen, "son"));
    const auto viewport = tui::editor_viewport({tui::kMinWidth, tui::kMinHeight}, f.state.layout());
    CHECK(viewport.columns > 0);
    CHECK(pane_x(tui::kMinWidth) - kTestGutter + viewport.columns == tui::kMinWidth - 1);
}

TEST_CASE("editor view: the status bar fits 60 columns in both languages without cutting a hint") {
    for (const char* language : {"en", "tr"}) {
        CAPTURE(language);
        Fixture f("x", language);
        const auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
        std::string bottom = row_text(screen, tui::kMinHeight - 1);
        while (!bottom.empty() && bottom.back() == ' ')
            bottom.pop_back();
        const auto hints = core::tr("tui.key_hints_editor");
        CHECK(ftxui::string_width(bottom) <= tui::kMinWidth);
        CHECK(hints.starts_with(bottom));
        CHECK((hints.size() == bottom.size() || hints.substr(bottom.size(), 2) == "  "));
    }
}

TEST_CASE("editor view: the leave prompt lists Save, Discard and Cancel with Save selected") {
    Fixture f("x");
    f.session.handle(Key::Char, "y");
    f.session.handle(Key::CtrlQ);
    const auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(contains(screen, core::tr("tui.editor_choice_save")));
    CHECK(contains(screen, core::tr("tui.discard_yes")));
    CHECK(contains(screen, core::tr("tui.discard_no")));
}

TEST_CASE("editor view: the leave prompt of an unsaved draft works and names the draft") {
    Fixture f("x");
    f.session.handle(Key::CtrlQ);
    f.session.begin_new_note("");
    f.session.handle(Key::Char, "z");
    f.session.handle(Key::CtrlQ);
    REQUIRE(f.session.mode() == EditorSession::Mode::LeavePrompt);
    CHECK(contains(f.frame(), core::tr("tui.editor_untitled")));

    Fixture unnamed("x");
    unnamed.session.handle(Key::CtrlQ);
    unnamed.session.begin_draft("sorgu\n", "");
    unnamed.session.handle(Key::CtrlQ);
    REQUIRE(unnamed.session.mode() == EditorSession::Mode::LeavePrompt);
    CHECK(contains(unnamed.frame(), core::tr("tui.editor_untitled")));
}

TEST_CASE("editor view: the conflict dialog offers all four choices at the minimum size") {
    Fixture f("x");
    f.session.handle(Key::Char, "y");
    test::upsert_note(f.dir.path(), "a.md", "dış");
    f.session.handle(Key::CtrlS);
    REQUIRE(f.session.mode() == EditorSession::Mode::ConflictPrompt);
    const auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    for (const char* key : {"tui.editor_choice_copy", "tui.editor_choice_load", "tui.editor_choice_overwrite",
                            "tui.discard_no"})
        CHECK(contains(screen, core::tr(key)));
}

TEST_CASE("editor view: the find bar shows the query; read-only shows the reason") {
    Fixture f("metin");
    f.session.handle(Key::CtrlF);
    f.session.handle(Key::Char, "m");
    const auto screen = f.frame();
    CHECK(contains(screen, core::tr("tui.find_label") + " m▏etin  1/1"));
    CHECK(screen.PixelAt(pane_x(kWidth), kPaneY).inverted);  // geçerli eşleşme seçili

    Fixture binary(std::string("ikili\0x", 7));
    CHECK(contains(binary.frame(), core::tr("document.readonly_binary")));
}

TEST_CASE("editor view: help names the editor keys, the external editor and the clipboard limit") {
    for (const char* language : {"en", "tr"}) {
        core::init_i18n(language);
        std::string help;
        for (const auto& line : tui::help_lines())
            help += line + "\n";
        CAPTURE(language);
        for (const char* needle :
             {"^S", "^Z", "^Y", "^X", "^C", "^V", "^F", "F3", "Enter", "$EDITOR", "OSC 52", "Shift+"})
            CHECK(help.find(needle) != std::string::npos);
    }
}

TEST_CASE("editor view: blank lines keep their row") {
    Fixture f("a\n\nb");
    const auto screen = f.frame();
    CHECK(screen.PixelAt(pane_x(kWidth), kPaneY + 2).character == "b");
}

TEST_CASE("editor view: find highlights every match, Turkish folded") {
    Fixture f("Işık bir ışık");
    f.session.handle(Key::CtrlF);
    for (const std::string_view letter : {"ı", "ş", "ı", "k"})
        f.session.handle(Key::Char, letter);
    const auto screen = f.frame();
    CHECK(screen.PixelAt(pane_x(kWidth), kPaneY).inverted);
    const int second = pane_x(kWidth) + 9;
    CHECK(screen.PixelAt(second, kPaneY).character == "ı");
    CHECK(screen.PixelAt(second, kPaneY).bold);
    CHECK(contains(screen, "1/2"));
}

TEST_CASE("new note view: the blank editor takes the whole terminal, list and top line hidden") {
    Fixture f("metin");
    f.session.handle(Key::CtrlQ);
    f.state.open_new_note_view();
    f.session.begin_new_note("");
    const auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(row_text(screen, 0).starts_with(core::tr("tui.new_note_header")));
    CHECK_FALSE(contains(screen, "a.md"));
    CHECK_FALSE(contains(screen, core::tr("tui.mode_name")));
    CHECK_FALSE(contains(screen, core::tr("tui.sync_off")));
    CHECK(screen.PixelAt(0, 1).character == "┏");
    CHECK(screen.PixelAt(tui::kMinWidth - 1, 1).character == "┓");
    CHECK(contains(screen, core::tr("tui.frame_edit")));
    CHECK(f.session.columns() == tui::kMinWidth - 2 - f.session.gutter_width());
    CHECK(row_text(screen, tui::kMinHeight - 1).find("Ctrl+S save") != std::string::npos);
}

TEST_CASE("new note view: the name prompt is a bottom input with the draft still on screen") {
    Fixture f("metin");
    f.session.handle(Key::CtrlQ);
    f.state.open_new_note_view();
    f.session.begin_new_note("work");
    for (const char* letter : {"D", "B", "=", "1", "\n"})
        f.session.handle(*letter == '\n' ? Key::Enter : Key::Char, letter);
    f.session.handle(Key::Char, "B");
    f.session.handle(Key::Char, "=");
    f.session.handle(Key::Char, "2");
    f.session.handle(Key::CtrlS);
    REQUIRE(f.session.mode() == EditorSession::Mode::NamePrompt);
    const auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    const std::string bottom = row_text(screen, tui::kMinHeight - 1);
    CHECK(bottom.starts_with(core::tr("tui.new_note_label") + " work/ "));
    CHECK(contains(screen, "DB=1"));
}

TEST_CASE("new note view: closing returns to the normal two panes with the note selected in the list") {
    Fixture f("metin");
    f.session.handle(Key::CtrlQ);
    f.state.open_new_note_view();
    f.session.begin_new_note("");
    CHECK(f.state.new_note_view());
    CHECK(f.state.layout().zoomed());
    f.session.handle(Key::Char, "y");
    f.session.handle(Key::CtrlS);
    f.session.handle(Key::Enter);
    REQUIRE(f.session.path());
    f.state.set_notes(f.store.list());
    REQUIRE(f.session.path().has_value());
    REQUIRE(f.state.select_note(*f.session.path()));
    REQUIRE(f.session.path().has_value());
    const std::string saved = f.session.path()->string();
    f.session.handle(Key::CtrlQ);
    CHECK_FALSE(f.state.new_note_view());
    CHECK_FALSE(f.state.layout().zoomed());
    CHECK(f.state.focus() == Focus::List);
    CHECK(f.state.selected_row()->path.string() == saved);
    const auto screen = f.frame();
    CHECK(contains(screen, core::tr("tui.mode_name")));
    CHECK(contains(screen, core::tr("tui.frame_list")));
}

TEST_CASE("new note view: an explicit zoom from before does not survive it") {
    Fixture f("metin");
    f.session.handle(Key::CtrlQ);
    f.state.layout().toggle_zoom();
    f.state.open_new_note_view();
    f.session.begin_new_note("");
    f.session.handle(Key::CtrlQ);
    CHECK_FALSE(f.state.layout().zoomed());
}

TEST_CASE("the nvALT draft stays in the right pane of the two panes") {
    Fixture f("metin");
    f.session.handle(Key::CtrlQ);
    f.session.begin_draft("sorgu\n", "");
    CHECK_FALSE(f.state.new_note_view());
    const auto screen = f.frame();
    CHECK(contains(screen, "a.md"));
    f.session.handle(Key::CtrlS);
    CHECK(f.session.mode() == EditorSession::Mode::Editing);  // ad içerikten: soru yok
}

TEST_CASE("editor view: zoom gives the focused pane the whole width and the editor follows it") {
    Fixture f(std::string(60, 'x') + "SON");
    f.state.layout().toggle_zoom();
    f.session.handle(Key::CtrlEnd);
    const auto screen = f.frame();
    CHECK(screen.PixelAt(0, 1).character == "┏");
    CHECK(contains(screen, "SON"));
    CHECK_FALSE(contains(screen, "▸"));  // liste gizli
    CHECK(tui::editor_viewport({kWidth, kHeight}, f.state.layout()).columns == kWidth - 2);
}

TEST_CASE("editor view: every bottom bar stays usable at 60x15") {
    Fixture f("metin");
    f.session.handle(Key::CtrlF);
    f.session.handle(Key::Char, "m");
    auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(contains(screen, "m▏"));
    f.session.handle(Key::Escape);
    f.session.handle(Key::Char, "x");
    f.session.handle(Key::CtrlQ);
    screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(contains(screen, core::tr("tui.editor_choice_save")));
}

TEST_CASE("terminal_cell_width matches FTXUI for Turkish, combining and wide characters") {
    CHECK(tui::terminal_cell_width(U'ı') == 1);
    CHECK(tui::terminal_cell_width(U'İ') == 1);
    CHECK(tui::terminal_cell_width(U'ş') == 1);
    CHECK(tui::terminal_cell_width(0x0327) == 0);
    CHECK(tui::terminal_cell_width(U'漢') == 2);
}

TEST_CASE("termination handler records the signal instead of ending the process") {
    tui::install_termination_handlers();
    CHECK(tui::termination_signal() == 0);
    std::raise(SIGTERM);
    CHECK(tui::termination_signal() == SIGTERM);
    tui::reset_termination_for_test();
}

namespace {

// Metnin ilk hücresi; ekran ızgarası hücre başına bir grapheme tutar, bu yüzden etiket yalnız ASCII aranır.
std::optional<std::pair<int, int>> locate(const ftxui::Screen& screen, std::string_view needle) {
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x + static_cast<int>(needle.size()) <= screen.dimx(); ++x) {
            bool same = true;
            for (std::size_t i = 0; i < needle.size() && same; ++i)
                same = screen.PixelAt(x + static_cast<int>(i), y).character == std::string(1, needle[i]);
            if (same)
                return std::pair(x, y);
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("mode frame: the editor has an EDIT label, a color distinct from REVIEW and a dirty mark") {
    Fixture f("metin");
    auto screen = f.frame();
    const std::string edit = core::tr("tui.frame_edit");
    auto label = locate(screen, edit);
    REQUIRE(label);
    const ftxui::Color color = screen.PixelAt(label->first, label->second).foreground_color;
    CHECK(color == ftxui::Color(ftxui::Color::Magenta));
    CHECK(color != ftxui::Color(ftxui::Color::Green));
    CHECK_FALSE(locate(screen, edit + " *"));

    f.session.handle(Key::Char, "x");
    screen = f.frame();
    CHECK(locate(screen, edit + " *"));
    label = locate(screen, edit);
    REQUIRE(label);
    CHECK(screen.PixelAt(label->first - 1, label->second).foreground_color == color);
}

TEST_CASE("mode frame: a read-only note in the editor is labeled REVIEW, not EDIT") {
    Fixture f(std::string("a\xFF\xFE"));
    REQUIRE(f.session.read_only() != EditorSession::ReadOnly::No);
    const auto screen = f.frame();
    const auto label = locate(screen, core::tr("tui.frame_review"));
    REQUIRE(label);
    CHECK(screen.PixelAt(label->first, label->second).foreground_color == ftxui::Color(ftxui::Color::Green));
    CHECK_FALSE(locate(screen, core::tr("tui.frame_edit")));
}

namespace {

// Fixture a.md açar; vurgu testleri uzantıyı seçebilsin.
struct SyntaxFixture : Fixture {
    SyntaxFixture(const std::string& name, std::string_view content) : Fixture("x") {
        test::upsert_note(dir.path(), name, content);
        session.open(core::NotePath::parse(name).value());
        REQUIRE(session.is_open());
    }
};

ftxui::Color fg_of(const ftxui::Screen& screen, std::string_view needle) {
    const auto at = locate(screen, needle);
    REQUIRE_MESSAGE(at, needle);
    return screen.PixelAt(at->first, at->second).foreground_color;
}

}  // namespace

TEST_CASE("editor syntax: tokens are colored from the 16 ANSI set by the file extension") {
    SyntaxFixture f("a.java", "public class A { // yorum\n  String s = \"dize\"; int n = 42;\n}\n");
    CHECK(f.session.syntax_language() == tui::SyntaxLanguage::Java);
    const auto screen = f.frame();
    CHECK(fg_of(screen, "class") == ftxui::Color(ftxui::Color::BlueLight));
    CHECK(fg_of(screen, "// yorum") == ftxui::Color(ftxui::Color::Green));
    CHECK(fg_of(screen, "\"dize\"") == ftxui::Color(ftxui::Color::Magenta));
    // Çıplak "42" dakikası 42 olan saatte liste sütununda da bulunur: sayı kendi satırında aranır.
    const auto number = locate(screen, "n = 42");
    REQUIRE(number);
    constexpr int kNumberOffset = 4;
    CHECK(screen.PixelAt(number->first + kNumberOffset, number->second).foreground_color ==
          ftxui::Color(ftxui::Color::RedLight));
}

TEST_CASE("editor syntax: an unknown extension or a file over the threshold is plain") {
    SyntaxFixture unknown("a.xyz", "public class A {}\n");
    CHECK(unknown.session.syntax_language() == tui::SyntaxLanguage::Plain);
    CHECK(fg_of(unknown.frame(), "class") == ftxui::Color(ftxui::Color::Default));

    std::string big = "public class A {}\n";
    big.resize(tui::kDefaultHighlightBytes + 1, 'x');
    SyntaxFixture large("big.java", big);
    REQUIRE(large.session.read_only() == EditorSession::ReadOnly::No);
    CHECK(large.session.syntax_spans(0).empty());
}

TEST_CASE(
    "editor syntax: a note above the old 2 MiB threshold is colored when limits.tui.highlight_bytes is 0") {
    std::string big = "public class A {}\n";
    big.resize(tui::kDefaultHighlightBytes + 1, 'x');
    const test::TempDir dir;
    test::upsert_note(dir.path(), "big.java", big);
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    tui::EditorHost host{store,
                         mutex,
                         clipboard,
                         tui::terminal_cell_width,
                         kLimit,
                         core::kDefaultSlugMaxLength,
                         [](std::string_view) { return std::string("md"); },
                         [](const core::NotePath&) {},
                         [] {},
                         [](std::optional<std::size_t>) {},
                         [](const core::Error&) {},
                         [] {}};
    host.syntax =
        tui::syntax_limits_of(core::parse_config("[limits.tui]\nhighlight_bytes = 0\n").config.limits.tui);
    EditorSession session(std::move(host));
    session.open(core::NotePath::parse("big.java").value());
    REQUIRE(session.is_open());
    CHECK_FALSE(session.syntax_spans(0).empty());
}

TEST_CASE("editor undo history follows limits.tui.undo_bytes") {
    const auto edited_after_undo = [](std::size_t undo_limit_bytes) {
        const test::TempDir dir;
        test::upsert_note(dir.path(), "a.md", "x\n");
        core::NoteStore store{dir.path()};
        std::mutex mutex;
        std::string clipboard;
        tui::EditorHost host{store,
                             mutex,
                             clipboard,
                             tui::terminal_cell_width,
                             kLimit,
                             core::kDefaultSlugMaxLength,
                             [](std::string_view) { return std::string("md"); },
                             [](const core::NotePath&) {},
                             [] {},
                             [](std::optional<std::size_t>) {},
                             [](const core::Error&) {},
                             [] {}};
        host.undo_limit_bytes = undo_limit_bytes;
        EditorSession session(std::move(host));
        session.open(core::NotePath::parse("a.md").value());
        REQUIRE(session.is_open());
        for (const char* letter : {"a", "b", "c", "d"})
            session.handle(Key::Char, letter);
        for (int i = 0; i < 8; ++i)
            session.handle(Key::CtrlZ);
        return session.buffer()->text();
    };
    CHECK(edited_after_undo(tui::EditorBuffer::kDefaultUndoLimitBytes) == "x\n");
    CHECK(edited_after_undo(1) != "x\n");  // bütçeyi aşan adım tutulmaz
}

TEST_CASE("editor syntax: an edit recolors the later lines and undo restores them") {
    SyntaxFixture f("a.java", "int a;\nint b;\nint c;\n");
    CHECK(fg_of(f.frame(), "int c") == ftxui::Color(ftxui::Color::Cyan));
    f.session.handle(Key::Char, "/");
    f.session.handle(Key::Char, "*");
    CHECK(fg_of(f.frame(), "int c") == ftxui::Color(ftxui::Color::Green));
    f.session.handle(Key::CtrlZ);
    CHECK(fg_of(f.frame(), "int c") == ftxui::Color(ftxui::Color::Cyan));
}

TEST_CASE("editor syntax: search and selection win over syntax color, bytes and wrapping stay") {
    const std::string line = "public int ipsum = 1; // " + std::string(120, 'x');
    SyntaxFixture colored("a.java", line);
    SyntaxFixture plain("a.xyz", line);
    const auto with_syntax = colored.frame();
    const auto without = plain.frame();
    // Yalnız panel içi: etiket ve ad satırları dosya adını taşır.
    for (int y = 2; y < kHeight - 3; ++y)
        for (int x = tui::PaneLayout{}.list_width(kWidth) + 1; x < kWidth - 1; ++x)
            CHECK(with_syntax.PixelAt(x, y).character == without.PixelAt(x, y).character);
    CHECK(colored.session.buffer()->text() == line);

    colored.session.handle(Key::CtrlF);
    for (const char* letter : {"i", "p", "s", "u", "m"})
        colored.session.handle(Key::Char, letter);
    colored.session.handle(Key::Enter);
    const auto found = colored.frame();
    const auto at = locate(found, "ipsum");
    REQUIRE(at);
    CHECK(found.PixelAt(at->first, at->second).inverted);  // geçerli eşleşme seçimdir
}

TEST_CASE("editor syntax: control bytes stay sanitized inside a colored token") {
    SyntaxFixture f("a.java", std::string("String s = \"a\x1b[31mb\";\n"));
    const auto screen = f.frame();
    CHECK(locate(screen, "[31mb"));
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x < screen.dimx(); ++x)
            CHECK(screen.PixelAt(x, y).character.find('\x1b') == std::string::npos);
}

TEST_CASE("editor syntax: a draft follows live detect, paced on big input") {
    int detects = 0;
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string clipboard;
    EditorSession session{tui::EditorHost{
        store, mutex, clipboard, tui::terminal_cell_width, kLimit, core::kDefaultSlugMaxLength,
        [&](std::string_view content) {
            ++detects;
            return std::string(content.find("=") != std::string_view::npos ? "env" : "txt");
        },
        [](const core::NotePath&) {}, [] {}, [](std::optional<std::size_t>) {}, [](const core::Error&) {},
        [] {}}};
    session.begin_new_note("");
    CHECK(session.syntax_language() == tui::SyntaxLanguage::Plain);
    session.handle(Key::Char, "A");
    session.handle(Key::Char, "=");
    CHECK(session.syntax_language() == tui::SyntaxLanguage::Env);  // küçük girdide her tuşta

    clipboard = std::string(256 * 1024, 'x');
    session.handle(Key::CtrlV);
    const int after_paste = detects;
    constexpr int kKeys = 500;
    for (int i = 0; i < kKeys; ++i)
        session.handle(Key::Char, "y");
    CHECK(detects - after_paste < kKeys / 10);
    CHECK(detects - after_paste >= 0);
}

namespace {

constexpr int kGutterTwoDigits = 3;  // iki basamak + boşluk

int frame_x(int width) {
    return tui::PaneLayout{}.list_width(width) + 1;  // sağ panelin ilk iç sütunu
}

std::string cell_text(const ftxui::Screen& screen, int x, int y, int count) {
    std::string out;
    for (int i = 0; i < count; ++i)
        out += screen.PixelAt(x + i, y).character;
    return out;
}

}  // namespace

TEST_CASE("edit view: the gutter numbers lines, marks the current one and is not part of the text") {
    Fixture f("bir\niki\nüç");
    const auto screen = f.frame();
    const int x = frame_x(kWidth);
    CHECK(cell_text(screen, x, kPaneY, kGutterTwoDigits) == " 1 ");
    CHECK(cell_text(screen, x, kPaneY + 1, kGutterTwoDigits) == " 2 ");
    CHECK(cell_text(screen, x + kGutterTwoDigits, kPaneY, 3) == "bir");
    CHECK(screen.PixelAt(x + 1, kPaneY).bold);
    CHECK_FALSE(screen.PixelAt(x + 1, kPaneY + 1).bold);
    CHECK(screen.PixelAt(x + 1, kPaneY + 1).dim);
    CHECK(f.session.buffer()->text() == "bir\niki\nüç");
}

TEST_CASE("edit view: wrapped continuations have a blank gutter and the text keeps its column") {
    Fixture f(std::string(100, 'x') + "\nson");
    const auto screen = f.frame();
    const int x = frame_x(kWidth);
    CHECK(cell_text(screen, x, kPaneY, kGutterTwoDigits) == " 1 ");
    CHECK(cell_text(screen, x, kPaneY + 1, kGutterTwoDigits) == "   ");
    CHECK(screen.PixelAt(x + kGutterTwoDigits, kPaneY + 1).character == "x");
    CHECK(cell_text(screen, x, kPaneY + 3, kGutterTwoDigits) == " 2 ");
}

TEST_CASE("edit view: the gutter grows with the line count and the text width shrinks with it") {
    Fixture small("a");
    small.frame();
    CHECK(small.session.gutter_width() == kGutterTwoDigits);
    const int small_columns = small.session.columns();

    std::string many;
    for (int i = 0; i < 1234; ++i)
        many += "x\n";
    Fixture large(many);
    const auto screen = large.frame();
    CHECK(large.session.gutter_width() == 5);
    CHECK(large.session.columns() == small_columns - 2);
    const int x = frame_x(kWidth);
    CHECK(cell_text(screen, x, kPaneY, 5) == "   1 ");
    large.session.handle(Key::CtrlEnd);
    CHECK(locate(large.frame(), "1235"));
}

TEST_CASE("edit view: the terminal cursor sits at the real position after the gutter") {
    Fixture f("ağ\nçok");
    auto screen = f.frame();
    const int x = frame_x(kWidth) + kGutterTwoDigits;
    CHECK(screen.cursor().shape != ftxui::Screen::Cursor::Hidden);
    CHECK(screen.cursor().x == x);
    CHECK(screen.cursor().y == kPaneY);
    f.session.handle(Key::Right);
    f.session.handle(Key::Right);
    screen = f.frame();
    CHECK(screen.cursor().x == x + 2);  // satır sonu, çok baytlı `ğ` tek hücre
    CHECK(screen.cursor().y == kPaneY);
    f.session.handle(Key::Down);
    screen = f.frame();
    CHECK(screen.cursor().y == kPaneY + 1);
    CHECK(screen.cursor().x == x + 2);
}

TEST_CASE("edit view: the cursor is hidden while an input or a prompt owns the keys") {
    Fixture f("metin");
    f.session.handle(Key::CtrlF);
    CHECK(f.frame().cursor().shape == ftxui::Screen::Cursor::Hidden);
    f.session.handle(Key::Escape);
    f.session.handle(Key::Char, "x");
    f.session.handle(Key::CtrlQ);
    REQUIRE(f.session.mode() == EditorSession::Mode::LeavePrompt);
    CHECK(f.frame().cursor().shape == ftxui::Screen::Cursor::Hidden);
}

TEST_CASE("edit view: only the current line has a background") {
    Fixture f("bir\niki");
    const auto screen = f.frame();
    const int x = frame_x(kWidth) + kGutterTwoDigits;
    const ftxui::Color current = screen.PixelAt(x, kPaneY).background_color;
    CHECK(current != ftxui::Color(ftxui::Color::Default));
    CHECK(screen.PixelAt(kWidth - 2, kPaneY).background_color == current);  // satır sonuna dek
    CHECK(screen.PixelAt(x, kPaneY + 1).background_color == ftxui::Color(ftxui::Color::Default));
}

TEST_CASE("edit view: a dim tilde marks rows past the end of the file") {
    Fixture f("bir\n");
    const auto screen = f.frame();
    const int x = frame_x(kWidth);
    CHECK(cell_text(screen, x, kPaneY + 1, kGutterTwoDigits) == " 2 ");  // sondaki boş satır dosyanın parçası
    CHECK(screen.PixelAt(x, kPaneY + 2).character == "~");
    CHECK(screen.PixelAt(x, kPaneY + 2).dim);
    const auto viewport = tui::editor_viewport({kWidth, kHeight}, f.state.layout());
    CHECK(screen.PixelAt(x, kPaneY + viewport.rows - 1).character == "~");
}

TEST_CASE("edit view: the status bar names the file, type, encoding, line ending, BOM, position, total") {
    Fixture f("\xEF\xBB\xBF"
              "bir\r\niki\r\n");
    auto screen = f.frame();
    const std::string bar = row_text(screen, kHeight - 2);
    for (const std::string& part :
         {std::string("a.md"), std::string("md"), std::string("UTF-8"), std::string("CRLF"),
          std::string("BOM"), std::string("1:1"), core::tr("tui.status_lines", {{"count", "3"}})})
        CHECK_MESSAGE(bar.find(part) != std::string::npos, part);
    CHECK(bar.find(" *") == std::string::npos);
    CHECK(row_text(screen, kHeight - 1).find("Ctrl+S save") != std::string::npos);
    CHECK(row_text(screen, kHeight - 1).find("Ctrl+Q quit") != std::string::npos);
    CHECK(screen.PixelAt(frame_x(kWidth), kHeight - 2).inverted);

    f.session.handle(Key::Down);
    f.session.handle(Key::Char, "ş");
    screen = f.frame();
    const std::string dirty = row_text(screen, kHeight - 2);
    CHECK(dirty.find("a.md *") != std::string::npos);
    CHECK(dirty.find("2:2") != std::string::npos);
}

TEST_CASE("edit view: LF and no BOM are reported, a draft shows the untitled name and detected type") {
    Fixture f("bir\niki\n");
    const std::string bar = row_text(f.frame(), kHeight - 2);
    CHECK(bar.find(" LF") != std::string::npos);
    CHECK(bar.find("CRLF") == std::string::npos);
    CHECK(bar.find("BOM") == std::string::npos);

    f.session.handle(Key::CtrlQ);
    f.state.open_new_note_view();
    f.session.begin_new_note("");
    const std::string draft = row_text(f.frame(), kHeight - 2);
    CHECK(draft.find("[" + core::tr("tui.editor_untitled") + "]") != std::string::npos);
    CHECK(draft.find("md  UTF-8") != std::string::npos);  // sahte detect `md` döner
}

TEST_CASE("edit view: find and name inputs take the hints line, the status bar stays") {
    Fixture f("metin");
    f.session.handle(Key::CtrlF);
    const auto screen = f.frame();
    CHECK(row_text(screen, kHeight - 1).starts_with(core::tr("tui.find_label")));
    CHECK(row_text(screen, kHeight - 2).find("a.md") != std::string::npos);
}

TEST_CASE("edit view: gutter, cursor, status and hints work at the minimum size, zoomed and in the n draft") {
    Fixture f("bir\niki");
    auto screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    const int x = frame_x(tui::kMinWidth);
    CHECK(cell_text(screen, x, kPaneY, kGutterTwoDigits) == " 1 ");
    CHECK(screen.cursor().x == x + kGutterTwoDigits);
    CHECK(row_text(screen, tui::kMinHeight - 2).find("1:1") != std::string::npos);
    CHECK(row_text(screen, tui::kMinHeight - 2).find("a.md") != std::string::npos);
    CHECK(row_text(screen, tui::kMinHeight - 1).find("Ctrl+S") != std::string::npos);

    f.state.layout().toggle_zoom();
    screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(cell_text(screen, 1, kPaneY, kGutterTwoDigits) == " 1 ");
    CHECK(screen.cursor().x == 1 + kGutterTwoDigits);
    f.state.layout().toggle_zoom();

    f.session.handle(Key::CtrlQ);
    f.state.open_new_note_view();
    f.session.begin_new_note("");
    screen = f.frame(tui::kMinWidth, tui::kMinHeight);
    CHECK(cell_text(screen, 1, kPaneY, kGutterTwoDigits) == " 1 ");
    CHECK(screen.cursor().x == 1 + kGutterTwoDigits);
    CHECK(screen.PixelAt(1, kPaneY + 1).character == "~");
    CHECK(f.session.columns() == tui::kMinWidth - 2 - kGutterTwoDigits);
}

TEST_CASE("edit view: a long file name is cut in the status bar, the fields stay at 60 columns") {
    const std::string name = std::string(70, 'n') + ".properties";
    SyntaxFixture f(name, "a=b\n");
    const std::string bar = row_text(f.frame(tui::kMinWidth, tui::kMinHeight), tui::kMinHeight - 2);
    CHECK(bar.find("properties") != std::string::npos);
    CHECK(bar.find("1:1") != std::string::npos);
    CHECK(bar.find(core::tr("tui.status_lines", {{"count", "2"}})) != std::string::npos);
}

TEST_CASE("review stays cat-like: no gutter, no cursor, no tilde") {
    Fixture f("bir\niki");
    f.session.handle(Key::CtrlQ);
    REQUIRE(f.state.focus() == Focus::Preview);
    tui::Preview preview;
    preview.kind = tui::Preview::Kind::Text;
    preview.lines = {"bir", "iki"};
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(f.state, preview, {"sync off", ""}, {kWidth, kHeight}));
    const int x = frame_x(kWidth);
    CHECK(cell_text(screen, x, kPaneY, 3) == "bir");
    CHECK(screen.cursor().shape == ftxui::Screen::Cursor::Hidden);
    CHECK(screen.PixelAt(x, kPaneY + 2).character != "~");
    CHECK(screen.PixelAt(x, kPaneY).background_color == ftxui::Color(ftxui::Color::Default));
}
