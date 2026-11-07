#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "support/test_support.hpp"
#include "sync/git_runtime.hpp"
#include "sync/sync_service.hpp"
#include "tui/settings.hpp"
#include "tui/sync_indicator.hpp"
#include "tui/view.hpp"

#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <git2.h>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace noteapp;
using tui::Key;
using tui::SettingsServices;
using tui::SettingsState;
using Outcome = SettingsState::Outcome;
using noteapp::test::ScopedEnv;
using noteapp::test::TempDir;

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

constexpr std::string_view kSecret = "ghp_SECRET_token_value";

// Gerçek kasa yerine bellek içi kasa; çağrı sırası da kaydedilir.
struct FakeVault {
    std::map<std::string, std::string> tokens;
    std::vector<std::string> calls;
    bool fail_store = false;

    tui::TokenStore store() {
        return {[this](std::string_view host) { return tokens.contains(std::string(host)); },
                [this](std::string_view host, std::string_view token) -> std::expected<void, core::Error> {
                    calls.push_back("store " + std::string(host));
                    if (fail_store)
                        return std::unexpected(
                            core::Error{core::Error::Code::Io, "sync.keychain_unavailable", {}});
                    tokens[std::string(host)] = std::string(token);
                    return {};
                },
                [this](std::string_view host) -> std::expected<void, core::Error> {
                    calls.push_back("clear " + std::string(host));
                    tokens.erase(std::string(host));
                    return {};
                }};
    }
};

// Her test kendi NOTEAPP_HOME/HOME'unda: gerçek config'e ve ~/.gitconfig'e dokunulmaz.
struct World {
    TempDir dir;
    ScopedEnv app{"NOTEAPP_HOME", (dir.path() / "app").u8string()};
    ScopedEnv home{"HOME", (dir.path() / "home").u8string()};
    ScopedEnv profile{"USERPROFILE", (dir.path() / "home").u8string()};
    sync::GitRuntime runtime;  // libgit2 ortam değişkenlerinden sonra başlar
    FakeVault vault;
    fs::path notes = dir.path() / "notes";

    World() {
        core::init_i18n("en");
        fs::create_directories(notes);
        std::ofstream(notes / "a.md", std::ios::binary) << "# a\n";
    }

    // Boş bare uzak depo; `main` dalı.
    fs::path make_remote(std::string_view name) const {
        const fs::path path = dir.path() / name;
        git_repository_init_options options;
        REQUIRE(git_repository_init_options_init(&options, GIT_REPOSITORY_INIT_OPTIONS_VERSION) >= 0);
        options.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_BARE;
        options.initial_head = "main";
        git_repository* repo = nullptr;
        REQUIRE(git_repository_init_ext(&repo, utf8(path).c_str(), &options) >= 0);
        git_repository_free(repo);
        return path;
    }

    core::Config config() const {
        core::Config c = core::parse_config("").config;
        c.notes_dir = notes;
        c.sync.branch = "main";
        c.sync.author_name = "Test";
        c.sync.author_email = "test@noteapp.invalid";
        return c;
    }

    SettingsState form(const core::Config& c, tui::SyncEnabler enabler = tui::system_sync_enabler()) {
        return SettingsState(c, SettingsServices{vault.store(), std::move(enabler)});
    }
};

void focus_key(SettingsState& s, std::string_view key) {
    for (std::size_t guard = 0; guard <= s.slot_count(); ++guard) {
        if (s.focus() < s.rows().size() && s.rows()[s.focus()].key == key)
            return;
        s.handle(Key::Down);
    }
    FAIL("no such row: " << key);
}

void focus_token(SettingsState& s) {
    for (std::size_t guard = 0; guard <= s.slot_count() && !s.on_token(); ++guard)
        s.handle(Key::Down);
    REQUIRE(s.on_token());
}

void type(SettingsState& s, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i)
        s.handle(Key::Char, text.substr(i, 1));
}

void set_remote(SettingsState& s, std::string_view url) {
    focus_key(s, "sync.remote");
    for (int i = 0; i < 200; ++i)
        s.handle(Key::Backspace);
    type(s, url);
}

void enable_toggle(SettingsState& s) {
    focus_key(s, "sync.enabled");
    s.handle(Key::Char, " ");
}

