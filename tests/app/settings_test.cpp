#include "app/settings.hpp"
#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "support/test_support.hpp"

#include <doctest/doctest.h>
#include <expected>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace noteapp;

namespace {

// Kayıt ve test akışının gözlenen her adımı: kasa yazımı ve enable çağrısı, sırasıyla.
struct FormWorld {
    test::TempDir dir;
    test::ScopedEnv home{"NOTEAPP_HOME", dir.path().u8string()};
    std::map<std::string, std::string> vault;
    std::vector<std::string> calls;
    std::optional<core::Error> enable_error;  // ilk enable bunu döner
    std::function<void()> during_enable;      // enable sürerken (ağ) başka bir süreç

    FormWorld() { core::init_i18n("en"); }

    app::SettingsServices services() {
        return {{[this](std::string_view host) { return vault.contains(std::string(host)); },
                 [this](std::string_view host, std::string_view token) -> std::expected<void, core::Error> {
                     calls.push_back("store " + std::string(host));
                     vault[std::string(host)] = std::string(token);
                     return {};
                 },
                 [this](std::string_view host) -> std::expected<void, core::Error> {
                     vault.erase(std::string(host));
                     return {};
                 }},
                [this](const core::Config& next, bool replace) -> std::expected<void, core::Error> {
                    calls.push_back("enable " + next.sync.remote + (replace ? " replace" : ""));
                    if (during_enable)
                        during_enable();
                    if (auto error = std::exchange(enable_error, std::nullopt))
                        return std::unexpected(std::move(*error));
                    return {};
                }};
    }
};

void turn_on_https_sync(app::SettingsForm& form) {
    form.field("sync.enabled").value = "true";
    form.field("sync.remote").value = "https://git.example.invalid/me/notes.git";
    form.remote_edited();
}

}  // namespace

TEST_CASE("SettingsForm save stores the typed token before enable and writes only the config") {
    FormWorld w;
    const core::Config applied = core::parse_config("").config;
    app::SettingsForm form(applied, w.services());
    turn_on_https_sync(form);
    REQUIRE(form.token_enabled());
    form.token = "ghp_secret_value";
    REQUIRE(form.changed());

    const auto saved = form.save(applied);
    REQUIRE_MESSAGE(saved, form.error());
    CHECK(saved->sync.enabled);
    CHECK(w.calls == std::vector<std::string>{"store git.example.invalid",
                                              "enable https://git.example.invalid/me/notes.git"});
    CHECK(w.vault.at("git.example.invalid") == "ghp_secret_value");
    CHECK(form.token.empty());
    CHECK(form.token_stored());
    CHECK_FALSE(form.changed());
    const std::string file = test::slurp(core::config_file());
    CHECK(file.find("enabled = true") != std::string::npos);
    CHECK(file.find("ghp_secret_value") == std::string::npos);
}

TEST_CASE("SettingsForm origin mismatch asks without credentials and saves nothing until confirmed") {
    FormWorld w;
    const core::Config applied = core::parse_config("").config;
    app::SettingsForm form(applied, w.services());
    turn_on_https_sync(form);
    w.enable_error = core::Error{core::Error::Code::Io,
                                 "sync.remote_mismatch",
                                 {{"current", "https://kisi:gizli@old.example.invalid/notes.git"},
                                  {"remote", "https://git.example.invalid/me/notes.git"}}};

    CHECK_FALSE(form.save(applied));
    REQUIRE(form.replace_request());
    CHECK(core::format_error(*form.replace_request()).find("gizli") == std::string::npos);
    CHECK(core::format_error(*form.replace_request()).find("old.example.invalid") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(core::config_file()));

    const auto saved = form.save(applied, true);
    REQUIRE_MESSAGE(saved, form.error());
    CHECK_FALSE(form.replace_request());
    CHECK(w.calls.back() == "enable https://git.example.invalid/me/notes.git replace");
}

TEST_CASE("SettingsForm keeps an enable failure on the form and writes nothing") {
    FormWorld w;
    const core::Config applied = core::parse_config("").config;
    app::SettingsForm form(applied, w.services());
    turn_on_https_sync(form);
    w.enable_error = core::Error{core::Error::Code::Io, "sync.keychain_unavailable", {}};

    CHECK_FALSE(form.save(applied));
    CHECK_FALSE(form.error().empty());
    CHECK_FALSE(form.replace_request());
    CHECK_FALSE(std::filesystem::exists(core::config_file()));
    CHECK(form.changed());
}

TEST_CASE("SettingsForm connection test runs on the form values and reports the outcome") {
    FormWorld w;
    const core::Config applied = core::parse_config("").config;
    app::SettingsForm form(applied, w.services());
    form.field("sync.branch").value = "notes";

    const auto candidate = form.begin_test(applied);
    REQUIRE(candidate);
    CHECK(candidate->sync.branch == "notes");
    CHECK(form.testing());
    CHECK(form.test_result() == core::tr("app.test_running"));
    CHECK_FALSE(form.begin_test(applied));  // süren test varken ikincisi başlamaz

    form.finish_test(std::unexpected(core::Error{core::Error::Code::Io, "sync.keychain_unavailable", {}}));
    CHECK_FALSE(form.testing());
    CHECK(form.test_result().find(core::tr("sync.keychain_unavailable")) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(core::config_file()));  // test dosyaya yazmaz
}

