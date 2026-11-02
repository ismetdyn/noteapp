#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "support/test_support.hpp"
#include "tui/settings.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Key;
using tui::SettingsState;
using Outcome = SettingsState::Outcome;
using noteapp::test::ScopedEnv;
using noteapp::test::TempDir;

namespace {

// Her test kendi NOTEAPP_HOME'unda çalışır: gerçek kullanıcı config'ine dokunulmaz.
struct Home {
    TempDir dir;
    ScopedEnv env{"NOTEAPP_HOME", dir.path().u8string()};
    Home() { core::init_i18n("en"); }
};

const SettingsState::Row& row(const SettingsState& s, std::string_view key) {
    for (const auto& r : s.rows())
        if (r.key == key)
            return r;
    FAIL("no such row: " << key);
    throw 0;
}

void focus_row(SettingsState& s, std::string_view key) {
    for (std::size_t guard = 0; guard <= s.rows().size() && s.rows()[s.focus() % s.rows().size()].key != key;
         ++guard)
        s.handle(Key::Down);
    REQUIRE(s.focus() < s.rows().size());
    REQUIRE(s.rows()[s.focus()].key == key);
}

void type(SettingsState& s, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i)
        s.handle(Key::Char, text.substr(i, 1));
}

void clear_field(SettingsState& s) {
    for (int i = 0; i < 200; ++i)
        s.handle(Key::Backspace);
}

void write_config(const std::string& content) {
    const auto file = core::config_file();
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

core::Config defaults() {
    return core::parse_config("").config;
}

ftxui::Screen render_settings(const SettingsState& s, int width, int height) {
    tui::TuiState state;
    state.set_notes({});
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen,
                  tui::render(state, {}, {"sync off", ""}, {width, height}, nullptr, nullptr, nullptr, &s));
    return screen;
}

