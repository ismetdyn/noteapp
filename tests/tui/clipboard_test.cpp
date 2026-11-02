#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/clipboard.hpp"
#include "tui/editor_session.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace noteapp;
using Copy = tui::SystemClipboard::CopyResult;
using Paste = tui::SystemClipboard::PasteStatus;
namespace fs = std::filesystem;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 30;

struct Terminal {
    std::string sent;
    tui::SystemClipboard::TerminalWriter writer() {
        return [this](std::string_view sequence) { sent += sequence; };
    }
};

// Boş PATH'li ortam: gerçek araçlar görünmez, sahteler `tools` dizinindedir.
struct ToolEnv {
    test::TempDir tools;
    test::ScopedEnv path{"PATH", test::utf8(tools.path())};
    test::ScopedEnv wayland{"WAYLAND_DISPLAY"};
    test::ScopedEnv display{"DISPLAY"};
    Terminal terminal;

    ToolEnv() {
        core::init_i18n("en");
        wayland.unset();
        display.unset();
    }

#ifndef _WIN32
    void fake(const std::string& name, const std::string& body) const {
        const fs::path file = tools.path() / name;
        {
            std::ofstream out(file);
            // Sahteler kabuk betiğidir; araçlar PATH'te olmadığından tam yol.
            out << "#!/bin/sh\n" << body << "\n";
        }
        fs::permissions(file, fs::perms::owner_all);
    }
    std::string out(const std::string& name) const { return test::slurp(tools.path() / name); }
    std::string dir() const { return test::utf8(tools.path()); }
#endif
    tui::SystemClipboard clipboard(std::chrono::milliseconds timeout = tui::SystemClipboard::kToolTimeout) {
        return tui::SystemClipboard(terminal.writer(), timeout);
    }
};

}  // namespace

#ifndef _WIN32  // Windows'ta Win32 panosu hep vardır: "araç yok" durumu oluşmaz
TEST_CASE("clipboard: no tool falls back to OSC 52 with base64") {
    ToolEnv env;
    auto clipboard = env.clipboard();
    CHECK(clipboard.copy("Türkçe") == Copy::Terminal);
    CHECK(env.terminal.sent == "\x1B]52;c;VMO8cmvDp2U=\x07");
}
#endif

#ifndef _WIN32  // Windows'ta Win32 panosu hep vardır: "araç yok" durumu oluşmaz
TEST_CASE("clipboard: OSC 52 refuses text over the stated limit and writes nothing") {
    ToolEnv env;
    auto clipboard = env.clipboard();
    CHECK(clipboard.copy(std::string(tui::SystemClipboard::kOsc52MaxBytes, 'a')) == Copy::Terminal);
    env.terminal.sent.clear();
    CHECK(clipboard.copy(std::string(tui::SystemClipboard::kOsc52MaxBytes + 1, 'a')) == Copy::TooLarge);
    CHECK(env.terminal.sent.empty());
}
#endif

TEST_CASE("clipboard: text with NUL is refused before any backend runs") {
    ToolEnv env;
    auto clipboard = env.clipboard();
    CHECK(clipboard.copy(std::string("a\0b", 3)) == Copy::Binary);
    CHECK(env.terminal.sent.empty());
}

#ifndef _WIN32  // Windows'ta Win32 panosu hep vardır: "araç yok" durumu oluşmaz
TEST_CASE("clipboard: paste with no tool says so") {
    ToolEnv env;
    auto clipboard = env.clipboard();
    CHECK(clipboard.paste().status == Paste::NoTool);
    const tui::PasteChoice nothing = tui::choose_paste(clipboard, "");
    CHECK(nothing.text.empty());
    CHECK(nothing.message.find("Ctrl+Shift+V") != std::string::npos);
    const tui::PasteChoice internal = tui::choose_paste(clipboard, "ic pano");
    CHECK(internal.text == "ic pano");
    CHECK(internal.message.find("Ctrl+Shift+V") != std::string::npos);
}
#endif

