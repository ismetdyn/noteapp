#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "gui/gui_state.hpp"
#include "gui/settings_page.hpp"
#include "sync/git_runtime.hpp"
#include "window_fixture.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <git2.h>
#include <imgui.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using gui_test::click_button;
using gui_test::make_view;
using gui_test::press;
using gui_test::RenderedText;
using gui_test::type_text;
using gui_test::WindowFixture;
using noteapp::core::tr;
using noteapp::gui::GuiState;
using noteapp::gui::SettingsPage;
using noteapp::gui::Tab;
using noteapp::gui::View;

namespace {

constexpr const char kCheckMark[] = "\xE2\x9C\x93";
constexpr const char kWarningSign[] = "\xE2\x9A\xA0";
constexpr const char kRunningSign[] = "\xE2\x9F\xB3";
constexpr std::string_view kSecretToken = "ghp_gizli_deger";

std::string utf8(const fs::path& path) {
    return noteapp::test::utf8(path);
}

struct SettingsFixture : WindowFixture {
    std::vector<std::string> stored_hosts;  // sahte kasa
    std::vector<std::string> stored_tokens;
    int replace_asked = 0;
    bool replace_answer = true;
    std::vector<std::string> infos;
    noteapp::sync::GitRuntime git;

    SettingsFixture() {
        // Pencere dışı süreç (note --background) da aynı notes_dir'i görsün: config dosyaya yazılır.
        REQUIRE(noteapp::core::save_config(config));
    }

    std::unique_ptr<GuiState> make_state_wired() {
        auto state = make_state();
        state->sync_note_exe = fs::path(NOTEAPP_NOTE_EXE);
        state->deciders.confirm_replace_origin = [this](const std::string&) {
            ++replace_asked;
            return replace_answer;
        };
        state->deciders.info = [this](const std::string& message) { infos.push_back(message); };
        SettingsPage& page = state->settings();
        page.tokens.has = [this](std::string_view host) {
            return std::ranges::find(stored_hosts, std::string(host)) != stored_hosts.end();
        };
        page.tokens.store = [this](std::string_view host,
                                   std::string_view token) -> std::expected<void, noteapp::core::Error> {
            stored_hosts.emplace_back(host);
            stored_tokens.emplace_back(token);
            return {};
        };
        page.tokens.clear = [this](std::string_view host) -> std::expected<void, noteapp::core::Error> {
            std::erase(stored_hosts, std::string(host));
            return {};
        };
        return state;
    }

    static std::string config_text() {
        std::ifstream in(noteapp::core::config_file(), std::ios::binary);
        std::stringstream out;
        out << in.rdbuf();
        return out.str();
    }
    static std::string get(const char* key) {
        for (const auto& [k, v] : noteapp::core::config_entries(noteapp::core::load_config().config)) {
            if (k == key)
                return v;
        }
        return "<none>";
    }
    // Yerel çıplak uzak depo (libgit2: sistemde git olması şart değil).
    fs::path make_bare(const char* name) const {
        const fs::path dir = home.path() / name;
        git_repository* repo = nullptr;
        REQUIRE(git_repository_init(&repo, dir.string().c_str(), 1) == 0);
        git_repository_free(repo);
        return dir;
    }
    static void enable_form(SettingsPage& page, const fs::path& remote) {
        page.set_field("sync.enabled", "true");
        page.set_field("sync.remote", utf8(remote));
        page.set_field("sync.author_name", "Test Yazar");
        page.set_field("sync.author_email", "test@noteapp.invalid");
    }
    // İşçi sonucu UI iş parçacığına tick'te geçer.
    static bool wait_for(GuiState& state, const std::function<bool()>& done,
                         std::chrono::seconds limit = std::chrono::seconds(30)) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            state.tick(std::chrono::steady_clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return done();
    }
};

void show_settings(GuiState& state, View& view) {
    state.show_settings_tab();
    for (int frame = 0; frame < 4; ++frame)
        view.draw_frame();
}

}  // namespace