std::string config_text() {
    std::ifstream in(core::config_file(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string screen_text(const SettingsState& s, int width = 60, int height = 15) {
    tui::TuiState state;
    state.set_notes({});
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen,
                  tui::render(state, {}, {"sync off", ""}, {width, height}, nullptr, nullptr, nullptr, &s));
    return screen.ToString();
}

bool has(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("the token row exists only for an https remote and lives outside the 13 config rows") {
    World w;
    for (const char* remote :
         {"", "git@github.com:me/notes.git", "C:/local/repo.git", "http://example.com/r.git"}) {
        const auto s = w.form([&] {
            auto c = w.config();
            c.sync.remote = remote;
            return c;
        }());
        CHECK_FALSE(s.token_visible());
        CHECK(s.rows().size() == 13);
        CHECK(s.slot_count() == 15);  // yalnız bağlantı testi ve Kaydet düğmeleri eklenir
    }
    auto c = w.config();
    c.sync.remote = "https://GitHub.com/me/notes.git";
    const auto s = w.form(c);
    CHECK(s.token_visible());
    CHECK(s.rows().size() == 13);
    CHECK(s.slot_count() == 16);
    CHECK(s.token_row().kind == SettingsState::Kind::Token);
}

TEST_CASE("the token row appears and disappears while the remote is being edited") {
    World w;
    auto s = w.form(w.config());
    CHECK_FALSE(s.token_visible());
    set_remote(s, "https://github.com/me/notes.git");
    CHECK(s.token_visible());
    set_remote(s, "git@github.com:me/notes.git");
    CHECK_FALSE(s.token_visible());
}

TEST_CASE("focus runs rows, then the token row, the connection test, then the save button") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    auto s = w.form(c);
    focus_token(s);
    CHECK(s.focus() == 13);
    CHECK_FALSE(s.on_button());
    s.handle(Key::Down);
    CHECK(s.on_test());
    s.handle(Key::Down);
    CHECK(s.on_button());
    s.handle(Key::Up);
    s.handle(Key::Up);
    CHECK(s.on_token());
}

TEST_CASE("typing a token masks it everywhere: row value, render, config file") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    auto s = w.form(c);
    focus_token(s);
    type(s, kSecret);
    CHECK(s.token_length() == kSecret.size());
    const auto row = s.token_row();
    CHECK(row.value.find("ghp") == std::string::npos);
    CHECK(row.value.find("•") != std::string::npos);
    const std::string screen = screen_text(s);
    CHECK_FALSE(has(screen, "SECRET"));
    CHECK_FALSE(has(screen, "ghp_"));
    CHECK(has(screen, "•"));
    CHECK(has(screen, "Access token"));
    CHECK(s.changed());  // yazılmış token Esc'te sessizce atılmaz
    w.vault.tokens.clear();
    REQUIRE(s.submit(c));
    CHECK_FALSE(has(config_text(), "SECRET"));
}

TEST_CASE("the keychain indicator follows has_token for the remote host") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://GitHub.com/me/notes.git";
    auto s = w.form(c);
    CHECK(s.token_row().note == "none");
    focus_token(s);  // 60x15'te satır görünsün diye odak oraya
    CHECK(has(screen_text(s), "none"));
    focus_token(s);
    type(s, kSecret);
    s.handle(Key::Enter);  // token'ı kasaya yaz
    CHECK(s.token_row().note == "in keychain ✓");
    CHECK(has(screen_text(s), "in keychain ✓"));
}

TEST_CASE("Enter on the token row stores it under the lower-case host and wipes the input") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://GitHub.com/me/notes.git";
    auto s = w.form(c);
    focus_token(s);
    type(s, std::string(kSecret));
    s.handle(Key::Enter);
    CHECK(w.vault.tokens.at("github.com") == kSecret);
    CHECK(s.token_length() == 0);
    CHECK(s.on_token());  // odak yerinde kalır
}

TEST_CASE("leaving with a typed token asks first; Cancel and Discard never reach the keychain") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    auto s = w.form(c);
    focus_token(s);
    type(s, kSecret);
    CHECK(s.handle(Key::Escape) == Outcome::Editing);  // yalnız token yazılı: yine sorulur
    REQUIRE(s.leave_prompt());
    CHECK(s.handle(Key::Enter) == Outcome::Editing);  // Vazgeç
    CHECK(s.token_length() == kSecret.size());
    s.handle(Key::Escape);
    s.handle(Key::Left);  // At
    CHECK(s.handle(Key::Enter) == Outcome::Cancel);
    CHECK(w.vault.calls.empty());
    CHECK_FALSE(has(config_text(), "SECRET"));
}

