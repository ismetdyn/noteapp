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
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

// Çalışma dizini test süresince değişir; başarısız testte de geri döner (Windows çalışma dizinini silmez).
class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& dir) : old_(std::filesystem::current_path()) {
        std::filesystem::current_path(dir);
    }
    ~ScopedCurrentPath() {
        std::error_code ignored;
        std::filesystem::current_path(old_, ignored);
    }
    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

private:
    std::filesystem::path old_;
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

// GUI'nin Ayarlar'dan çıkış sorusuyla aynı seçenekler (app::CloseChoice), düğme sırası Kaydet, At, Vazgeç.
TEST_CASE("Esc without changes cancels at once; with changes it asks Save, Discard or Cancel, defaulting to Cancel") {
    using noteapp::app::CloseChoice;
    const Home home;
    SettingsState clean(defaults());
    CHECK(clean.handle(Key::Escape) == Outcome::Cancel);

    SettingsState s(defaults());
    focus_row(s, "editor");
    type(s, "x");
    CHECK(s.handle(Key::Escape) == Outcome::Editing);
    REQUIRE(s.leave_prompt());
    CHECK(*s.leave_prompt() == CloseChoice::Cancel);
    CHECK(s.handle(Key::Enter) == Outcome::Editing);  // varsayılan "Vazgeç": forma döner
    CHECK_FALSE(s.leave_prompt());
    CHECK(row(s, "editor").value == "x");

    s.handle(Key::Escape);
    CHECK(s.handle(Key::Escape) == Outcome::Editing);  // soruda Esc de Vazgeç
    CHECK_FALSE(s.leave_prompt());

    s.handle(Key::Escape);
    s.handle(Key::Left);
    CHECK(*s.leave_prompt() == CloseChoice::Discard);
    s.handle(Key::Left);
    CHECK(*s.leave_prompt() == CloseChoice::Save);
    s.handle(Key::Left);  // döner
    CHECK(*s.leave_prompt() == CloseChoice::Cancel);
    s.handle(Key::Right);  // döner
    CHECK(*s.leave_prompt() == CloseChoice::Save);
    s.handle(Key::Right);
    CHECK(*s.leave_prompt() == CloseChoice::Discard);
    CHECK(s.handle(Key::Enter) == Outcome::Cancel);  // At: çağıran formu kapatır, hiçbir şey yazılmaz
}

// Kaydet seçimi Kaydet düğmesiyle aynı akış (Outcome::Submit -> submit): hata formda kalır, ekran kapanmaz.
TEST_CASE("Save in the leave prompt submits like the Save button; an invalid value keeps the form open") {
    using noteapp::app::CloseChoice;
    const Home home;
    write_config("editor = \"vim\"\n");
    const std::string before = noteapp::test::slurp(core::config_file());
    const core::Config base = core::load_config().config;
    SettingsState s(base);
    focus_row(s, "sync.idle_seconds");
    clear_field(s);
    type(s, "0");
    s.handle(Key::Escape);
    s.handle(Key::Left);
    s.handle(Key::Left);
    REQUIRE(*s.leave_prompt() == CloseChoice::Save);
    CHECK(s.handle(Key::Enter) == Outcome::Submit);
    CHECK_FALSE(s.leave_prompt());
    CHECK_FALSE(s.submit(base));
    CHECK_FALSE(row(s, "sync.idle_seconds").error.empty());
    CHECK(noteapp::test::slurp(core::config_file()) == before);
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
        CHECK_FALSE(s.submit(base));
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
    CHECK_FALSE(s.submit(base));
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
    const auto saved = s.submit(base);
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
    CHECK_FALSE(s.submit(base));
    CHECK_FALSE(s.error().empty());
    CHECK(s.error().find(core::config_file().filename().string()) != std::string::npos);
    CHECK(noteapp::test::slurp(core::config_file()) == broken);
}