bool has(const ftxui::Screen& screen, const std::string& needle) {
    return screen.ToString().find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("rows follow the fixed human order with the right kinds") {
    const Home home;
    const SettingsState s(defaults());
    const char* order[] = {"language",
                           "notes_dir",
                           "editor",
                           "default_type",
                           "default_folder",
                           "types",
                           "icons",
                           "sync.enabled",
                           "sync.remote",
                           "sync.branch",
                           "sync.idle_seconds",
                           "sync.author_name",
                           "sync.author_email"};
    REQUIRE(s.rows().size() == 13);
    for (std::size_t i = 0; i < 13; ++i)
        CHECK(s.rows()[i].key == order[i]);
    CHECK(s.rows()[0].kind == SettingsState::Kind::Choice);
    CHECK(s.rows()[3].kind == SettingsState::Kind::Choice);
    CHECK(s.rows()[6].kind == SettingsState::Kind::Choice);
    CHECK(s.rows()[7].kind == SettingsState::Kind::Toggle);
    CHECK(s.rows()[1].kind == SettingsState::Kind::Text);
    CHECK(s.rows()[5].kind == SettingsState::Kind::Text);
    CHECK_FALSE(s.changed());
}

TEST_CASE("up down and tab move the focus through the button row and wrap") {
    const Home home;
    SettingsState s(defaults());
    CHECK(s.focus() == 0);
    s.handle(Key::Down);
    s.handle(Key::Tab);
    CHECK(s.focus() == 2);
    s.handle(Key::Up);
    s.handle(Key::ShiftTab);
    CHECK(s.focus() == 0);
    s.handle(Key::Up);  // başa sarar: düğme satırı
    CHECK(s.on_button());
    s.handle(Key::Down);
    CHECK(s.focus() == 0);
}

TEST_CASE("language is a choice cycled with arrows and ignores typing") {
    const Home home;
    auto config = defaults();
    config.language = "auto";
    SettingsState s(config);
    s.handle(Key::Right);
    CHECK(row(s, "language").value == "tr");
    s.handle(Key::Right);
    CHECK(row(s, "language").value == "en");
    s.handle(Key::Right);
    CHECK(row(s, "language").value == "auto");
    s.handle(Key::Left);
    CHECK(row(s, "language").value == "en");
    type(s, "x");
    CHECK(row(s, "language").value == "en");
    CHECK(s.changed());
}

TEST_CASE("default_type cycles through the current types and also accepts typing") {
    const Home home;
    auto config = defaults();
    config.types = {"md", "txt", "java"};
    config.default_type = "md";
    SettingsState s(config);
    focus_row(s, "default_type");
    s.handle(Key::Right);
    CHECK(row(s, "default_type").value == "txt");
    s.handle(Key::Left);
    s.handle(Key::Left);
    CHECK(row(s, "default_type").value == "java");
    type(s, "x");
    CHECK(row(s, "default_type").value == "javax");
    s.handle(Key::Backspace);
    s.handle(Key::Right);  // listede olmayan değerden ilk öğeye düşer
    CHECK(row(s, "default_type").value == "md");
}

TEST_CASE("default_type follows edits of the types row") {
    const Home home;
    SettingsState s(defaults());
    focus_row(s, "types");
    clear_field(s);
    type(s, "py, rs");
    focus_row(s, "default_type");
    s.handle(Key::Right);
    const auto value = row(s, "default_type").value;
    CHECK((value == "py" || value == "rs"));
}

TEST_CASE("sync.enabled toggles with space and arrows") {
    const Home home;
    SettingsState s(defaults());
    focus_row(s, "sync.enabled");
    CHECK(row(s, "sync.enabled").value == "false");
    type(s, " ");
    CHECK(row(s, "sync.enabled").value == "true");
    s.handle(Key::Left);
    CHECK(row(s, "sync.enabled").value == "false");
    s.handle(Key::Right);
    CHECK(row(s, "sync.enabled").value == "true");
    type(s, "x");  // diğer karakterler anahtarı oynatmaz
    CHECK(row(s, "sync.enabled").value == "true");
}

TEST_CASE("text rows edit with UTF-8 aware backspace") {
    const Home home;
    SettingsState s(defaults());
    focus_row(s, "default_folder");
    clear_field(s);
    type(s, "iş");
    s.handle(Key::Char, "ğ");
    CHECK(row(s, "default_folder").value == "işğ");
    s.handle(Key::Backspace);
    s.handle(Key::Backspace);
    CHECK(row(s, "default_folder").value == "i");
    CHECK(s.changed());
}

TEST_CASE("Esc without changes cancels at once; with changes it asks, defaulting to keep editing") {
    const Home home;
    SettingsState clean(defaults());
    CHECK(clean.handle(Key::Escape) == Outcome::Cancel);

    SettingsState s(defaults());
    focus_row(s, "editor");
    type(s, "x");
    CHECK(s.handle(Key::Escape) == Outcome::Editing);
    REQUIRE(s.discard_confirm());
    CHECK_FALSE(s.discard_confirm()->yes_selected());
    CHECK(s.handle(Key::Enter) == Outcome::Editing);  // varsayılan "Vazgeç": forma döner
    CHECK(s.discard_confirm() == nullptr);
    CHECK(row(s, "editor").value == "x");

    s.handle(Key::Escape);
    s.handle(Key::Right);  // "At"
    CHECK(s.handle(Key::Enter) == Outcome::Cancel);
}

TEST_CASE("Enter on a field only advances; Enter on the button submits") {
    const Home home;
    SettingsState s(defaults());
    CHECK(s.handle(Key::Enter) == Outcome::Editing);
    CHECK(s.focus() == 1);
    s.handle(Key::Up);
    s.handle(Key::Up);
    REQUIRE(s.on_button());
    CHECK(s.handle(Key::Enter) == Outcome::Submit);
}

TEST_CASE("invalid values show an inline error on the row and leave the config file untouched") {
    const Home home;
    write_config("editor = \"vim\"\n");
    const std::string before = noteapp::test::slurp(core::config_file());

    struct Case {
        const char* key;
        const char* value;
    };
    for (const Case& bad : {Case{"sync.idle_seconds", "0"}, Case{"types", ""}, Case{"notes_dir", ""},
                            Case{"sync.idle_seconds", "abc"}}) {
        CAPTURE(bad.key);
        CAPTURE(bad.value);
        const auto base = core::load_config().config;
        SettingsState s(base);
        focus_row(s, bad.key);
        clear_field(s);
        type(s, bad.value);
        CHECK_FALSE(s.commit(base));
        CHECK_FALSE(row(s, bad.key).error.empty());
        CHECK(noteapp::test::slurp(core::config_file()) == before);
    }
}

TEST_CASE("an emptied default_type is rejected like on the command line") {
    const Home home;
    const auto base = defaults();
    SettingsState s(base);
    focus_row(s, "default_type");
    clear_field(s);
    CHECK_FALSE(s.commit(base));
    CHECK_FALSE(row(s, "default_type").error.empty());
    CHECK_FALSE(std::filesystem::exists(core::config_file()));
}

TEST_CASE("a valid save writes config.toml and returns the new config") {
    const Home home;
    const auto base = core::load_config().config;
    SettingsState s(base);
    focus_row(s, "editor");
    type(s, "nano");
    focus_row(s, "sync.idle_seconds");
    clear_field(s);
    type(s, "45");
    focus_row(s, "types");
    clear_field(s);
    type(s, "md, txt ,py");
    focus_row(s, "default_type");
    type(s, "");
    const auto saved = s.commit(base);
    REQUIRE(saved);
    CHECK(saved->editor == "nano");
    const auto reloaded = core::load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.editor == "nano");
    CHECK(reloaded.config.sync.idle_seconds == 45);
    CHECK(reloaded.config.types == std::vector<std::string>{"md", "txt", "py"});
    // `note config get` aynı tablodan okur
    for (const auto& [key, value] : core::config_entries(reloaded.config))
        if (key == "editor")
            CHECK(value == "nano");
}