TEST_CASE_FIXTURE(SettingsFixture,
                  "saved value is visible through load_config and the token never reaches config.toml") {
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    page.set_field("editor", "vim");
    page.set_field("types", "md, txt, py");
    page.set_field("default_type", "py");
    page.set_field("sync.author_name", "Test Yazar");
    page.save();

    CHECK(get("editor") == "vim");
    CHECK(get("types") == "md, txt, py");
    CHECK(get("default_type") == "py");
    CHECK(get("sync.author_name") == "Test Yazar");
    CHECK(page.field_error("editor").empty());

    // Token yalnız kasaya gider (https remote'ta) ve config.toml'da hiçbir yerde geçmez.
    page.set_field("sync.remote", "https://github.com/kisi/notlar.git");
    page.token = kSecretToken;
    page.save_token();
    REQUIRE(stored_tokens.size() == 1);
    CHECK(stored_tokens.front() == kSecretToken);
    CHECK(page.token.empty());  // alan temizlenir, değer asla geri gösterilmez
    page.save();
    CHECK(config_text().find(kSecretToken) == std::string::npos);
    CHECK(config_text().find("token") == std::string::npos);
}

TEST_CASE_FIXTURE(SettingsFixture, "an invalid value is shown under its field and nothing is saved") {
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    const std::string before = config_text();
    page.set_field("editor", "vim");  // geçerli değişiklik de kaydedilmemeli
    page.set_field("default_type", "bad.type");
    page.save();

    CHECK_FALSE(page.field_error("default_type").empty());
    CHECK(page.field_error("editor").empty());
    CHECK(config_text() == before);

    // Düzeltilince hata silinir ve kayıt geçer.
    page.set_field("default_type", "txt");
    page.save();
    CHECK(page.field_error("default_type").empty());
    CHECK(get("editor") == "vim");
}

TEST_CASE_FIXTURE(SettingsFixture, "a broken config file is never overwritten") {
    {
        std::ofstream out(noteapp::core::config_file(), std::ios::binary | std::ios::trunc);
        out << "editor = = bozuk\n";
    }
    const std::string before = config_text();
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    page.set_field("editor", "vim");
    page.save();

    CHECK(config_text() == before);
    CHECK_FALSE(page.global_error().empty());
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "changing notes_dir flushes first and keeps the notes: they live in the database") {
    add_note("eski.md", "old");
    const fs::path other = home.path() / "other-notes";
    fs::create_directories(other);
    auto state = make_state_wired();
    REQUIRE(state->model().row_count() == 1);

    state->edit_text("old edited");  // kirli tampon yeni dizine bağlanmadan önce kaydedilir
    state->settings().set_field("notes_dir", utf8(other));
    state->settings().save();

    CHECK(read_note("eski.md") == "old edited");
    REQUIRE(state->model().row_count() == 1);
    CHECK(state->model().display(0) == "eski.md");
    CHECK(fs::path(get("notes_dir")) == other);
}

TEST_CASE_FIXTURE(SettingsFixture, "a failing flush cancels a notes_dir change and saves nothing") {
    add_note("eski.md", "old", 10);
    const fs::path other = home.path() / "other-notes";
    fs::create_directories(other);
    auto state = make_state_wired();
    state->edit_text("mine");
    add_note("eski.md", "external", 0);  // çakışma; varsayılan karar Vazgeç
    const std::string before = config_text();

    state->settings().set_field("notes_dir", utf8(other));
    state->settings().save();

    CHECK_FALSE(state->settings().global_error().empty());
    CHECK(config_text() == before);
    CHECK(state->buffer() == "mine");
    CHECK(state->dirty());
}

TEST_CASE_FIXTURE(SettingsFixture, "the notes_dir browse button goes through the decider") {
    const fs::path other = home.path() / "picked";
    auto state = make_state_wired();
    auto view = make_view(*state);
    state->deciders.browse_dir = [&](const std::string&) { return std::optional<std::string>(utf8(other)); };
    show_settings(*state, *view);
    click_button(*view, "browse");
    CHECK(state->settings().field("notes_dir") == utf8(other));
}