#ifndef _WIN32
TEST_CASE("clipboard: wl-copy receives the bytes on stdin, without a shell and without AppImage variables") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    const test::ScopedEnv cert_file("SSL_CERT_FILE", std::string("/paket/ca.pem"));
    const test::ScopedEnv cert_dir("SSL_CERT_DIR", std::string("/paket/yok"));
    const test::ScopedEnv library("LD_LIBRARY_PATH", std::string("/paket/lib"));
    const test::ScopedEnv token("NOTEAPP_GIT_TOKEN", std::string("gizli"));
    env.fake("wl-copy", "/bin/cat > \"" + env.dir() + "/copied\"\n/usr/bin/env > \"" + env.dir() + "/env\"");
    auto clipboard = env.clipboard();
    const std::string text = "bir $(whoami) `id` \"tırnak\"\nikinci";
    CHECK(clipboard.copy(text) == Copy::Tool);
    CHECK(env.out("copied") == text);
    const std::string seen = env.out("env");
    CHECK(seen.find("SSL_CERT") == std::string::npos);
    CHECK(seen.find("LD_LIBRARY_PATH") == std::string::npos);
    CHECK(seen.find("NOTEAPP_GIT_TOKEN") == std::string::npos);
    CHECK(env.terminal.sent.empty());
}

TEST_CASE("clipboard: a forking tool that keeps running does not stall the copy") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    env.fake("wl-copy", "/bin/cat > /dev/null\n/bin/sleep 5 &\nexit 0");
    auto clipboard = env.clipboard();
    const auto start = std::chrono::steady_clock::now();
    CHECK(clipboard.copy("x") == Copy::Tool);
    if (noteapp::test::kWallClockBudgets)
        CHECK(std::chrono::steady_clock::now() - start < tui::SystemClipboard::kToolTimeout);
}

TEST_CASE("clipboard: a large copy goes through and stays byte exact") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    env.fake("wl-copy", "/bin/cat > \"" + env.dir() + "/copied\"");
    auto clipboard = env.clipboard(std::chrono::milliseconds(20'000));
    std::string big;
    for (int i = 0; big.size() < std::size_t{4} * 1024 * 1024; ++i)
        big += "satır " + std::to_string(i) + "\n";
    CHECK(clipboard.copy(big) == Copy::Tool);
    CHECK(env.out("copied") == big);
}

TEST_CASE("clipboard: a hung tool is killed at the timeout and OSC 52 takes over") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    env.fake("wl-copy", "exec /bin/sleep 30");
    auto clipboard = env.clipboard(std::chrono::milliseconds(200));
    const auto start = std::chrono::steady_clock::now();
    CHECK(clipboard.copy("x") == Copy::Terminal);
    if (noteapp::test::kWallClockBudgets)
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK(env.terminal.sent == "\x1B]52;c;eA==\x07");
    CHECK(clipboard.stale());
}

TEST_CASE("clipboard: a tool that exits without reading does not kill the process") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    env.fake("wl-copy", "exit 3");
    auto clipboard = env.clipboard();
    CHECK(clipboard.copy(std::string(std::size_t{1024} * 1024, 'x')) == Copy::TooLarge);
    CHECK(clipboard.stale());
}

TEST_CASE("clipboard: after a failed tool copy, paste prefers the internal clipboard") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    env.fake("wl-copy", "exit 1");
    env.fake("wl-paste", "/usr/bin/printf eski");
    auto clipboard = env.clipboard();
    CHECK(clipboard.copy("yeni") == Copy::Terminal);
    CHECK(tui::choose_paste(clipboard, "yeni").text == "yeni");
}

TEST_CASE("clipboard: paste reads the tool's stdout unchanged") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    env.fake("wl-paste", "[ \"$1\" = --no-newline ] || exit 9\n/usr/bin/printf 'İlk\\r\\nikinci'");
    auto clipboard = env.clipboard();
    const auto pasted = clipboard.paste();
    CHECK(pasted.status == Paste::Text);
    CHECK(pasted.text == "İlk\r\nikinci");
    CHECK(tui::choose_paste(clipboard, "ic").text == "İlk\r\nikinci");
}