// GUI ile tek metin: dosya adı ve sözdizimi hatasının yeri birlikte söylenir.
TEST_CASE("the broken-config refusal also tells where the syntax error is") {
    const Home home;
    write_config("editor = \"vim\"\n[sync\nenabled = \n");
    const auto base = core::load_config().config;
    SettingsState s(base);
    focus_row(s, "editor");
    type(s, "x");
    CHECK_FALSE(s.submit(base));
    CHECK(s.error().find(core::config_file().filename().string()) != std::string::npos);
    CHECK(s.error().find("at line ") != std::string::npos);
}

namespace {

// Ayarlar kutusunun içindeki düz metin (biçim kodsuz, arkadaki liste hariç): satırlar sondaki boşluksuz, tek
// boşlukla birleşir; sarma boşlukta kırdığı için sarılmış cümle yeniden bütün okunur.
std::string settings_box_text(ftxui::Screen& screen) {
    const std::string title = " " + core::tr("tui.settings_title") + " ";
    for (int top = 0; top < screen.dimy(); ++top) {
        for (int left = 0; left < screen.dimx(); ++left) {
            if (screen.PixelAt(left, top).character != "╭" || screen.PixelAt(left + 1, top).character != " " ||
                screen.PixelAt(left + 2, top).character != title.substr(1, 1))
                continue;
            int right = left + 1;
            while (right < screen.dimx() && screen.PixelAt(right, top).character != "╮")
                ++right;
            std::string joined;
            for (int y = top + 1; y < screen.dimy() && screen.PixelAt(left, y).character == "│"; ++y) {
                std::string row;
                for (int x = left + 1; x < right; ++x)
                    row += screen.PixelAt(x, y).character;
                joined += row.substr(0, row.find_last_not_of(' ') + 1) + " ";
            }
            return joined;
        }
    }
    FAIL("settings box not found");
    return {};
}

}  // namespace

// Uzun yapılandırma yolunda da "nothing was saved" ve sözdizimi hatasının satır/sütunu görünür: hata satırı kutu
// genişliğinde sarılır; alan satırlarına yer kalmazsa baştan kısalır, sondaki ileti ve konum kalır.
TEST_CASE("a long config path still shows that nothing was saved and where the syntax error is") {
    const TempDir dir;
    std::filesystem::path home = dir.path();
#ifdef _WIN32
    constexpr int kDepth = 3;  // MAX_PATH (260) altında kalsın
#else
    constexpr int kDepth = 12;  // en küçük ekranda (60x15) bile sığmaz: kısaltma yolu da sınanır
#endif
    for (int i = 0; i < kDepth; ++i)
        home /= std::string(40, static_cast<char>('a' + i));
    ScopedEnv env{"NOTEAPP_HOME", home.u8string()};
    core::init_i18n("en");
    write_config("editor = \"vim\"\n[sync\nenabled = \n");
    const auto base = core::load_config().config;
    SettingsState s(base);
    focus_row(s, "editor");
    type(s, "x");
    REQUIRE_FALSE(s.submit(base));
    const std::size_t tail_at = s.error().find("nothing was saved.");
    REQUIRE(tail_at != std::string::npos);
    const std::string tail = s.error().substr(tail_at);
    REQUIRE(tail.find("at line 2, column") != std::string::npos);
    for (const auto& [width, height] : {std::pair{80, 24}, std::pair{60, 15}}) {
        CAPTURE(width);
        ftxui::Screen screen = render_settings(s, width, height);
        const std::string shown = settings_box_text(screen);
        INFO(shown);
        CHECK(shown.find(tail) != std::string::npos);
        CHECK(shown.find(core::tr("tui.settings_hints")) != std::string::npos);  // kutu ekrandan taşmadı
        CHECK(shown.find(core::tr("tui.settings_editor")) != std::string::npos);  // odaklı alan görünür
    }
    ftxui::Screen roomy = render_settings(s, 80, 24);
    CHECK(settings_box_text(roomy).find("The config file '") != std::string::npos);  // yer varken kısalmaz
}