TEST_CASE_FIXTURE(SettingsFixture, "the token row is enabled only for HTTPS remotes") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);

    page.set_field("sync.remote", "C:/some/local/remote.git");
    CHECK_FALSE(page.token_enabled());
    page.set_field("sync.remote", "git@github.com:kisi/notlar.git");
    CHECK_FALSE(page.token_enabled());
    view->draw_frame();
    click_button(*view, "token_input");
    type_text(*view, "x");
    CHECK(page.token.empty());  // kilitli alan yazı almaz

    // Model de https dışında kasaya gitmez. Düğmelerin kilidini ayrıca görmek için remote, field_edited
    // olmadan https yapılır: satır kilitli kalır, tek koruma çizimdeki kilittir.
    page.field("sync.remote") = "https://github.com/kisi/notlar.git";
    page.token = kSecretToken;
    stored_hosts.push_back("github.com");
    click_button(*view, "token_save");
    CHECK(stored_tokens.empty());
    click_button(*view, "token_clear");
    CHECK(stored_hosts.size() == 1);
    stored_hosts.clear();
    page.token.clear();

    page.set_field("sync.remote", "https://github.com/kisi/notlar.git");
    CHECK(page.token_enabled());
    CHECK_FALSE(page.token_stored());
    view->draw_frame();
    click_button(*view, "token_input");
    type_text(*view, kSecretToken.data());
    CHECK(page.token == kSecretToken);
    click_button(*view, "token_save");
    REQUIRE(stored_tokens.size() == 1);
    CHECK(stored_tokens.front() == kSecretToken);
    CHECK(page.token.empty());
    CHECK(page.token_stored());  // "kasada kayıtlı"

    page.set_field("sync.remote", "https://github.com/kisi/baska.git");
    CHECK(page.token_stored());  // aynı host
    click_button(*view, "token_clear");
    CHECK(stored_hosts.empty());
    CHECK_FALSE(page.token_stored());
}

TEST_CASE_FIXTURE(SettingsFixture, "the drawn token field never shows the typed value") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    page.set_field("sync.remote", "https://github.com/kisi/notlar.git");
    page.token = kSecretToken;

    const RenderedText rendered;
    view->draw_frame();

    CHECK(rendered.contains(tr("gui.token_label")));  // satır gerçekten çizildi
    CHECK_FALSE(rendered.contains(kSecretToken));
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "clearing the token also drops the typed value so another host never gets it") {
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    page.set_field("sync.remote", "https://a.example/kisi/notlar.git");
    page.token = kSecretToken;

    page.clear_token();
    CHECK(page.token.empty());

    page.set_field("sync.remote", "https://b.example/kisi/notlar.git");
    page.save_token();
    CHECK(std::ranges::find(stored_tokens, kSecretToken) == stored_tokens.end());
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "enabling sync against a local bare remote saves the config and starts syncing") {
    add_note("a.md", "# a\n");
    const fs::path remote = make_bare("remote.git");
    auto state = make_state_wired();
    CHECK(state->sync_label() == tr("gui.sync_off"));

    enable_form(state->settings(), remote);
    state->settings().save();

    CHECK(get("sync.enabled") == "true");
    CHECK(get("sync.remote") == remote.string());
    CHECK(state->settings().field_error("sync.remote").empty());
    CHECK(fs::exists(config.notes_dir / ".git"));
    // AutoSync açılışta bir senkron koşar; durum çubuğu "kapalı"dan çıkıp ✓'ye gelir.
    CHECK(wait_for(*state, [&] { return state->sync_label().starts_with(kCheckMark); }));
}