TEST_CASE("clipboard: binary, invalid UTF-8 and oversized paste are refused with a message") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    auto clipboard = env.clipboard();
    env.fake("wl-paste", "/usr/bin/printf 'a\\000b'");
    CHECK(clipboard.paste().status == Paste::Binary);
    CHECK(tui::choose_paste(clipboard, "ic").text.empty());
    CHECK_FALSE(tui::choose_paste(clipboard, "ic").message.empty());
    env.fake("wl-paste", "/usr/bin/printf '\\377\\376'");
    CHECK(clipboard.paste().status == Paste::Binary);
    env.fake("wl-paste", "/usr/bin/head -c 40000000 /dev/zero");
    CHECK(clipboard.paste().status == Paste::TooLarge);
}

TEST_CASE("clipboard: a failing or hung paste tool falls back to the internal clipboard") {
    ToolEnv env;
    env.wayland.set("wayland-1");
    env.fake("wl-paste", "exit 1");
    auto clipboard = env.clipboard(std::chrono::milliseconds(200));
    CHECK(clipboard.paste().status == Paste::Unavailable);
    CHECK(tui::choose_paste(clipboard, "ic").text == "ic");
    CHECK_FALSE(tui::choose_paste(clipboard, "").message.empty());
    env.fake("wl-paste", "exec /bin/sleep 30");
    CHECK(clipboard.paste().status == Paste::Unavailable);
    CHECK(tui::choose_paste(clipboard, "ic").text == "ic");
}

TEST_CASE("clipboard: X11 uses xclip, then xsel, with the clipboard selection") {
    ToolEnv env;
    env.display.set(":0");
    env.fake("xsel", "/bin/cat > \"" + env.dir() + "/xsel.out\"\n/usr/bin/printf '%s ' \"$@\" > \"" +
                         env.dir() + "/xsel.args\"");
    auto clipboard = env.clipboard();
    CHECK(clipboard.copy("x") == Copy::Tool);
    CHECK(env.out("xsel.args") == "--clipboard --input ");
    env.fake("xclip", "/bin/cat > \"" + env.dir() + "/xclip.out\"\n/usr/bin/printf '%s ' \"$@\" > \"" +
                          env.dir() + "/xclip.args\"");
    CHECK(clipboard.copy("y") == Copy::Tool);
    CHECK(env.out("xclip.args") == "-selection clipboard -in ");
    CHECK(env.out("xclip.out") == "y");
}

TEST_CASE("clipboard: Wayland tools are ignored when only DISPLAY is set") {
    ToolEnv env;
    env.display.set(":0");
    env.fake("wl-copy", "/bin/cat > \"" + env.dir() + "/wl\"");
    auto clipboard = env.clipboard();
    CHECK(clipboard.copy("x") == Copy::Terminal);
    CHECK_FALSE(fs::exists(env.tools.path() / "wl"));
}
#endif