TEST_CASE("a broken config file refuses the save and stays untouched") {
    const Home home;
    const std::string broken = "editor = \"vim\"\n[sync\nenabled = \n";
    write_config(broken);
    const auto base = core::load_config().config;
    SettingsState s(base);
    focus_row(s, "editor");
    type(s, "x");
    CHECK_FALSE(s.commit(base));
    CHECK_FALSE(s.error().empty());
    CHECK(s.error().find(core::config_file().filename().string()) != std::string::npos);
    CHECK(noteapp::test::slurp(core::config_file()) == broken);
}

TEST_CASE("the language change is visible to the caller through the saved config") {
    const Home home;
    const auto base = core::load_config().config;
    SettingsState s(base);
    s.handle(Key::Right);
    const auto saved = s.commit(base);
    REQUIRE(saved);
    CHECK(saved->language == row(s, "language").value);
    core::init_i18n(saved->language);
    CHECK(core::current_language() == saved->language);
    CHECK(core::tr("tui.settings_title") != "tui.settings_title");
}

TEST_CASE("settings screen renders at the minimum size in both languages") {
    const Home home;
    for (const char* lang : {"tr", "en"}) {
        CAPTURE(lang);
        core::init_i18n(lang);
        SettingsState s(defaults());
        auto screen = render_settings(s, 60, 15);
        CHECK(has(screen, core::tr("tui.settings_title")));
        CHECK(has(screen, core::tr("tui.settings_language")));
        CHECK(has(screen, "‹"));
        CHECK(has(screen, "[ ]"));
        CHECK(has(screen, core::tr("tui.settings_save")));  // düğme satırı görünür
        CHECK(has(screen, core::tr("tui.settings_hints")));
    }
}

TEST_CASE("the focused row scrolls into view and errors render inline") {
    const Home home;
    SettingsState s(defaults());
    s.handle(Key::Up);  // düğme satırı
    auto screen = render_settings(s, 60, 15);
    CHECK(has(screen, core::tr("tui.settings_save")));
    CHECK(has(screen, core::tr("tui.settings_sync_author_email")));
    CHECK_FALSE(has(screen, core::tr("tui.settings_language")));  // en üst satır kaydı

    const auto base = defaults();
    SettingsState bad(base);
    focus_row(bad, "sync.idle_seconds");
    clear_field(bad);
    type(bad, "0");
    REQUIRE_FALSE(bad.commit(base));
    CHECK(has(render_settings(bad, 60, 15), row(bad, "sync.idle_seconds").error.substr(0, 10)));
}

TEST_CASE("the discard confirmation replaces the form body") {
    const Home home;
    SettingsState s(defaults());
    type(s, "");
    focus_row(s, "editor");
    type(s, "x");
    s.handle(Key::Escape);
    CHECK(has(render_settings(s, 60, 15), core::tr("tui.settings_confirm_discard")));
}

TEST_CASE("config warnings become one translated status line, empty when there are none") {
    const Home home;
    CHECK(tui::config_warnings_message({}).empty());

    const core::Error unknown{core::Error::Code::ConfigInvalid, "config.unknown_key", {{"key", "bogus"}}};
    const core::Error syntax{core::Error::Code::ConfigInvalid,
                             "config.syntax_error",
                             {{"line", "3"}, {"column", "5"}, {"detail", "x"}}};
    CHECK(tui::config_warnings_message({unknown}) == core::format_error(unknown));
    const std::string both = tui::config_warnings_message({unknown, syntax});
    CHECK(both == core::format_error(unknown) + "; " + core::format_error(syntax));
    CHECK(both.find("bogus") != std::string::npos);
}

TEST_CASE("the icons row cycles through nerd and off and is saved") {
    const Home home;
    SettingsState s(defaults());
    focus_row(s, "icons");
    CHECK(s.rows()[s.focus()].value == "nerd");
    s.handle(Key::Right);
    CHECK(s.rows()[s.focus()].value == "off");
    s.handle(Key::Right);
    CHECK(s.rows()[s.focus()].value == "nerd");
    s.handle(Key::Left);
    const auto saved = s.commit(defaults());
    REQUIRE(saved.has_value());
    CHECK(saved->icons == "off");
    core::init_i18n("en");
    CHECK(settings_label(s.rows()[s.focus()]) == "File icons");
    core::init_i18n("tr");
    CHECK(settings_label(s.rows()[s.focus()]) == "Dosya ikonları");
    core::init_i18n("en");
}