TEST_CASE_FIXTURE(SettingsFixture, "an origin mismatch asks before replacing and declining saves nothing") {
    const fs::path first = make_bare("first.git");
    const fs::path second = make_bare("second.git");
    {  // notes_dir zaten `first` remote'lu bir depo
        git_repository* repo = nullptr;
        REQUIRE(git_repository_init(&repo, config.notes_dir.string().c_str(), 0) == 0);
        git_remote* remote = nullptr;
        REQUIRE(git_remote_create(&remote, repo, "origin", first.string().c_str()) == 0);
        git_remote_free(remote);
        git_repository_free(repo);
    }
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    enable_form(page, second);

    replace_answer = false;
    page.save();
    CHECK(replace_asked == 1);
    CHECK(get("sync.enabled") == "false");
    CHECK_FALSE(page.field_error("sync.remote").empty());

    replace_answer = true;
    page.save();
    CHECK(replace_asked == 2);
    CHECK(get("sync.enabled") == "true");
    CHECK(get("sync.remote") == second.string());
    CHECK(page.field_error("sync.remote").empty());
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "the test connection button enables and runs one sync, showing the outcome") {
    add_note("a.md", "# a\n");
    const fs::path remote = make_bare("remote.git");
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    page.set_field("sync.remote", utf8(remote));
    page.set_field("sync.author_name", "Test Yazar");
    page.set_field("sync.author_email", "test@noteapp.invalid");
    click_button(*view, "test");
    CHECK(wait_for(*state, [&] { return !page.test_result().empty(); }));
    CHECK(page.test_result().starts_with(kCheckMark));
    CHECK(fs::exists(config.notes_dir / ".git"));

    // Geçersiz adres: sonuç hatadır, ✓ değil.
    page.set_field("sync.remote", utf8(remote / "yok"));
    replace_answer = true;
    state->deciders.confirm_replace_origin = [](const std::string&) { return true; };
    view->draw_frame();  // sonuç UI iş parçacığında geldi; düğmenin kilidi kalksın
    click_button(*view, "test");
    CHECK(wait_for(*state, [&] { return !page.test_result().empty() && !page.testing(); }));
    CHECK(page.test_result().starts_with(kWarningSign));
}

TEST_CASE_FIXTURE(SettingsFixture, "the status label shows off, running, last success and last error") {
    auto state = make_state_wired();
    noteapp::sync::SyncStatus status;
    state->show_sync_status(status, true);
    CHECK(state->sync_label() == kRunningSign);

    std::tm local{};
    local.tm_year = 126;
    local.tm_mon = 3;
    local.tm_mday = 10;
    local.tm_hour = 14;
    local.tm_min = 5;
    local.tm_isdst = -1;
    status.last_success = std::chrono::system_clock::from_time_t(std::mktime(&local));
    state->show_sync_status(status, false);
    CHECK(state->sync_label() == std::string(kCheckMark) + " 14:05");

    status.last_error = noteapp::core::Error{noteapp::core::Error::Code::Io, "sync.timeout", {}};
    state->show_sync_status(status, false);
    CHECK(state->sync_label().starts_with(kWarningSign));
    CHECK(state->sync_label().size() > 4);
}

TEST_CASE_FIXTURE(SettingsFixture, "F5 asks for a sync now and Ctrl+comma opens the settings tab") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    CHECK(state->sync_requests() == 0);
    press(*view, ImGuiKey_F5);
    CHECK(state->sync_requests() == 1);
    press(*view, ImGuiMod_Ctrl | ImGuiKey_Comma);
    CHECK(state->tab() == Tab::Settings);
}

TEST_CASE_FIXTURE(SettingsFixture, "changing language only announces a restart") {
    auto state = make_state_wired();
    state->settings().set_field("language", "tr");
    state->settings().save();
    CHECK(get("language") == "tr");
    CHECK(infos.size() == 1);
    noteapp::core::init_i18n("en");
}

TEST_CASE_FIXTURE(SettingsFixture, "revert restores the saved values") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    const std::string saved = state->settings().field("editor");
    show_settings(*state, *view);
    state->settings().set_field("editor", "emacs");
    click_button(*view, "revert");
    CHECK(state->settings().field("editor") == saved);
}