// Hata metni dosya yolunu ve toml++ ayrıntısını taşır: ikisi de ham denetim karakteri (CSI, RLO) içerebilir.
// Hata yolla başlar ve uzunsa baştan kısalır; yol geçici dizinin altında olsaydı denetim karakterleri uzun
// geçici dizinde (Windows: C:\users\...\Temp\) kesilen kısma düşer, "yok" denetimleri boşuna geçerdi.
// Göreli NOTEAPP_HOME onları her platformda satırın görünen başına koyar.
TEST_CASE("the settings error line never draws hidden controls raw") {
    const TempDir cwd;
    const ScopedCurrentPath in_cwd(cwd.path());
    ScopedEnv env{"NOTEAPP_HOME", std::optional<std::u8string>(u8"-\xE2\x80\xAE\xC2\x9B-")};
    core::init_i18n("en");
    write_config("editor = = bozuk\n");
    const auto base = core::load_config().config;
    SettingsState s(base);
    focus_row(s, "editor");
    type(s, "x");
    CHECK_FALSE(s.submit(base));
    REQUIRE(s.error().find("\xE2\x80\xAE") != std::string::npos);  // model ham baytı tutar
    const std::string screen = render_settings(s, 80, 24).ToString();
    // Hata satırı çizildi, iki karakter de yerinde U+FFFD olarak.
    CHECK(screen.find("'-\xEF\xBF\xBD\xEF\xBF\xBD-") != std::string::npos);
    CHECK(screen.find("\xE2\x80\xAE") == std::string::npos);
    CHECK(screen.find("\xC2\x9B") == std::string::npos);
}

// GUI ile tek kural: ilk hatada durulmaz, düzeltilecek her satır aynı denemede görünür.
TEST_CASE("every invalid row shows its error in the same save attempt") {
    const Home home;
    const auto base = defaults();
    SettingsState s(base);
    focus_row(s, "notes_dir");
    clear_field(s);
    focus_row(s, "sync.idle_seconds");
    clear_field(s);
    type(s, "0");
    CHECK_FALSE(s.submit(base));
    CHECK_FALSE(row(s, "notes_dir").error.empty());
    CHECK_FALSE(row(s, "sync.idle_seconds").error.empty());
    CHECK_FALSE(std::filesystem::exists(core::config_file()));
}

// Form açıkken `note config set` ile değişen ayar Kaydet'te ezilmez: yalnız formda değişen anahtarlar
// diskteki dosyanın üstüne yazılır (GUI ile tek kural).
TEST_CASE("saving keeps a setting changed on disk while the form was open") {
    const Home home;
    write_config("editor = \"vim\"\n");
    const auto base = core::load_config().config;
    SettingsState s(base);
    write_config("editor = \"emacs\"\n");
    focus_row(s, "sync.idle_seconds");
    clear_field(s);
    type(s, "45");
    REQUIRE(s.submit(base));
    const auto reloaded = core::load_config().config;
    CHECK(reloaded.editor == "emacs");
    CHECK(reloaded.sync.idle_seconds == 45);
}

TEST_CASE("the language change is visible to the caller through the saved config") {
    const Home home;
    const auto base = core::load_config().config;
    SettingsState s(base);
    s.handle(Key::Right);
    const auto saved = s.submit(base);
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
        CHECK(has(screen, core::tr("app.settings_save")));  // düğme satırı görünür
        CHECK(has(screen, core::tr("tui.settings_hints")));
    }
}