TEST_CASE("Ctrl-D on the token row deletes the stored token") {
    World w;
    w.vault.tokens["github.com"] = std::string(kSecret);
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    auto s = w.form(c);
    CHECK(s.token_row().note == "in keychain ✓");
    focus_token(s);
    s.handle(Key::CtrlD);
    CHECK(w.vault.tokens.empty());
    CHECK(s.token_row().note == "none");
}

TEST_CASE("a keychain failure is shown on the form and the typed token is not echoed") {
    World w;
    w.vault.fail_store = true;
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    auto s = w.form(c);
    focus_token(s);
    type(s, kSecret);
    s.handle(Key::Enter);
    CHECK_FALSE(s.error().empty());
    CHECK_FALSE(has(s.error(), "SECRET"));
    CHECK(w.vault.tokens.empty());
}

TEST_CASE("Enter on an empty token row just moves on") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    auto s = w.form(c);
    focus_token(s);
    s.handle(Key::Enter);
    CHECK(s.on_test());
    CHECK(w.vault.calls.empty());
}

TEST_CASE("submit enables sync when any sync field or notes_dir changes, and stores a typed token first") {
    World w;
    std::vector<std::string> log;
    const auto enabler = [&](const core::Config& next, bool replace) -> std::expected<void, core::Error> {
        log.push_back("enable " + next.sync.remote + (replace ? " replace" : ""));
        return {};
    };
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    {
        auto s = w.form(c, enabler);
        enable_toggle(s);
        focus_token(s);
        type(s, kSecret);
        const auto saved = s.submit(c);
        REQUIRE(saved);
        CHECK(saved->sync.enabled);
        REQUIRE(log.size() == 1);
        CHECK(log[0] == "enable https://github.com/me/notes.git");
        REQUIRE(w.vault.calls.size() == 1);  // token enable'dan ÖNCE kasaya gitti
        CHECK(w.vault.tokens.at("github.com") == kSecret);
        CHECK(has(config_text(), "enabled = true"));
        CHECK_FALSE(has(config_text(), "SECRET"));
    }
    log.clear();
    c.sync.enabled = true;
    {
        auto s = w.form(c, enabler);  // zaten açık, senkron alanı değişmedi: enable yok
        focus_key(s, "editor");
        type(s, "x");
        REQUIRE(s.submit(c));
        CHECK(log.empty());
    }
    {
        auto s = w.form(c, enabler);  // açıkken dal değişti: depo yeni ayarla kurulur (GUI ile tek kural)
        focus_key(s, "sync.branch");
        type(s, "x");
        REQUIRE(s.submit(c));
        REQUIRE(log.size() == 1);
        CHECK(log[0] == "enable https://github.com/me/notes.git");
    }
    log.clear();
    {
        auto s = w.form(c, enabler);  // açıkken not dizini değişti: yeni dizinde depo kurulur
        focus_key(s, "notes_dir");
        type(s, "x");
        REQUIRE(s.submit(c));
        CHECK(log.size() == 1);
    }
    log.clear();
    {
        auto s = w.form(c, enabler);  // açıkken remote değişti: yeniden kurulum
        set_remote(s, "https://github.com/me/other.git");
        REQUIRE(s.submit(c));
        REQUIRE(log.size() == 1);
        CHECK(log[0] == "enable https://github.com/me/other.git");
    }
    log.clear();
    {
        auto s = w.form(c, enabler);  // kapatmak enable çağırmaz
        enable_toggle(s);
        const auto saved = s.submit(c);
        REQUIRE(saved);
        CHECK_FALSE(saved->sync.enabled);
        CHECK(log.empty());
    }
}

TEST_CASE("enable success against a local bare remote saves sync.remote and sync.enabled") {
    World w;
    const fs::path remote = w.make_remote("remote.git");
    const auto base = w.config();
    auto s = w.form(base);
    set_remote(s, utf8(remote));
    enable_toggle(s);
    const auto saved = s.submit(base);
    REQUIRE_MESSAGE(saved, s.error());
    CHECK(saved->sync.enabled);
    CHECK(fs::exists(w.notes / ".git"));
    const std::string text = config_text();
    CHECK(has(text, "enabled = true"));
    CHECK(has(text, "remote"));
    CHECK(s.error().empty());
}