TEST_CASE_FIXTURE(SettingsFixture, "labels and fields form two aligned columns under General then Sync") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    const auto& rects = view->item_rects();
    const float label_x = rects.at("setting_label:language").min_x;
    const float field_x = rects.at("setting:language").min_x;
    CHECK(label_x < field_x);
    for (const std::string& key : page.keys()) {
        CAPTURE(key);
        const auto& label = rects.at("setting_label:" + key);
        const auto& input = rects.at("setting:" + key);
        CHECK(label.min_x == doctest::Approx(label_x));
        CHECK(input.min_x == doctest::Approx(field_x));
        CHECK(label.max_x <= input.min_x);
    }
    CHECK(rects.at("token_label").min_x == doctest::Approx(label_x));
    CHECK(rects.at("token_input").min_x == doctest::Approx(field_x));

    const auto above = [&](const std::string& upper, const std::string& lower) {
        CAPTURE(upper);
        CAPTURE(lower);
        CHECK(rects.at(upper).max_y <= rects.at(lower).min_y);
    };
    above("settings_group_general", "setting:language");
    for (const char* key : {"language", "notes_dir", "default_type", "default_folder", "types", "editor"})
        above(std::string("setting:") + key, "settings_group_sync");
    for (const char* key : {"sync.enabled", "sync.remote", "sync.branch", "sync.idle_seconds",
                            "sync.author_name", "sync.author_email"})
        above("settings_group_sync", std::string("setting:") + key);
    above("settings_group_sync", "token_input");
    above("settings_group_sync", "test");
}

TEST_CASE_FIXTURE(SettingsFixture, "text fields grow with the window") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    show_settings(*state, *view);
    const auto width = [&] {
        const auto& rect = view->item_rects().at("setting:editor");
        return rect.max_x - rect.min_x;
    };
    const float before = width();
    int count = 0;
    const std::unique_ptr<SDL_Window*, decltype(&SDL_free)> windows(SDL_GetWindows(&count), &SDL_free);
    REQUIRE(count == 1);
    REQUIRE(SDL_SetWindowSize(windows.get()[0], 1600, 650));
    gui_test::settle(*view);
    CHECK(width() > before + 500.0F);
}

TEST_CASE_FIXTURE(SettingsFixture, "language options read as text while the stored value stays a code") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    REQUIRE(page.field("language") == "auto");
    {
        const RenderedText rendered;
        view->draw_frame();
        CHECK(rendered.contains(tr("gui.language_auto")));
    }
    click_button(*view, "setting:language");
    click_button(*view, "language:tr");
    CHECK(page.field("language") == "tr");
    page.save();
    CHECK(get("language") == "tr");
    noteapp::core::init_i18n("en");
}

TEST_CASE_FIXTURE(SettingsFixture, "the token row states why it is locked and whether a token is stored") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    const auto drawn = [&](const std::string& text) {
        const RenderedText rendered;
        view->draw_frame();
        return rendered.contains(text);
    };
    const std::string stored = tr("gui.token_state", {{"state", tr("gui.token_stored")}});
    const std::string none = tr("gui.token_state", {{"state", tr("gui.token_none")}});

    page.set_field("sync.remote", "git@github.com:kisi/notlar.git");
    CHECK(drawn(tr("gui.token_https_only")));
    CHECK_FALSE(drawn(none));
    CHECK_FALSE(drawn(stored));

    page.set_field("sync.remote", "https://github.com/kisi/notlar.git");
    CHECK_FALSE(drawn(tr("gui.token_https_only")));
    CHECK(drawn(none));

    page.token = kSecretToken;
    page.save_token();
    CHECK(drawn(stored));
    CHECK_FALSE(drawn(none));

    gui_test::settle(*view);
    const auto& rects = view->item_rects();
    CHECK(rects.at("token_save").min_y < rects.at("token_state").min_y);
    CHECK(rects.at("token_input").max_y <= rects.at("token_state").min_y);
    // Geniş pencerede düğmeler alanla aynı satırda.
    CHECK(rects.at("token_save").min_y == doctest::Approx(rects.at("token_input").min_y));
    CHECK(rects.at("token_clear").min_y == doctest::Approx(rects.at("token_input").min_y));
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "sync off locks the sync fields but keeps their values and the token row") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    page.set_field("sync.remote", "https://github.com/kisi/notlar.git");
    REQUIRE(page.field("sync.enabled") == "false");
    view->draw_frame();
    const auto is_locked = [&](const std::string& name) { return view->item_rects().at(name).disabled; };
    for (const char* key :
         {"sync.remote", "sync.branch", "sync.idle_seconds", "sync.author_name", "sync.author_email"}) {
        CAPTURE(key);
        CHECK(is_locked(std::string("setting:") + key));
    }
    CHECK_FALSE(is_locked("setting:sync.enabled"));
    CHECK_FALSE(is_locked("setting:editor"));
    // Token kasaya gider, enable'dan önce gerekir ve senkron kapalıyken de silinebilmelidir.
    CHECK_FALSE(is_locked("token_input"));
    CHECK_FALSE(is_locked("test"));  // "enable (gerekirse) + tek senkron" davranışı

    click_button(*view, "setting:sync.remote");
    type_text(*view, "x");
    CHECK(page.field("sync.remote") == "https://github.com/kisi/notlar.git");

    click_button(*view, "setting:sync.enabled");
    CHECK(page.field("sync.enabled") == "true");
    gui_test::settle(*view);
    CHECK_FALSE(is_locked("setting:sync.remote"));
    CHECK(page.field("sync.remote") == "https://github.com/kisi/notlar.git");
}