namespace {

int plain_width(char32_t) {
    return 1;
}

struct SessionFixture {
    ToolEnv env;
    test::TempDir dir;
    core::NoteStore store{dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system = env.clipboard(std::chrono::milliseconds(500));
    tui::EditorSession session{tui::EditorHost{
        store,
        mutex,
        internal,
        plain_width,
        1024 * 1024,
        core::kDefaultSlugMaxLength,
        [](std::string_view) { return std::string("txt"); },
        [](const core::NotePath&) {},
        [] {},
        [](std::optional<std::size_t>) {},
        [](const core::Error&) {},
        [] {},
        [this](std::string_view text) { return tui::copy_message(system.copy(text), text.size()); },
        [this](const std::string& from) { return tui::choose_paste(system, from); },
    }};

    SessionFixture() {
        session.layout(40, 10);
        session.begin_new_note("");
    }
    void type(std::string_view text) {
        for (const char c : text)
            session.handle(tui::Key::Char, std::string(1, c));
    }
};

}  // namespace

#ifndef _WIN32  // Windows'ta Win32 panosu hep vardır: "araç yok" durumu oluşmaz
TEST_CASE("editor: Ctrl+C copies the line, Ctrl+X too, the message says where it went") {
    SessionFixture f;
    f.type("satir");
    f.session.handle(tui::Key::CtrlC);
    CHECK(f.internal == "satir");
    CHECK(f.env.terminal.sent == "\x1B]52;c;c2F0aXI=\x07");
    CHECK(f.session.message().find("OSC 52") != std::string::npos);
    f.session.handle(tui::Key::CtrlX);
    CHECK(f.session.buffer()->text().empty());
    CHECK(f.internal == "satir");
}
#endif

TEST_CASE("editor: Ctrl+C copies only the selection") {
    SessionFixture f;
    f.type("abcd");
    f.session.handle(tui::Key::ShiftLeft);
    f.session.handle(tui::Key::ShiftLeft);
    f.session.handle(tui::Key::CtrlC);
    CHECK(f.internal == "cd");
}

#ifndef _WIN32
TEST_CASE("editor: Ctrl+V pastes the system clipboard as one undo step") {
    SessionFixture f;
    f.env.wayland.set("wayland-1");
    f.env.fake("wl-paste", "/usr/bin/printf 'bir\\niki\\nüç'");
    f.type("x");
    f.session.handle(tui::Key::CtrlV);
    CHECK(f.session.buffer()->text() == "xbir\niki\nüç");
    f.session.handle(tui::Key::CtrlZ);
    CHECK(f.session.buffer()->text() == "x");
}

TEST_CASE("editor: a refused paste changes nothing and keeps the internal clipboard") {
    SessionFixture f;
    f.env.wayland.set("wayland-1");
    f.env.fake("wl-paste", "/usr/bin/printf 'a\\000b'");
    f.internal = "ic";
    f.type("x");
    f.session.handle(tui::Key::CtrlV);
    CHECK(f.session.buffer()->text() == "x");
    CHECK(f.internal == "ic");
    CHECK_FALSE(f.session.message().empty());
}
#endif

#ifndef _WIN32  // Windows'ta Win32 panosu hep vardır: "araç yok" durumu oluşmaz
TEST_CASE("editor: Ctrl+V with no tool and an empty internal clipboard tells the user about Ctrl+Shift+V") {
    SessionFixture f;
    f.type("x");
    f.session.handle(tui::Key::CtrlV);
    CHECK(f.session.buffer()->text() == "x");
    CHECK(f.session.message().find("Ctrl+Shift+V") != std::string::npos);
}
#endif

namespace {

// Liste + REVIEW: gerçek durum modeli, yerleşim 100x30 (sol 40).
struct ListFixture {
    ToolEnv env;
    test::TempDir dir;
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system = env.clipboard(std::chrono::milliseconds(500));
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    tui::EditorSession editor{tui::EditorHost{
        *store,
        mutex,
        internal,
        plain_width,
        1024 * 1024,
        core::kDefaultSlugMaxLength,
        [](std::string_view) { return std::string("txt"); },
        [](const core::NotePath&) {},
        [] {},
        [](std::optional<std::size_t>) {},
        [](const core::Error&) {},
        [] {},
    }};
    tui::ListActions actions{
        tui::ListHost{store, mutex, internal, system, state, preview, status, [] {}, [] {}}};
    std::optional<tui::ConfirmState> confirm;

    int right_x() const { return state.layout().geometry(kWidth, true).right_x + 1; }
    static constexpr int kTopRow = 2;