TEST_CASE("an enable error stays inline on the form and nothing is saved") {
    World w;
    const auto base = w.config();
    auto s = w.form(base);
    set_remote(s, utf8(w.dir.path() / "does-not-exist.git"));
    enable_toggle(s);
    CHECK_FALSE(s.submit(base));
    CHECK_FALSE(s.error().empty());
    CHECK_FALSE(has(config_text(), "enabled = true"));
    CHECK_FALSE(fs::exists(w.notes / ".git"));
    CHECK(has(screen_text(s), s.error().substr(0, 20)));  // formda görünür
    CHECK(s.changed());                                   // form açık, değişiklikler duruyor
}

// Mevcut origin `.git/config`'ten gelir (güvenilmez): soru onu denetim karakterleri görünür işaretle çizer.
TEST_CASE("the origin question draws the repository address without hidden controls") {
    World w;
    const auto enabler = [](const core::Config&, bool) -> std::expected<void, core::Error> {
        return std::unexpected(core::Error{core::Error::Code::Io,
                                           "sync.remote_mismatch",
                                           {{"current", "https://old\xE2\x80\xAE.example.invalid/n.git"},
                                            {"remote", "https://github.com/me/notes.git"}}});
    };
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    auto s = w.form(c, enabler);
    enable_toggle(s);
    CHECK_FALSE(s.submit(c));
    REQUIRE(s.replace_confirm() != nullptr);
    const std::string screen = screen_text(s, 80, 24);
    CHECK(has(screen, "example.invalid"));
    CHECK_FALSE(has(screen, "\xE2\x80\xAE"));
}

TEST_CASE("an origin mismatch asks to replace it; declining changes nothing, accepting replaces origin") {
    World w;
    const fs::path first = w.make_remote("first.git");
    const fs::path second = w.make_remote("second.git");
    REQUIRE(sync::SyncService(w.config()).enable(utf8(first)));  // notes_dir -> origin = first

    const auto base = w.config();
    auto s = w.form(base);
    set_remote(s, utf8(second));
    enable_toggle(s);
    CHECK_FALSE(s.submit(base));
    REQUIRE(s.replace_confirm() != nullptr);
    CHECK_FALSE(s.replace_confirm()->yes_selected());  // varsayılan Vazgeç
    CHECK(has(s.replace_confirm()->message(), "second.git"));
    CHECK(has(screen_text(s), "Replace"));

    CHECK(s.handle(Key::Enter) == Outcome::Editing);  // varsayılan seçimle onaylamak = vazgeç
    CHECK(s.replace_confirm() == nullptr);
    CHECK_FALSE(has(config_text(), "enabled = true"));

    CHECK_FALSE(s.submit(base));
    REQUIRE(s.replace_confirm() != nullptr);
    s.handle(Key::Right);
    CHECK(s.handle(Key::Enter) == Outcome::Replace);
    const auto saved = s.submit(base, true);
    REQUIRE_MESSAGE(saved, s.error());
    CHECK(saved->sync.enabled);
    CHECK(has(config_text(), "enabled = true"));
}

TEST_CASE("sync_restart_needed ignores editor and author changes but not sync wiring changes") {
    const core::Config base = core::parse_config("").config;
    const auto changed = [&](auto edit) {
        core::Config next = base;
        edit(next);
        return tui::sync_restart_needed(base, next);
    };
    CHECK_FALSE(tui::sync_restart_needed(base, base));
    CHECK_FALSE(changed([](core::Config& c) { c.editor = "vim"; }));
    CHECK_FALSE(changed([](core::Config& c) { c.sync.author_name = "Baska"; }));
    CHECK_FALSE(changed([](core::Config& c) { c.sync.author_email = "b@example.invalid"; }));
    CHECK(changed([](core::Config& c) { c.sync.enabled = !c.sync.enabled; }));
    CHECK(changed([](core::Config& c) { c.sync.remote = "https://example.invalid/r.git"; }));
    CHECK(changed([](core::Config& c) { c.sync.branch = "baska"; }));
    CHECK(changed([](core::Config& c) { c.sync.idle_seconds += 1; }));
    CHECK(changed([](core::Config& c) { c.notes_dir /= "alt"; }));
}