TEST_CASE_FIXTURE(SettingsFixture, "revert and save stay disabled until something changed") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    const auto& rects = view->item_rects();
    CHECK(rects.at("revert").disabled);
    CHECK(rects.at("save").disabled);
    CHECK_FALSE(rects.contains("settings_unsaved"));

    const std::string saved = page.field("editor");
    page.set_field("editor", "nano");
    view->draw_frame();
    CHECK_FALSE(view->item_rects().at("revert").disabled);
    CHECK_FALSE(view->item_rects().at("save").disabled);
    CHECK(view->item_rects().contains("settings_unsaved"));

    page.set_field("editor", saved);  // aynı değere dönmek değişiklik değildir
    view->draw_frame();
    CHECK(view->item_rects().at("save").disabled);
    CHECK_FALSE(view->item_rects().contains("settings_unsaved"));

    page.set_field("editor", "nano");
    view->draw_frame();
    click_button(*view, "save");
    CHECK(get("editor") == "nano");
    CHECK(view->item_rects().at("save").disabled);  // kayıt sonrası temiz
}

TEST_CASE_FIXTURE(SettingsFixture, "a field error is drawn under its field in the field column") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    page.set_field("default_type", "bad.type");
    click_button(*view, "save");
    gui_test::settle(*view);
    REQUIRE_FALSE(page.field_error("default_type").empty());
    const auto& rects = view->item_rects();
    CHECK(rects.at("setting_error:default_type").min_y >= rects.at("setting:default_type").max_y);
    CHECK(rects.at("setting_error:default_type").min_x ==
          doctest::Approx(rects.at("setting:default_type").min_x));
    CHECK(rects.at("setting_error:default_type").max_y <= rects.at("setting:default_folder").min_y);
}

TEST_CASE_FIXTURE(SettingsFixture, "editor, remote and idle fields carry a hint") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    show_settings(*state, *view);
    const auto& rects = view->item_rects();
    for (const char* key : {"editor", "sync.remote", "sync.idle_seconds"}) {
        CAPTURE(key);
        CHECK(rects.at(std::string("setting_hint:") + key).min_y >=
              rects.at(std::string("setting:") + key).max_y);
    }
    const RenderedText rendered;
    view->draw_frame();
    CHECK(rendered.contains("$VISUAL"));
    CHECK(rendered.contains("$EDITOR"));
}

TEST_CASE_FIXTURE(SettingsFixture, "leaving the settings tab keeps unsaved values") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    page.set_field("editor", "nano");
    state->focus_search();
    gui_test::settle(*view);
    REQUIRE(state->tab() == Tab::Notes);
    show_settings(*state, *view);
    CHECK(page.field("editor") == "nano");
    CHECK(view->item_rects().contains("settings_unsaved"));
}