    void put(const std::string& name, std::string_view bytes) const {
        test::upsert_note(dir.path(), name, bytes);
    }
    void open(const std::string& name, std::string_view bytes) {
        put(name, bytes);
        state.set_window(kWidth, kHeight);
        state.set_notes(store->list());
        sync();
    }
    void sync() {
        const std::vector<std::string>& lines = preview.get(*store, state.selected_row()).lines;
        state.set_preview_lines(&lines, preview.generation());
    }
    void mouse(int x, int y, tui::MouseButton button, tui::MouseMotion motion) {
        actions.on_mouse({x, y, button, motion}, editor);
        sync();
    }
    void drag(int x0, int y0, int x1, int y1) {
        mouse(x0, y0, tui::MouseButton::Left, tui::MouseMotion::Press);
        mouse(x1, y1, tui::MouseButton::Left, tui::MouseMotion::Move);
        mouse(x1, y1, tui::MouseButton::Left, tui::MouseMotion::Release);
    }
};

}  // namespace

#ifndef _WIN32  // Windows'ta Win32 panosu hep vardır: "araç yok" durumu oluşmaz
TEST_CASE("c copies the full original bytes from the list and from REVIEW") {
    ListFixture f;
    const std::string body = "bir\r\niki\n";
    f.open("a.md", body);
    CHECK(f.state.handle(tui::Key::Char, "c") == tui::Intent::CopyNote);
    f.actions.run(tui::Intent::CopyNote, f.confirm);
    CHECK(f.internal == body);
    CHECK(f.status.message.find("OSC 52") != std::string::npos);
    f.state.handle(tui::Key::Right);
    CHECK(f.state.handle(tui::Key::Char, "c") == tui::Intent::CopyNote);
}
#endif

TEST_CASE("c on a binary or non-UTF-8 note states why nothing is copied") {
    ListFixture f;
    f.open("a.bin", std::string("a\0b", 3));
    f.actions.run(tui::Intent::CopyNote, f.confirm);
    CHECK(f.status.message.find("binary") != std::string::npos);
    CHECK(f.internal.empty());
    CHECK(f.env.terminal.sent.empty());
    ListFixture g;
    g.open("b.txt", "a\xFF");
    g.actions.run(tui::Intent::CopyNote, g.confirm);
    CHECK(g.status.message.find("UTF-8") != std::string::npos);
    CHECK(g.internal.empty());
}

TEST_CASE("REVIEW drag selects within a line and copies on release") {
    ListFixture f;
    f.open("a.txt", "merhaba dunya\nikinci\n");
    const int x = f.right_x();
    f.drag(x + 8, f.kTopRow, x + 12, f.kTopRow);
    CHECK(f.internal == "dunya");
    CHECK(f.state.focus() == tui::Focus::Preview);
    const auto on_line = f.state.preview_selection_on_line(0, 13);
    REQUIRE(on_line);
    CHECK(on_line->begin == 8);
    CHECK(on_line->end == 13);
}

TEST_CASE("REVIEW drag copies the original text across wrapped lines and CRLF, no gutter or wrap") {
    ListFixture f;
    const std::string first(100, 'a');  // sağ panel 58 sütun: iki görsel satır
    f.open("a.txt", first + "\r\nikinci\r\nson");
    const int x = f.right_x();
    f.drag(x, f.kTopRow, x + 1, f.kTopRow + 2);
    CHECK(f.internal == first + "\r\nik");  // son satırın ilk iki karakteri
}

TEST_CASE("REVIEW backward drag selects the same text") {
    ListFixture f;
    f.open("a.txt", "abcdef\n");
    const int x = f.right_x();
    f.drag(x + 4, f.kTopRow, x + 1, f.kTopRow);
    CHECK(f.internal == "bcde");
}

TEST_CASE("a click without a drag copies nothing and selects nothing") {
    ListFixture f;
    f.open("a.txt", "abcdef\n");
    const int x = f.right_x();
    f.mouse(x + 1, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Press);
    f.mouse(x + 1, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Release);
    CHECK(f.internal.empty());
    CHECK_FALSE(f.state.preview_selection_range());
    CHECK(f.state.focus() == tui::Focus::Preview);
}

TEST_CASE("a drag past the pane edges clamps to the pane") {
    ListFixture f;
    f.open("a.txt", "abcdef\nikinci\n");
    const int x = f.right_x();
    f.mouse(x + 2, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Press);
    f.mouse(1, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Move);  // sol panelin üstü
    f.mouse(1, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Release);
    CHECK(f.internal == "abc");
    ListFixture g;
    g.open("a.txt", "abcdef\nikinci\n");
    g.mouse(x + 2, g.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Press);
    g.mouse(kWidth + 40, kHeight + 40, tui::MouseButton::Left, tui::MouseMotion::Move);
    g.mouse(kWidth + 40, kHeight + 40, tui::MouseButton::Left, tui::MouseMotion::Release);
    CHECK(g.internal == "cdef\nikinci\n");
}

TEST_CASE("wheel during a selection scrolls and keeps the anchor") {
    ListFixture f;
    std::string body;
    for (int i = 0; i < 100; ++i)
        body += "s" + std::to_string(i) + "\n";
    f.open("a.txt", body);
    const int x = f.right_x();
    f.mouse(x, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Press);
    f.mouse(x, f.kTopRow + 1, tui::MouseButton::Left, tui::MouseMotion::Move);
    f.mouse(x, f.kTopRow + 1, tui::MouseButton::WheelDown, tui::MouseMotion::Press);
    CHECK(f.state.preview_scroll() == 3);
    f.mouse(x, f.kTopRow + 1, tui::MouseButton::Left, tui::MouseMotion::Release);
    CHECK(f.internal == "s0\ns1\ns2\ns3\ns");  // baş satır 0, uç kayan pencerede satır 4'ün ilk hücresi
}

TEST_CASE("a lost release ends the selection on the next plain move") {
    ListFixture f;
    f.open("a.txt", "abcdef\n");
    const int x = f.right_x();
    f.mouse(x, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Press);
    f.mouse(x + 2, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Move);
    f.mouse(x + 5, f.kTopRow, tui::MouseButton::Other, tui::MouseMotion::Move);
    CHECK(f.internal == "abc");
    CHECK_FALSE(f.state.preview_selection_dragging());
}

TEST_CASE("divider drag and list clicks still work beside the selection") {
    ListFixture f;
    f.open("a.txt", "abc\n");
    // Aynı saat tıkında yazılırsa eşitlik ada göre çözülür, sıra değişirdi.
    test::upsert_note(f.dir.path(), "b.txt", "def\n",
                      std::chrono::system_clock::now() + std::chrono::seconds(5));
    f.state.set_notes(f.store->list());
    f.sync();
    REQUIRE(f.state.selected() == 1);  // en yenisi başta, seçim a.txt'de kaldı
    f.mouse(1, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Press);
    f.mouse(1, f.kTopRow, tui::MouseButton::Left, tui::MouseMotion::Release);
    CHECK(f.state.selected() == 0);
    CHECK(f.state.focus() == tui::Focus::List);
    const int divider = f.state.layout().geometry(kWidth, false).list_width - 1;
    f.mouse(divider, 5, tui::MouseButton::Left, tui::MouseMotion::Press);
    f.mouse(divider + 6, 5, tui::MouseButton::Left, tui::MouseMotion::Move);
    f.mouse(divider + 6, 5, tui::MouseButton::Left, tui::MouseMotion::Release);
    CHECK(f.state.layout().geometry(kWidth, false).list_width == divider + 7);
    CHECK(f.internal.empty());
}

TEST_CASE("selecting in a non-UTF-8 note says why nothing is copied") {
    ListFixture f;
    f.open("a.txt", "abc\xFF"
                    "def\n");
    const int x = f.right_x();
    f.drag(x, f.kTopRow, x + 3, f.kTopRow);
    CHECK(f.internal.empty());
    CHECK(f.status.message.find("UTF-8") != std::string::npos);
}

TEST_CASE("selected_text never reads past a text that shrank") {
    tui::Preview preview;
    preview.text = "ab";
    preview.lines = {"ab"};
    preview.line_starts = {0};
    CHECK_NOTHROW(CHECK(tui::selected_text(preview, {0, 50, 0, 60}).empty()));
    CHECK(tui::selected_text(preview, {0, 1, 0, 60}) == "b");
    CHECK(tui::selected_text(preview, {0, 0, 0, 2}) == "ab");
}

TEST_CASE("a note that shrinks during a REVIEW drag copies nothing and does not throw") {
    ListFixture f;
    f.open("a.md", std::string(70, 'x'));
    f.state.handle(tui::Key::Right);
    f.internal = "onceki";
    f.mouse(f.right_x() + 20, ListFixture::kTopRow, tui::MouseButton::Left, tui::MouseMotion::Press);
    f.mouse(f.right_x() + 30, ListFixture::kTopRow, tui::MouseButton::Left, tui::MouseMotion::Move);
    REQUIRE(f.state.preview_selection_dragging());
    f.put("a.md", "k");
    f.preview.invalidate();  // yeniden taramanın yaptığı
    const tui::MouseInput release{f.right_x() + 30, ListFixture::kTopRow, tui::MouseButton::Left,
                                  tui::MouseMotion::Release};
    CHECK_NOTHROW(f.actions.on_mouse(release, f.editor));
    CHECK(f.internal == "onceki");
}