TEST_CASE("connection test: Enter on its slot asks to test; the worker enables and syncs, the result shows") {
    World w;
    auto c = w.config();
    c.notes_dir = w.dir.path() / "fresh";  // World'ün a.md'si eski düzendir; senkron yeni depoda koşar
    c.sync.remote = utf8(w.make_remote("remote.git"));
    auto s = w.form(c);
    s.handle(Key::Up);
    REQUIRE(s.on_button());
    s.handle(Key::Up);
    REQUIRE(s.on_test());
    CHECK(s.handle(Key::Enter) == Outcome::Test);

    const std::optional<core::Config> candidate = s.begin_test(c);
    REQUIRE(candidate.has_value());
    CHECK(s.testing());
    CHECK_FALSE(s.begin_test(c).has_value());  // tek test birden
    // main'in işçisi aynı adayla enable + tek senkron koşar; sonucu forma döner.
    const auto outcome = app::run_connection_test(*candidate, tui::system_sync_enabler());
    CHECK(outcome.has_value());
    s.finish_test(outcome);
    CHECK_FALSE(s.testing());
    CHECK(s.test_result() == core::tr("app.test_ok"));
    const std::string small = screen_text(s, 60, 15);
    CAPTURE(small);
    CHECK(has(small, core::tr("tui.settings_test")));
    CHECK(has(screen_text(s, 80, 24), core::tr("app.test_ok")));
    CHECK_FALSE(core::load_config().config.sync.enabled);  // test kaydetmez

    // Bekleyen test yokken gelen geç sonuç yok sayılır (form kapanıp yeniden açıldı).
    s.finish_test(std::unexpected(core::Error{core::Error::Code::Io, "sync.keychain_unavailable", {}}));
    CHECK(s.test_result() == core::tr("app.test_ok"));
}

// Sev-3 (T1+T2 review): test kasaya yazmazdı ama TUI yazıyordu; iptal edilen formda token kasada kalıyordu.
// GUI ile aynı: test kasaya dokunmaz, enable da UI iş parçacığında koşmaz (ağa çıkar, arayüz donardı).
TEST_CASE("connection test: begin_test writes nothing to the keychain and leaves enable to the worker") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://github.com/me/notes.git";
    int enables = 0;
    auto s = w.form(c, [&](const core::Config&, bool) -> std::expected<void, core::Error> {
        ++enables;
        return {};
    });
    focus_token(s);
    type(s, kSecret);
    const std::optional<core::Config> candidate = s.begin_test(c);
    REQUIRE(candidate.has_value());
    CHECK(s.testing());
    CHECK(enables == 0);
    CHECK(w.vault.calls.empty());
    CHECK(s.token_length() == kSecret.size());  // yazılan token formda kalır: Kaydet ya da Enter saklar
}

// Sev-3: yazılmış token yazıldığı host'a aittir; remote başka host'a dönünce Kaydet onu yeni host'a
// götürürdü.
TEST_CASE("a typed token is dropped when the remote moves to another host and never reaches it") {
    World w;
    auto c = w.config();
    c.sync.remote = "https://a.example/me/notes.git";
    auto s = w.form(c, [](const core::Config&, bool) -> std::expected<void, core::Error> { return {}; });
    focus_token(s);
    type(s, kSecret);
    REQUIRE(s.token_length() == kSecret.size());
    set_remote(s, "https://b.example/me/notes.git");
    CHECK(s.token_length() == 0);
    enable_toggle(s);
    REQUIRE(s.submit(c));
    CHECK(w.vault.calls.empty());
    CHECK_FALSE(w.vault.tokens.contains("b.example"));
}

TEST_CASE("connection test: a failing enable is the result and no sync starts") {
    World w;
    auto c = w.config();
    c.notes_dir = w.dir.path() / "fresh";
    c.sync.remote = utf8(w.make_remote("remote.git"));
    auto s = w.form(c);
    const std::optional<core::Config> candidate = s.begin_test(c);
    REQUIRE(candidate.has_value());
    const auto outcome = app::run_connection_test(
        *candidate, [](const core::Config&, bool) -> std::expected<void, core::Error> {
            return std::unexpected(core::Error{core::Error::Code::Io, "sync.keychain_unavailable", {}});
        });
    CHECK_FALSE(outcome.has_value());
    CHECK_FALSE(fs::exists(c.notes_dir / ".git"));  // enable düştü: senkron başlamadı, depo kurulmadı
    s.finish_test(outcome);
    CHECK_FALSE(s.testing());
    CHECK(has(s.test_result(), core::tr("sync.keychain_unavailable")));
    // Uzun neden düğmeyi en küçük pencerede de itmez.
    const std::string small = screen_text(s, 60, 15);
    CAPTURE(small);
    CHECK(has(small, core::tr("tui.settings_test")));
    CHECK(has(small, core::tr("app.settings_save")));
}