TEST_CASE_FIXTURE(SettingsFixture, "closing with unsaved settings asks and honours each answer") {
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    int asked = 0;
    auto answer = noteapp::gui::CloseChoice::Cancel;
    state->deciders.close_unsaved_settings = [&] {
        ++asked;
        return answer;
    };
    const std::string saved = get("editor");

    SUBCASE("cancel keeps the window and the typed value") {
        page.set_field("editor", "nano");
        CHECK_FALSE(state->request_close());
        CHECK(asked == 1);
        CHECK(page.field("editor") == "nano");
        CHECK(get("editor") == saved);
        CHECK(state->tab() == Tab::Settings);
    }
    SUBCASE("save writes the value and closes") {
        answer = noteapp::gui::CloseChoice::Save;
        page.set_field("editor", "nano");
        CHECK(state->request_close());
        CHECK(asked == 1);
        CHECK(get("editor") == "nano");
    }
    SUBCASE("a save that fails keeps the window open with the error under the field") {
        answer = noteapp::gui::CloseChoice::Save;
        page.set_field("default_type", "bad.type");
        CHECK_FALSE(state->request_close());
        CHECK_FALSE(page.field_error("default_type").empty());
        CHECK(state->tab() == Tab::Settings);
    }
    SUBCASE("discard closes without writing") {
        answer = noteapp::gui::CloseChoice::Discard;
        page.set_field("editor", "nano");
        CHECK(state->request_close());
        CHECK(asked == 1);
        CHECK(get("editor") == saved);
    }
    SUBCASE("nothing changed: no question") {
        CHECK(state->request_close());
        CHECK(asked == 0);
    }
}

TEST_CASE_FIXTURE(SettingsFixture, "the production close prompt answers through the shared modal") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    view->install_deciders();
    SettingsPage& page = state->settings();
    show_settings(*state, *view);
    page.set_field("editor", "nano");
    int frame = 0;
    view->on_modal_frame = [&] {
        constexpr int kSettledFrame = 6;
        if (++frame == kSettledFrame)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    };
    CHECK_FALSE(state->request_close());  // Esc = Vazgeç
    view->on_modal_frame = nullptr;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    CHECK(frame >= 6);
    CHECK(page.field("editor") == "nano");
}

TEST_CASE_FIXTURE(SettingsFixture, "the save button on the settings tab writes the config") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    show_settings(*state, *view);
    state->settings().set_field("editor", "nano");
    click_button(*view, "save");
    CHECK(get("editor") == "nano");
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "saving unrelated settings keeps the running sync, a branch change restarts it") {
    add_note("a.md", "# a\n");
    const fs::path remote = make_bare("remote.git");
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    enable_form(page, remote);
    page.save();
    REQUIRE(wait_for(*state, [&] { return state->sync_label().starts_with(kCheckMark); }));
    CHECK(state->sync_restarts() == 1);

    // Editör ve yazar bilgisi AutoSync'i ilgilendirmez: çalışan süreç öldürülmemeli.
    page.set_field("editor", "vim");
    page.set_field("sync.author_name", "Başka Yazar");
    page.save();
    CHECK(get("editor") == "vim");
    CHECK(state->sync_restarts() == 1);

    page.set_field("sync.branch", "other");
    page.save();
    CHECK(get("sync.branch") == "other");
    CHECK(state->sync_restarts() == 2);
}

// ---- Senkron bağlantısı ----