TEST_CASE("SettingsForm connection test does not start on a broken config file") {
    FormWorld w;
    std::filesystem::create_directories(core::config_file().parent_path());
    std::ofstream(core::config_file(), std::ios::binary) << "editor = = bozuk\n";
    const core::Config applied = core::parse_config("").config;
    app::SettingsForm form(applied, w.services());

    CHECK_FALSE(form.begin_test(applied));
    CHECK_FALSE(form.testing());
    CHECK(form.error().find(core::config_file().filename().string()) != std::string::npos);
    CHECK(form.error().find("nothing was saved") == std::string::npos);  // test kaydetmez
    CHECK(form.error().find("not tested") != std::string::npos);
}

// Kayıt dosyayı yazmadan hemen önce yeniden okur: enable (ağ) sürerken `note config set` ile yazılan ayar
// ezilmez.
TEST_CASE("SettingsForm save keeps a setting written to disk while sync was being set up") {
    FormWorld w;
    const core::Config applied = core::parse_config("").config;
    app::SettingsForm form(applied, w.services());
    turn_on_https_sync(form);
    w.during_enable = [] {
        core::Config other = core::load_config().config;
        REQUIRE(core::set_config_value(other, "editor", "emacs"));
        REQUIRE(core::save_config(other));
    };
    REQUIRE_MESSAGE(form.save(applied), form.error());
    const core::Config reloaded = core::load_config().config;
    CHECK(reloaded.editor == "emacs");
    CHECK(reloaded.sync.enabled);
}

TEST_CASE("config_warnings_message joins translated warnings with a semicolon, empty without any") {
    CHECK(app::config_warnings_message({}).empty());

    const core::Error unknown{core::Error::Code::ConfigInvalid, "config.unknown_key", {{"key", "bogus"}}};
    const core::Error syntax{core::Error::Code::ConfigInvalid,
                             "config.syntax_error",
                             {{"line", "3"}, {"column", "5"}, {"detail", "x"}}};
    CHECK(app::config_warnings_message({unknown}) == core::format_error(unknown));
    CHECK(app::config_warnings_message({unknown, syntax}) ==
          core::format_error(unknown) + "; " + core::format_error(syntax));
}

TEST_CASE("https_host names the keychain host only for HTTPS remotes") {
    CHECK(app::https_host("HTTPS://GitHub.COM/kisi/notlar") == std::optional<std::string>("github.com"));
    CHECK(app::https_host("https://git.example.com:8443/n.git") ==
          std::optional<std::string>("git.example.com"));
    CHECK_FALSE(app::https_host("git@github.com:kisi/notlar.git"));
    CHECK_FALSE(app::https_host("http://github.com/kisi/notlar.git"));
    CHECK_FALSE(app::https_host("/srv/git/notlar.git"));
    CHECK_FALSE(app::https_host(""));
}

TEST_CASE("config_broken_warning reports a syntax error but not an unknown key") {
    const core::LoadedConfig broken = core::parse_config("editor = [");
    const auto warning = app::config_broken_warning(broken);
    REQUIRE(warning);
    CHECK(warning->message_key == "config.syntax_error");

    core::LoadedConfig unreadable = core::parse_config("");
    unreadable.warnings.push_back({core::Error::Code::ConfigInvalid, "config.unreadable", {{"path", "x"}}});
    CHECK(app::config_broken_warning(unreadable)->message_key == "config.unreadable");

    CHECK_FALSE(app::config_broken_warning(core::parse_config("")));
    CHECK_FALSE(app::config_broken_warning(core::parse_config("bogus_key = 1")));
}

TEST_CASE("sync_restart_needed is true only for the fields AutoSync reads at start") {
    const core::Config base = core::parse_config("").config;
    const auto changed = [&](auto edit) {
        core::Config next = base;
        edit(next);
        return app::sync_restart_needed(base, next);
    };
    CHECK_FALSE(app::sync_restart_needed(base, base));
    CHECK_FALSE(changed([](core::Config& c) { c.editor = "vim"; }));
    CHECK_FALSE(changed([](core::Config& c) { c.sync.author_name = "Other"; }));
    CHECK(changed([](core::Config& c) { c.sync.enabled = !c.sync.enabled; }));
    CHECK(changed([](core::Config& c) { c.sync.remote = "https://example.invalid/r.git"; }));
    CHECK(changed([](core::Config& c) { c.sync.branch = "other"; }));
    CHECK(changed([](core::Config& c) { c.sync.idle_seconds += 1; }));
    CHECK(changed([](core::Config& c) { c.notes_dir /= "sub"; }));
}

// Uygulama açıkken `note config set limits.search.relation_depth` ya da `tuning.search_relations`: Kaydet
// çalışan config'e taşır, arayüz listeye uygular.
TEST_CASE("adopt_live_limits also carries the relation search settings from the file") {
    noteapp::core::Config live = noteapp::core::parse_config("").config;
    noteapp::core::Config disk = live;
    REQUIRE(noteapp::core::set_config_value(disk, "limits.search.relation_depth", "0"));
    disk.tuning.search_relations = false;
    noteapp::app::adopt_live_limits(live, disk);
    CHECK(live.limits.search.relation_depth.unlimited());
    CHECK_FALSE(live.tuning.search_relations);
}