TEST_CASE("the focused row scrolls into view and errors render inline") {
    const Home home;
    SettingsState s(defaults());
    s.handle(Key::Up);  // düğme satırı
    auto screen = render_settings(s, 60, 15);
    CHECK(has(screen, core::tr("app.settings_save")));
    CHECK(has(screen, core::tr("tui.settings_sync_author_email")));
    CHECK_FALSE(has(screen, core::tr("tui.settings_language")));  // en üst satır kaydı

    const auto base = defaults();
    SettingsState bad(base);
    focus_row(bad, "sync.idle_seconds");
    clear_field(bad);
    type(bad, "0");
    REQUIRE_FALSE(bad.submit(base));
    CHECK(has(render_settings(bad, 60, 15), row(bad, "sync.idle_seconds").error.substr(0, 10)));
}

// Ctrl+C (çıkış) GUI'nin pencere kapanışı gibi: kirli formda aynı soru, At çıkar, Kaydet kaydedip çıkar.
TEST_CASE("Ctrl+C with unsaved changes asks; Cancel stays, Discard quits, Save submits and quits") {
    using noteapp::app::CloseChoice;
    const Home home;
    SettingsState clean(defaults());
    CHECK_FALSE(clean.ask_before_quit());  // değişiklik yok: çağıran hemen çıkar

    SettingsState s(defaults());
    focus_row(s, "editor");
    type(s, "x");
    CHECK(s.ask_before_quit());
    REQUIRE(s.leave_prompt());
    CHECK(*s.leave_prompt() == CloseChoice::Cancel);
    CHECK(s.handle(Key::Enter) == Outcome::Editing);  // Vazgeç: formda kalınır
    CHECK_FALSE(s.leave_prompt());
    CHECK(s.handle(Key::Escape) == Outcome::Editing);  // sonraki Esc sorusu çıkış değil
    s.handle(Key::Left);
    CHECK(s.handle(Key::Enter) == Outcome::Cancel);

    CHECK(s.ask_before_quit());
    s.handle(Key::Left);
    CHECK(s.handle(Key::Enter) == Outcome::Quit);
    CHECK(s.ask_before_quit());
    s.handle(Key::Left);
    s.handle(Key::Left);
    CHECK(s.handle(Key::Enter) == Outcome::SubmitAndQuit);
}

// Uygulama açıkken `note config set limits.app.map_*`: Kaydet dönen config'te dosyadaki harita sınırları gelir,
// çağıran onları çalışan haritaya uygular.
TEST_CASE("submit carries the map limits from the file so the caller applies them live") {
    const Home home;
    write_config("[limits.app]\nmap_depth = 3\nmap_nodes = 7\n");
    const core::Config base = defaults();
    REQUIRE(base.limits.app.map_depth.value == 6);
    SettingsState s(base);
    focus_row(s, "editor");
    type(s, "x");
    const auto saved = s.submit(base);
    REQUIRE(saved);
    CHECK(saved->limits.app.map_depth == core::Limit{3, "3"});
    CHECK(saved->limits.app.map_nodes == core::Limit{7, "7"});
}

TEST_CASE("the leave prompt replaces the form body with its three buttons") {
    const Home home;
    SettingsState s(defaults());
    type(s, "");
    focus_row(s, "editor");
    type(s, "x");
    s.handle(Key::Escape);
    const ftxui::Screen screen = render_settings(s, 60, 15);
    CHECK(has(screen, core::tr("tui.settings_confirm_leave")));
    CHECK(has(screen, "[ " + core::tr("app.settings_save") + " ]"));
    CHECK(has(screen, "[ " + core::tr("tui.discard_yes") + " ]"));
    CHECK(has(screen, "[ " + core::tr("tui.discard_no") + " ]"));
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
    const auto saved = s.submit(defaults());
    REQUIRE(saved.has_value());
    CHECK(saved->icons == "off");
    core::init_i18n("en");
    CHECK(tui::settings_label(s.rows()[s.focus()]) == "File icons");
    core::init_i18n("tr");
    CHECK(tui::settings_label(s.rows()[s.focus()]) == "Dosya ikonları");
    core::init_i18n("en");
}