TEST_CASE_FIXTURE(SettingsFixture, "every write, move, delete and create notifies the sync") {
    add_note("work/a.md", "a", 10);
    auto state = make_state_wired();
    state->deciders.confirm_delete = [](const std::string&) { return true; };
    state->deciders.folder_name = [](const std::string&, const std::string&) {
        return std::optional<std::string>("yeni");
    };
    std::size_t expected = state->change_count();
    REQUIRE(expected == 0);

    state->edit_text("edited");
    state->save_now();  // yazma
    CHECK(state->change_count() == ++expected);
    state->change_type("txt");  // taşıma
    CHECK(state->change_count() == ++expected);
    state->new_folder("");  // klasör oluşturma
    CHECK(state->change_count() == ++expected);
    state->rename_folder("yeni");  // aynı ad: işlem yok, bildirim yok
    CHECK(state->change_count() == expected);
    state->delete_folder("yeni");  // klasör silme
    CHECK(state->change_count() == ++expected);
    state->deciders.folder_name = [](const std::string&, const std::string&) {
        return std::optional<std::string>("proj");
    };
    state->rename_folder("work");  // klasör taşıma
    CHECK(state->change_count() == ++expected);
    state->delete_note();  // not silme
    CHECK(state->change_count() == ++expected);
    add_note("c.md", "c");
    state->rescan();
    REQUIRE(state->current_path());
    state->trash_note();  // çöpe atma
    CHECK(state->change_count() == ++expected);
    state->undo_trash();  // geri alma
    CHECK(state->change_count() == ++expected);
    state->trash_note();
    ++expected;
    state->open_trash();
    state->restore_trashed();  // çöpten geri getirme
    CHECK(state->change_count() == ++expected);
    state->close_trash();
    state->trash_note();
    ++expected;
    state->open_trash();
    state->purge_trashed();  // çöpten kalıcı silme
    CHECK(state->change_count() == ++expected);
    state->close_trash();
    add_note("d.md", "d");
    state->rescan();
    state->trash_note();
    ++expected;
    state->empty_trash();  // çöpü boşaltma
    CHECK(state->change_count() == ++expected);

    state->deciders.save_dialog = [](const noteapp::gui::SaveDialogRequest& request) {
        return std::optional<noteapp::gui::SaveDetails>(request.initial);
    };
    state->new_note();
    state->edit_text("taslak\n");
    state->save_now();  // not oluşturma
    CHECK(state->change_count() == ++expected);
}

TEST_CASE_FIXTURE(SettingsFixture,
                  "close stops the sync after the save flow, so the last edit reaches the remote") {
    add_note("a.md", "# a\n");
    const fs::path remote = make_bare("remote.git");
    auto state = make_state_wired();
    SettingsPage& page = state->settings();
    enable_form(page, remote);
    page.set_field("sync.idle_seconds", "3600");  // debounce dolmaz: son senkronu yalnız kapanış tetikler
    page.save();
    REQUIRE(wait_for(*state, [&] { return state->sync_label().starts_with(kCheckMark); }));

    const auto remote_head = [&] {
        git_repository* repo = nullptr;
        REQUIRE(git_repository_open(&repo, remote.string().c_str()) == 0);
        git_oid oid{};
        const std::string ref = "refs/heads/" + get("sync.branch");
        REQUIRE(git_reference_name_to_id(&oid, repo, ref.c_str()) == 0);
        git_repository_free(repo);
        return std::string(git_oid_tostr_s(&oid));
    };
    const std::string before = remote_head();

    state->edit_text("# son düzenleme\n");
    CHECK(state->request_close());
    CHECK(read_note("a.md") == "# son düzenleme\n");
    CHECK(remote_head() != before);
}

TEST_CASE_FIXTURE(SettingsFixture, "a worker post wakes the idle event loop through an SDL event") {
    auto state = make_state_wired();
    auto view = make_view(*state);
    for (int i = 0; i < 10; ++i)  // olaydan sonraki sabitlenme kareleri tükensin
        view->idle_step(0);
    const auto frames = view->frames();

    std::atomic<bool> ran_on_ui_thread = false;
    const auto ui_thread = std::this_thread::get_id();
    std::jthread worker(
        [&] { state->post([&] { ran_on_ui_thread = std::this_thread::get_id() == ui_thread; }); });
    worker.join();
    constexpr int kLongWaitMs = 10000;  // uyanmazsa bu kadar beklerdi; aşağıdaki süre sınırı bunu yakalar
    const auto started = std::chrono::steady_clock::now();
    view->idle_step(kLongWaitMs);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    CHECK(ran_on_ui_thread);
    CHECK(view->frames() > frames);
}

TEST_CASE_FIXTURE(SettingsFixture, "the icons setting is saved from the page and applied to the list") {
    auto state = make_state_wired();
    CHECK(state->icon_style() == noteapp::core::IconStyle::Nerd);  // pencerenin kendi fontu var
    state->settings().set_field("icons", "off");
    state->settings().save();
    CHECK(get("icons") == "off");
    CHECK(state->icon_style() == noteapp::core::IconStyle::Off);
    state->settings().set_field("icons", "nerd");
    state->settings().save();
    CHECK(get("icons") == "nerd");
    CHECK(state->icon_style() == noteapp::core::IconStyle::Nerd);
}
