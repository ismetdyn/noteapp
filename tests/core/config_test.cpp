#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/json_text.hpp"
#include "core/paths.hpp"
#include "core/resources.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>  // _wputenv_s
#endif

using namespace noteapp::core;
namespace fs = std::filesystem;

namespace {

using namespace noteapp::test;

std::string arg_of(const Error& e, std::string_view name) {
    for (const auto& [k, v] : e.args)
        if (k == name)
            return v;
    return {};
}

// Uyarı, `message_key` ve `key` argümanıyla eşleşiyorsa true.
bool has_warning_about(const LoadedConfig& loaded, std::string_view needle,
                       std::string_view message_key = "") {
    return std::any_of(loaded.warnings.begin(), loaded.warnings.end(), [&](const Error& w) {
        return w.code == Error::Code::ConfigInvalid && arg_of(w, "key") == needle &&
               (message_key.empty() || w.message_key == message_key);
    });
}

}  // namespace

TEST_CASE("embedded_resource returns defaults.toml and rejects unknown keys") {
    const auto defaults = embedded_resource("defaults.toml");
    REQUIRE(defaults.has_value());
    CHECK(defaults->find("[tuning]") != std::string_view::npos);
    CHECK_FALSE(embedded_resource("no/such/file.toml").has_value());
}

TEST_CASE("parse_config with empty input yields the embedded defaults") {
    const ScopedEnv no_home("NOTEAPP_HOME", std::nullopt);  // ayarlıyken notes_dir onun altına gider
    const LoadedConfig loaded = parse_config("");
    CHECK(loaded.warnings.empty());
    const Config& c = loaded.config;
    CHECK(c.language == "auto");
    CHECK(c.notes_dir == expand_user_path("~/notes"));
    CHECK(c.editor.empty());
    CHECK(c.default_type == "md");
    CHECK(c.default_folder.empty());
    CHECK(c.types == std::vector<std::string>{"md",   "txt", "csv",  "cpp",        "java",       "py",
                                              "json", "sh",  "env",  "properties", "yaml",       "toml",
                                              "ini",  "xml", "html", "css",        "js",         "ts",
                                              "sql",  "c",   "go",   "rs",         "dockerfile", "makefile"});
    CHECK_FALSE(c.sync.enabled);
    CHECK(c.sync.branch == "main");
    CHECK(c.sync.idle_seconds == 30);
    CHECK(c.tuning.slug_max_length == 60);
    CHECK(c.tuning.min_confidence == doctest::Approx(0.5));
    CHECK(c.tuning.max_scan_bytes == 65536);
    CHECK(c.tuning.max_note_bytes == Limit{10485760, "10485760"});
    CHECK(c.tuning.rescan_seconds == 5);
    CHECK(c.tuning.autosave_ms == 3000);
    CHECK(c.tuning.exit_timeout_seconds == 10);
    CHECK(c.tuning.log_max_bytes == 1048576);
    CHECK(c.tuning.sync_stale_lock_minutes == 10);
    CHECK(c.tuning.sync_timeout_seconds == 120);
    CHECK(c.tuning.graph_batch_ops == 30000);
}

TEST_CASE("parse_config applies user overrides") {
    const LoadedConfig loaded = parse_config(R"(
language = "tr"
notes_dir = "/data/notlar"
editor = "code --wait"
default_type = "txt"
default_folder = "inbox"
types = ["md", "rs"]

[sync]
enabled = true
remote = "git@github.com:u/notes.git"
branch = "dev"
idle_seconds = 5
author_name = "İsmet"
author_email = "i@example.com"

[tuning]
slug_max_length = 40
min_confidence = 0.8
autosave_ms = 250
)");
    CHECK(loaded.warnings.empty());
    const Config& c = loaded.config;
    CHECK(c.language == "tr");
    CHECK(c.notes_dir == fs::path("/data/notlar"));
    CHECK(c.editor == "code --wait");
    CHECK(c.default_type == "txt");
    CHECK(c.default_folder == "inbox");
    CHECK(c.types == std::vector<std::string>{"md", "rs"});
    CHECK(c.sync.enabled);
    CHECK(c.sync.remote == "git@github.com:u/notes.git");
    CHECK(c.sync.branch == "dev");
    CHECK(c.sync.idle_seconds == 5);
    CHECK(c.sync.author_name == "İsmet");
    CHECK(c.tuning.slug_max_length == 40);
    CHECK(c.tuning.min_confidence == doctest::Approx(0.8));
    CHECK(c.tuning.autosave_ms == 250);
    CHECK(c.tuning.rescan_seconds == 5);  // dokunulmayan tuning varsayılan kalır
}

TEST_CASE("parse_config falls back to the default and warns on each invalid value") {
    const Config defaults = parse_config("").config;

    struct Case {
        const char* toml;
        const char* warns_about;
    };
    const Case cases[] = {
        {R"(language = "de")", "language"},
        {R"(language = 3)", "language"},
        {R"(notes_dir = "")", "notes_dir"},
        {R"(notes_dir = 7)", "notes_dir"},
        {R"(default_type = "")", "default_type"},
        {R"(default_type = "tar.gz")", "default_type"},
        {R"(types = ["md", ""])", "types"},
        {R"(types = ["md", "a.b"])", "types"},
        {R"(types = "md")", "types"},
        {R"(types = ["md", 1])", "types"},
        {"[sync]\nidle_seconds = 0", "sync.idle_seconds"},
        {"[sync]\nidle_seconds = -5", "sync.idle_seconds"},
        {"[sync]\nidle_seconds = \"x\"", "sync.idle_seconds"},
        {"[sync]\nbranch = \"\"", "sync.branch"},
        {"[sync]\nenabled = \"yes\"", "sync.enabled"},
        {"sync = 5", "sync"},
        {"[tuning]\nslug_max_length = 0", "tuning.slug_max_length"},
        {"[tuning]\nmin_confidence = 1.5", "tuning.min_confidence"},
        {"[tuning]\nautosave_ms = -1", "tuning.autosave_ms"},
        {"[tuning]\nsync_stale_lock_minutes = 0", "tuning.sync_stale_lock_minutes"},
        {"[tuning]\nsync_timeout_seconds = 0", "tuning.sync_timeout_seconds"},
        {"[tuning]\ngraph_batch_ops = 0", "tuning.graph_batch_ops"},
        {"[tuning]\ntui_list_share = 1.5", "tuning.tui_list_share"},
    };
    for (const Case& tc : cases) {
        CAPTURE(tc.toml);
        const LoadedConfig loaded = parse_config(tc.toml);
        CHECK(loaded.config == defaults);
        CHECK(has_warning_about(loaded, tc.warns_about));
    }
}

TEST_CASE("one invalid value does not discard the other valid overrides") {
    const LoadedConfig loaded = parse_config("language = \"xx\"\neditor = \"vim\"\n");
    CHECK(loaded.config.language == "auto");
    CHECK(loaded.config.editor == "vim");
    CHECK(loaded.warnings.size() == 1);
}

TEST_CASE("parse_config reports unknown keys and ignores them") {
    const Config defaults = parse_config("").config;
    const LoadedConfig loaded =
        parse_config("colour = \"red\"\n[sync]\nfoo = 1\n[tuning]\nbar = 2\n[extra]\nx = 1\n");
    CHECK(loaded.config == defaults);
    CHECK(has_warning_about(loaded, "colour", "config.unknown_key"));
    CHECK(has_warning_about(loaded, "sync.foo", "config.unknown_key"));
    CHECK(has_warning_about(loaded, "tuning.bar", "config.unknown_key"));
    CHECK(has_warning_about(loaded, "extra", "config.unknown_key"));
}

TEST_CASE("parse_config survives a TOML syntax error with defaults and a positioned warning") {
    const Config defaults = parse_config("").config;
    const LoadedConfig loaded = parse_config("language = \"tr\"\neditor = = broken\n");
    CHECK(loaded.config == defaults);  // kısmi uygulama yok: dosya bütün olarak reddedilir
    REQUIRE(loaded.warnings.size() == 1);
    CHECK(loaded.warnings[0].code == Error::Code::ConfigInvalid);
    CHECK(loaded.warnings[0].message_key == "config.syntax_error");
    CHECK(arg_of(loaded.warnings[0], "line") == "2");
}

TEST_CASE("parse_config expands ~ and environment variables in notes_dir") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_TEST_DIR", tmp.path().u8string());
    CHECK(parse_config(R"(notes_dir = "~/n")").config.notes_dir == home_dir() / "n");
    CHECK(parse_config(R"(notes_dir = "$NOTEAPP_TEST_DIR/n")").config.notes_dir == tmp.path() / "n");
}

TEST_CASE("NOTEAPP_HOME overrides config, state and data dirs, including non-ASCII paths") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    CHECK(config_dir() == tmp.path() / "config");
    CHECK(state_dir() == tmp.path() / "state");
    CHECK(data_dir() == tmp.path() / "data");
    CHECK(config_file() == tmp.path() / "config" / "config.toml");
}

TEST_CASE("NOTEAPP_HOME moves the default notes_dir under it; an explicit value stays") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    CHECK(parse_config("").config.notes_dir == tmp.path() / "notes");
    CHECK(load_config().config.notes_dir == tmp.path() / "notes");
    CHECK(parse_config(R"(notes_dir = "/data/notlar")").config.notes_dir == fs::path("/data/notlar"));
    CHECK(parse_config(R"(notes_dir = "~/n")").config.notes_dir == home_dir() / "n");
}

TEST_CASE("platform default dirs follow SPEC section 8") {
    const ScopedEnv no_home("NOTEAPP_HOME", std::nullopt);
#ifdef _WIN32
    const ScopedEnv roaming("APPDATA", u8"C:\\Users\\İşıl\\AppData\\Roaming");
    const ScopedEnv local("LOCALAPPDATA", u8"C:\\Users\\İşıl\\AppData\\Local");
    CHECK(config_dir() == fs::path(u8"C:\\Users\\İşıl\\AppData\\Roaming") / "noteapp");
    CHECK(state_dir() == fs::path(u8"C:\\Users\\İşıl\\AppData\\Local") / "noteapp");
    CHECK(data_dir() == fs::path(u8"C:\\Users\\İşıl\\AppData\\Local") / "noteapp");
#elif defined(__APPLE__)
    CHECK(config_dir() == home_dir() / "Library" / "Application Support" / "noteapp");
    CHECK(state_dir() == config_dir());
    CHECK(data_dir() == config_dir());
#else
    {
        const ScopedEnv xdg_config("XDG_CONFIG_HOME", u8"/x/config");
        const ScopedEnv xdg_state("XDG_STATE_HOME", u8"/x/state");
        CHECK(config_dir() == fs::path("/x/config/noteapp"));
        CHECK(state_dir() == fs::path("/x/state/noteapp"));
        const ScopedEnv xdg_data("XDG_DATA_HOME", u8"/x/data");
        CHECK(data_dir() == fs::path("/x/data/noteapp"));
    }
    const ScopedEnv no_config("XDG_CONFIG_HOME", std::nullopt);
    const ScopedEnv no_state("XDG_STATE_HOME", std::nullopt);
    CHECK(config_dir() == home_dir() / ".config" / "noteapp");
    CHECK(state_dir() == home_dir() / ".local" / "state" / "noteapp");
    const ScopedEnv no_data("XDG_DATA_HOME", std::nullopt);
    CHECK(data_dir() == home_dir() / ".local" / "share" / "noteapp");
#endif
}

TEST_CASE("expand_user_path handles ~, $VAR, ${VAR} and leaves unknowns alone") {
    const ScopedEnv var("NOTEAPP_TEST_VAR", u8"ğüşiö");
    const ScopedEnv unset("NOTEAPP_TEST_UNSET", std::nullopt);

    CHECK(expand_user_path("~") == home_dir());
    CHECK(expand_user_path("~/notes") == home_dir() / "notes");
    CHECK(expand_user_path("~user/notes") == fs::path("~user/notes"));  // başka kullanıcı: desteklenmez
    CHECK(expand_user_path("a/~/b") == fs::path("a/~/b"));              // ~ yalnız başta
    CHECK(expand_user_path("/p/$NOTEAPP_TEST_VAR/n") == fs::path(u8"/p/ğüşiö/n"));
    CHECK(expand_user_path("/p/${NOTEAPP_TEST_VAR}x") == fs::path(u8"/p/ğüşiöx"));
    CHECK(expand_user_path("/p/$NOTEAPP_TEST_UNSET/n") == fs::path("/p/$NOTEAPP_TEST_UNSET/n"));
    CHECK(expand_user_path("/p/${NOTEAPP_TEST_UNSET}") == fs::path("/p/${NOTEAPP_TEST_UNSET}"));
    CHECK(expand_user_path("/cost/5$") == fs::path("/cost/5$"));
    CHECK(expand_user_path("") == fs::path());
#ifdef _WIN32
    CHECK(expand_user_path("C:/p/%NOTEAPP_TEST_VAR%/n") == fs::path(u8"C:/p/ğüşiö/n"));
    CHECK(expand_user_path("C:/p/%NOTEAPP_TEST_UNSET%") == fs::path("C:/p/%NOTEAPP_TEST_UNSET%"));
    CHECK(expand_user_path("~\\notes") == home_dir() / "notes");
#endif
}

TEST_CASE("save_config then load_config round-trips under NOTEAPP_HOME") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());

    // Dosya yokken: varsayılanlar, uyarısız.
    const LoadedConfig fresh = load_config();
    CHECK(fresh.warnings.empty());
    CHECK(fresh.config == parse_config("").config);

    Config c = fresh.config;
    c.language = "en";
    c.notes_dir = tmp.path() / fs::path(u8"notlarım");
    c.editor = "code --wait";
    c.default_type = "py";
    c.default_folder = "gelen kutusu";
    c.types = {"md", "py", "rs"};
    c.sync.enabled = true;
    c.sync.remote = "https://github.com/u/notes.git";
    c.sync.branch = "trunk";
    c.sync.idle_seconds = 12;
    c.sync.author_name = "İsmet \"Dayan\"";  // kaçış gerektiren karakter
    c.sync.author_email = "i@example.com";

    REQUIRE(save_config(c).has_value());  // dizin yokken de oluşturur
    CHECK(fs::exists(config_file()));

    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config == c);
}

TEST_CASE("save_config keeps the user's [tuning] overrides and leaves defaults implicit") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    std::ofstream(config_file(), std::ios::binary) << "[tuning]\nsync_timeout_seconds = 300\n";

    // Uygulamanın kendi kaydı (ör. `note config set editor`) elle yazılmış ince ayarı silmemeli.
    Config c = load_config().config;
    REQUIRE(c.tuning.sync_timeout_seconds == 300);
    REQUIRE(set_config_value(c, "editor", "vim"));
    REQUIRE(save_config(c).has_value());

    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.tuning.sync_timeout_seconds == 300);
    CHECK(reloaded.config.editor == "vim");
    const std::string text = slurp(config_file());
    CHECK(text.find("sync_timeout_seconds = 300") != std::string::npos);
    CHECK(text.find("slug_max_length") == std::string::npos);  // dokunulmamış varsayılan yazılmaz
}

TEST_CASE("save_config round-trips every [tuning] key that differs from the defaults") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());

    // Yeni bir tuning anahtarı kaydetme listesine eklenmeyi unutursa burada düşer.
    Config c = parse_config("").config;
    c.tuning.slug_max_length += 1;
    c.tuning.min_confidence = 0.123456789;
    c.tuning.max_scan_bytes += 1;
    c.tuning.max_note_bytes = Limit{10485761, "10485761"};
    c.tuning.rescan_seconds += 1;
    c.tuning.autosave_ms += 1;
    c.tuning.exit_timeout_seconds += 1;
    c.tuning.log_max_bytes += 1;
    c.tuning.sync_stale_lock_minutes += 1;
    c.tuning.sync_timeout_seconds += 1;
    c.tuning.completion_min_prefix_bytes += 1;
    c.tuning.tui_list_share = 0.25;
    REQUIRE(save_config(c).has_value());

    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.tuning == c.tuning);
}

TEST_CASE("load_config with a corrupt file returns defaults and a warning instead of failing") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    std::ofstream(config_file(), std::ios::binary) << "this is [not toml";

    const LoadedConfig loaded = load_config();
    CHECK(loaded.config == parse_config("").config);
    CHECK_FALSE(loaded.warnings.empty());
}

TEST_CASE("set_config_value applies typed values with the same rules as parse_config") {
    Config c = parse_config("").config;

    CHECK(set_config_value(c, "language", "en"));
    CHECK(c.language == "en");
    CHECK(set_config_value(c, "sync.enabled", "true"));
    CHECK(c.sync.enabled);
    CHECK(set_config_value(c, "sync.idle_seconds", "45"));
    CHECK(c.sync.idle_seconds == 45);
    CHECK(set_config_value(c, "sync.remote", "https://example.com/n.git"));
    CHECK(c.sync.remote == "https://example.com/n.git");
    CHECK(set_config_value(c, "editor", ""));  // boş editör geçerli: platform varsayılanı
    CHECK(set_config_value(c, "types", "md, txt ,py"));
    CHECK(c.types == std::vector<std::string>{"md", "txt", "py"});
    CHECK(set_config_value(c, "notes_dir", "~/elsewhere"));
    CHECK(c.notes_dir == expand_user_path("~/elsewhere"));
}

TEST_CASE("set_config_value rejects bad values and unknown keys without touching the config") {
    const Config original = parse_config("").config;
    Config c = original;

    for (const auto& [key, value] :
         std::vector<std::pair<std::string, std::string>>{{"language", "de"},
                                                          {"sync.enabled", "yes"},
                                                          {"sync.idle_seconds", "0"},
                                                          {"sync.idle_seconds", "abc"},
                                                          {"sync.idle_seconds", "3000000000"},
                                                          {"sync.branch", ""},
                                                          {"default_type", "a.b"},
                                                          {"types", "md,,txt"},
                                                          {"types", "md,a.b"},
                                                          {"notes_dir", ""}}) {
        CAPTURE(key);
        CAPTURE(value);
        const auto result = set_config_value(c, key, value);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == Error::Code::ConfigInvalid);
        CHECK(c == original);
    }

    const auto unknown = set_config_value(c, "nope", "1");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().message_key == "config.unknown_key");
    CHECK_FALSE(set_config_value(c, "tuning.autosave_ms", "5"));  // ayar arayüzünde görünmez
    CHECK_FALSE(set_config_value(c, "sync.token", "secret"));
    CHECK(c == original);
}

// https://kullanıcı:token@… token'ı düz metin config.toml'a yazardı; token kasada durur (note sync login).
TEST_CASE("sync.remote with credentials in an http(s) URL is refused and the secret is never echoed") {
    const Config original = parse_config("").config;
    Config c = original;
    for (const std::string bad :
         {"https://user:s3cret@github.com/u/notes.git", "http://s3cret@host/notes.git",
          "HTTPS://u:s3cret@host", "https://s3cret@host?x=1"}) {
        CAPTURE(bad);
        const auto set = set_config_value(c, "sync.remote", bad);
        REQUIRE_FALSE(set);
        CHECK(set.error().code == Error::Code::ConfigInvalid);
        CHECK(arg_of(set.error(), "reason") == "remote_has_credentials");
        for (const auto& [key, value] : set.error().args)
            CHECK(value.find("s3cret") == std::string::npos);
        CHECK(c == original);
    }

    // SSH kullanıcı adı sır değildir; yoldaki '@' kullanıcı bilgisi değildir.
    for (const std::string ok : {"git@github.com:u/notes.git", "ssh://git@host/notes.git",
                                 "https://github.com/u/n.git", "https://host/path/with@sign.git"}) {
        CAPTURE(ok);
        CHECK(set_config_value(c, "sync.remote", ok));
        CHECK(c.sync.remote == ok);
    }
}

// Elle yazılmış remote kaybolmasın: URL'deki kimlik zaten hiç kullanılmaz (token ortamdan ya da kasadan
// gelir).
TEST_CASE(
    "loading a sync.remote with credentials strips them, keeps the remote and warns without the secret") {
    for (const auto& [raw, clean] : std::vector<std::pair<std::string, std::string>>{
             {"https://u:s3cret@host/r.git", "https://host/r.git"},
             {"http://s3cret@host/r.git", "http://host/r.git"},
             {"HTTPS://u:s3cret@host", "HTTPS://host"},
             {"https://u:s3cret@x@host?y=1", "https://host?y=1"}}) {
        CAPTURE(raw);
        const LoadedConfig loaded = parse_config("[sync]\nremote = \"" + raw + "\"\n");
        CHECK(loaded.config.sync.remote == clean);
        REQUIRE(has_warning_about(loaded, "sync.remote", "config.remote_credentials_stripped"));
        for (const Error& warning : loaded.warnings) {
            CHECK(format_error(warning).find("s3cret") == std::string::npos);
            for (const auto& [key, value] : warning.args)
                CHECK(value.find("s3cret") == std::string::npos);
        }
    }

    const LoadedConfig plain = parse_config("[sync]\nremote = \"git@github.com:u/notes.git\"\n");
    CHECK(plain.config.sync.remote == "git@github.com:u/notes.git");
    CHECK(plain.warnings.empty());
}

TEST_CASE("the next save_config leaves no credentials from sync.remote in the file") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    std::ofstream(config_file(), std::ios::binary) << "[sync]\nremote = \"https://u:s3cret@host/r.git\"\n";

    REQUIRE(save_config(load_config().config).has_value());

    CHECK(slurp(config_file()).find("s3cret") == std::string::npos);
    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.config.sync.remote == "https://host/r.git");
    CHECK(reloaded.warnings.empty());
}

TEST_CASE("config_entries lists the user-facing keys with dotted sync keys and joined lists") {
    const auto entries = config_entries(parse_config("").config);
    const std::map<std::string, std::string> by_key(entries.begin(), entries.end());

    CHECK(by_key.at("language") == "auto");
    CHECK(by_key.at("types") == "md, txt, csv, cpp, java, py, json, sh, env, properties, yaml, toml, ini, "
                                "xml, html, css, js, ts, sql, c, go, rs, dockerfile, makefile");
    CHECK(by_key.at("sync.enabled") == "false");
    CHECK(by_key.at("sync.idle_seconds") == "30");
    CHECK(by_key.count("tuning.autosave_ms") == 0);
    CHECK(entries.front().first == "language");
}

TEST_CASE("icons defaults to nerd, reads the old auto as nerd, accepts off, warns otherwise") {
    CHECK(parse_config("").config.icons == "nerd");
    CHECK(parse_config("icons = \"auto\"").config.icons == "nerd");
    CHECK(parse_config("icons = \"auto\"").warnings.empty());
    CHECK(parse_config("icons = \"nerd\"").config.icons == "nerd");
    CHECK(parse_config("icons = \"off\"").config.icons == "off");
    const LoadedConfig bad = parse_config("icons = \"emoji\"");
    CHECK(bad.config.icons == "nerd");
    REQUIRE(bad.warnings.size() == 1);
    CHECK(format_error(bad.warnings[0]).find("icons") != std::string::npos);

    Config config = parse_config("").config;
    CHECK_FALSE(set_config_value(config, "icons", "emoji").has_value());
    CHECK(config.icons == "nerd");
    REQUIRE(set_config_value(config, "icons", "off").has_value());
    REQUIRE(set_config_value(config, "icons", "auto").has_value());  // eski ad kabul edilir
    CHECK(config.icons == "nerd");
    const auto entries = config_entries(config);
    const std::map<std::string, std::string> by_key(entries.begin(), entries.end());
    CHECK(by_key.at("icons") == "nerd");
}

TEST_CASE("save_config keeps the icons setting") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    Config config = parse_config("").config;
    config.icons = "off";
    REQUIRE(save_config(config).has_value());
    CHECK(load_config().config.icons == "off");
}

namespace {

using Argv = std::vector<std::string>;
using ServerMap = std::map<std::string, std::vector<Argv>>;

// Varsayılan sunucular: proje kodu çalıştırmayanlar ve T59e'de sertleştirilenler (README "Dil sunucusu").
const ServerMap& default_servers() {
    static const ServerMap servers = {
        {"c", {Argv{"clangd"}}},
        {"cpp", {Argv{"clangd"}}},
        {"css", {Argv{"vscode-css-language-server", "--stdio"}}},
        {"dockerfile", {Argv{"docker-langserver", "--stdio"}}},
        {"go", {Argv{"gopls"}}},
        {"html", {Argv{"vscode-html-language-server", "--stdio"}, Argv{"htmx-lsp"}}},
        {"java", {Argv{"jdtls"}}},
        {"javascript", {Argv{"typescript-language-server", "--stdio"}}},
        {"json", {Argv{"vscode-json-language-server", "--stdio"}}},
        {"less", {Argv{"vscode-css-language-server", "--stdio"}}},
        {"lua", {Argv{"lua-language-server"}}},
        {"markdown", {Argv{"marksman", "server"}}},
        {"python", {Argv{"pyright-langserver", "--stdio"}}},
        {"rust", {Argv{"rust-analyzer"}}},
        {"scss", {Argv{"vscode-css-language-server", "--stdio"}}},
        {"shellscript", {Argv{"bash-language-server", "start"}}},
        {"sql", {Argv{"sql-language-server", "up", "--method", "stdio"}}},
        {"toml", {Argv{"taplo", "lsp", "--no-auto-config", "stdio"}}},
        {"typescript", {Argv{"typescript-language-server", "--stdio"}}},
        {"yaml", {Argv{"yaml-language-server", "--stdio"}}},
    };
    return servers;
}

Json parsed_json(std::string_view text) {
    auto json = JsonParser(text).parse();
    REQUIRE(json);
    return *json;
}

}  // namespace

TEST_CASE("lsp is on by default with safe servers and the T59e servers that are hardened") {
    const LoadedConfig loaded = parse_config("");
    CHECK(loaded.warnings.empty());
    CHECK(loaded.config.lsp.enabled);
    CHECK(loaded.config.lsp.servers == default_servers());
    CHECK(loaded.config.lsp.settings.empty());  // sertleştirme gömülü tablodadır, defaults.toml'da değil
}

TEST_CASE("lsp servers from the user file are merged over the defaults") {
    const LoadedConfig loaded = parse_config(R"(
[lsp.servers]
python = ["pylsp"]
kotlin = ["kotlin-language-server"]
)");
    CHECK(loaded.warnings.empty());
    ServerMap expected = default_servers();
    expected["python"] = {Argv{"pylsp"}};
    expected["kotlin"] = {Argv{"kotlin-language-server"}};
    CHECK(loaded.config.lsp.servers == expected);
}

TEST_CASE("an lsp server value may list several argv lists for one language") {
    const LoadedConfig loaded = parse_config(R"(
[lsp.servers]
html = [["vscode-html-language-server", "--stdio"], ["htmx-lsp", "--level", "WARN"]]
xml = [["lemminx"]]
)");
    CHECK(loaded.warnings.empty());
    CHECK(loaded.config.lsp.servers.at("html") ==
          std::vector<Argv>{{"vscode-html-language-server", "--stdio"}, {"htmx-lsp", "--level", "WARN"}});
    CHECK(loaded.config.lsp.servers.at("xml") == std::vector<Argv>{{"lemminx"}});
}

TEST_CASE("invalid multi-server lists warn and are ignored") {
    std::string many = "[";
    for (std::size_t i = 0; i <= LspConfig::kMaxServersPerLanguage; ++i)
        many += "[\"a\"],";
    many += "]";
    const LoadedConfig loaded = parse_config("[lsp.servers]\n"
                                             "mixed = [[\"a\"], \"b\"]\n"
                                             "empty_inner = [[\"a\"], []]\n"
                                             "bad_inner = [[\"a\", 3]]\n"
                                             "many = " +
                                             many + "\n");
    CHECK(loaded.config.lsp.servers == default_servers());
    for (const char* key :
         {"lsp.servers.mixed", "lsp.servers.empty_inner", "lsp.servers.bad_inner", "lsp.servers.many"})
        CHECK_MESSAGE(has_warning_about(loaded, key, "config.invalid_value"), key);
}

TEST_CASE("lsp settings tables become JSON per language") {
    const LoadedConfig loaded = parse_config(R"(
[lsp.settings.rust]
cargo.buildScripts.enable = true
check = { command = "clippy" }
"rust-analyzer".procMacro.enable = true
[lsp.settings.yaml]
yaml.schemas = { "https://example.com/s.json" = ["*.k8s.yaml"] }
yaml.customTags = ["!Ref", "!Sub scalar"]
yaml.maxItemsComputed = 100
http.ratio = 0.5
)");
    CHECK(loaded.warnings.empty());
    REQUIRE(loaded.config.lsp.settings.size() == 2);
    CHECK(loaded.config.lsp.settings.at("rust") ==
          parsed_json(R"({"cargo":{"buildScripts":{"enable":true}},"check":{"command":"clippy"},)"
                      R"("rust-analyzer":{"procMacro":{"enable":true}}})"));
    CHECK(json_text(loaded.config.lsp.settings.at("yaml")) ==
          R"({"http":{"ratio":0.5},"yaml":{"customTags":["!Ref","!Sub scalar"],"maxItemsComputed":100,)"
          R"("schemas":{"https://example.com/s.json":["*.k8s.yaml"]}}})");
}

TEST_CASE("invalid lsp settings warn and are ignored, valid languages stay") {
    std::string key = "k";
    for (int i = 0; i <= LspConfig::kMaxSettingsDepth; ++i)
        key += ".k";
    const LoadedConfig loaded = parse_config("[lsp.settings]\n"
                                             "go = { ok = true }\n"
                                             "flat = 3\n"
                                             "\"Bad Id\" = { a = 1 }\n"
                                             "nan = { x = nan }\n"
                                             "date = { x = 1979-05-27 }\n"
                                             "[lsp.settings.deep]\n" +
                                             key + " = 1\n");
    REQUIRE(loaded.config.lsp.settings.size() == 1);
    CHECK(loaded.config.lsp.settings.at("go") == parsed_json(R"({"ok":true})"));
    for (const char* entry : {"lsp.settings.flat", "lsp.settings.Bad Id", "lsp.settings.nan",
                              "lsp.settings.date", "lsp.settings.deep"})
        CHECK_MESSAGE(has_warning_about(loaded, entry, "config.invalid_value"), entry);
}

TEST_CASE("an empty argv turns one default server off; enabled = false turns all off") {
    const LoadedConfig one = parse_config("[lsp.servers]\nmarkdown = []\n");
    CHECK(one.warnings.empty());
    CHECK_FALSE(one.config.lsp.servers.contains("markdown"));
    CHECK(one.config.lsp.servers.size() == default_servers().size() - 1);
    CHECK_FALSE(parse_config("[lsp]\nenabled = false\n").config.lsp.enabled);
}

TEST_CASE("invalid lsp server entries warn and are ignored, valid ones stay") {
    const std::string too_long(LspConfig::kMaxArgumentBytes + 1, 'x');
    std::string many = "[";
    for (std::size_t i = 0; i <= LspConfig::kMaxArguments; ++i)
        many += "\"a\",";
    many += "]";
    const LoadedConfig loaded =
        parse_config("[lsp]\nenabled = true\n[lsp.servers]\n"
                     "python = [\"pylsp\"]\n"
                     "number = [\"clangd\", 3]\n"
                     "text = \"clangd\"\n"
                     "blank = [\"\"]\n"
                     "nul = [\"a\\u0000b\"]\n"
                     "long = [\"" +
                     too_long + "\"]\n" + "many = " + many + "\n" + "\"Bad Id\" = [\"x\"]\n");
    ServerMap expected = default_servers();
    expected["python"] = {Argv{"pylsp"}};
    CHECK(loaded.config.lsp.servers == expected);
    for (const char* key : {"lsp.servers.number", "lsp.servers.text", "lsp.servers.blank", "lsp.servers.nul",
                            "lsp.servers.long", "lsp.servers.many", "lsp.servers.Bad Id"})
        CHECK_MESSAGE(has_warning_about(loaded, key, "config.invalid_value"), key);
    CHECK(loaded.warnings.size() == 7);
}

TEST_CASE("more lsp servers than the limit are cut with a warning") {
    std::string text = "[lsp.servers]\n";
    for (std::size_t i = 0; i <= LspConfig::kMaxServers; ++i)
        text += "l" + std::to_string(100 + i) + " = [\"x\"]\n";
    const LoadedConfig loaded = parse_config(text);
    CHECK(loaded.config.lsp.servers.size() == default_servers().size() + LspConfig::kMaxServers);
    CHECK(loaded.warnings.size() == 1);
}

TEST_CASE("lsp section rejects a non-table servers value and unknown keys") {
    const LoadedConfig loaded = parse_config("[lsp]\nenabled = \"yes\"\nservers = 3\nport = 1\n");
    CHECK(loaded.config.lsp == parse_config("").config.lsp);  // bozuk değer varsayılana düşer
    CHECK(has_warning_about(loaded, "lsp.enabled", "config.invalid_value"));
    CHECK(has_warning_about(loaded, "lsp.servers", "config.invalid_value"));
    CHECK(has_warning_about(loaded, "lsp.port", "config.unknown_key"));
}

TEST_CASE("save_config keeps the lsp section of the file so a settings save does not drop the servers") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    std::ofstream(config_file(), std::ios::binary)
        << "[lsp]\nenabled = true\n[lsp.servers]\npython = [\"pyright-langserver\", \"--stdio\"]\n"
           "c = [\"C:\\\\tools\\\\clangd \\\"x\\\".exe\"]\n";
    const LoadedConfig loaded = load_config();
    REQUIRE(loaded.warnings.empty());
    Config c = loaded.config;
    c.language = "en";  // ayar ekranından başka bir anahtar değişir
    REQUIRE(save_config(c).has_value());
    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.lsp == loaded.config.lsp);
    CHECK(reloaded.config.lsp.servers.at("c") == std::vector<Argv>{{"C:\\tools\\clangd \"x\".exe"}});
}

TEST_CASE("save_config writes the lsp section as it is in the file now, not the startup snapshot") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    std::ofstream(config_file(), std::ios::binary)
        << "[lsp]\nenabled = true\n[lsp.servers]\npython = [\"pyright-langserver\", \"--stdio\"]\n";
    Config snapshot = load_config().config;  // TUI açılışta okudu
    REQUIRE(snapshot.lsp.enabled);

    // Uygulama açıkken kullanıcı LSP'yi dosyadan kapatır ve bir girdiyi bozar.
    std::ofstream(config_file(), std::ios::binary | std::ios::trunc)
        << "[lsp]\nenabled = false\n[lsp.servers]\npython = [\"pylsp\"]\nbroken = 3\n";
    snapshot.language = "en";
    REQUIRE(save_config(snapshot).has_value());

    const LoadedConfig reloaded = load_config();
    CHECK_FALSE(reloaded.config.lsp.enabled);
    CHECK(reloaded.config.lsp.servers.at("python") == std::vector<Argv>{{"pylsp"}});
    CHECK(has_warning_about(reloaded, "lsp.servers.broken", "config.invalid_value"));  // elle yazılan korunur
    CHECK(reloaded.config.language == "en");
}

TEST_CASE("save_config keeps an lsp opt-out written as inline table, dotted key or header") {
    for (const std::string form :
         {"lsp = { enabled = false }\n", "lsp.enabled = false\n", "[lsp]\nenabled = false\n"}) {
        CAPTURE(form);
        const TempDir tmp(kTurkishUncreated);
        const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
        fs::create_directories(config_dir());
        std::ofstream(config_file(), std::ios::binary) << form;
        const LoadedConfig loaded = load_config();
        REQUIRE(loaded.warnings.empty());
        REQUIRE_FALSE(loaded.config.lsp.enabled);
        REQUIRE(save_config(loaded.config).has_value());
        const LoadedConfig reloaded = load_config();
        CHECK(reloaded.warnings.empty());
        CHECK_FALSE(reloaded.config.lsp.enabled);
        CHECK(reloaded.config.sync == loaded.config.sync);
    }
}

TEST_CASE("save_config drops an lsp value that is not a table") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    std::ofstream(config_file(), std::ios::binary) << "lsp = 3\n";
    REQUIRE(save_config(load_config().config).has_value());
    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.lsp == parse_config("").config.lsp);
}

TEST_CASE("save_config writes no lsp section when the file has none") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    Config c = parse_config("").config;
    c.lsp.enabled = !c.lsp.enabled;  // bellekteki değer dosyaya taşınmaz: uygulama bu bölümü düzenlemez
    REQUIRE(save_config(c).has_value());
    std::ifstream in(config_file(), std::ios::binary);
    const std::string text(std::istreambuf_iterator<char>(in), {});
    CHECK(text.find("[lsp") == std::string::npos);
    CHECK(load_config().config.lsp == parse_config("").config.lsp);
}

// --- [limits] sözleşmesi (L0): 0 = sınırsız, boyut/süre birimleri, tek uyarı yolu, gidiş-dönüş ---

TEST_CASE("parse_limit reads plain integers, size and duration units and rejects garbage") {
    constexpr std::uint64_t kKiB = 1024;
    CHECK(parse_limit("0", LimitUnit::Count) == 0U);
    CHECK(parse_limit("1000", LimitUnit::Count) == 1000U);
    CHECK(parse_limit("512KiB", LimitUnit::Bytes) == 512 * kKiB);
    CHECK(parse_limit("64MiB", LimitUnit::Bytes) == 64 * kKiB * kKiB);
    CHECK(parse_limit("10MB", LimitUnit::Bytes) == 10'000'000U);
    CHECK(parse_limit("2GiB", LimitUnit::Bytes) == 2 * kKiB * kKiB * kKiB);
    CHECK(parse_limit("7B", LimitUnit::Bytes) == 7U);
    CHECK(parse_limit("4096", LimitUnit::Bytes) == 4096U);
    CHECK(parse_limit("250ms", LimitUnit::Milliseconds) == 250U);
    CHECK(parse_limit("5s", LimitUnit::Milliseconds) == 5000U);
    CHECK(parse_limit("2min", LimitUnit::Milliseconds) == 120'000U);
    CHECK(parse_limit("1h", LimitUnit::Milliseconds) == 3'600'000U);
    CHECK(parse_limit("0s", LimitUnit::Milliseconds) == 0U);  // birimli sıfır da sınırsızdır

    for (const char* bad : {"", "-1", "+5", " 5", "5 ", "abc", "5MiBx", "MiB", "1.5MiB", "10mb", "5s",
                            "99999999999999999999", "9223372036854775808"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_limit(bad, LimitUnit::Count).has_value());
    }
    CHECK_FALSE(parse_limit("5s", LimitUnit::Bytes).has_value());
    CHECK_FALSE(parse_limit("5MiB", LimitUnit::Milliseconds).has_value());
    CHECK_FALSE(parse_limit("9000000TiB", LimitUnit::Bytes).has_value());  // int64 taşması
}

TEST_CASE("Limit treats 0 as unlimited in allows and cap") {
    const Limit unlimited{0, "0"};
    CHECK(unlimited.unlimited());
    CHECK(unlimited.allows(UINT64_MAX));
    CHECK(unlimited.cap() == SIZE_MAX);
    const Limit five{5, "5"};
    CHECK_FALSE(five.unlimited());
    CHECK(five.allows(5));
    CHECK_FALSE(five.allows(6));
    CHECK(five.cap() == 5U);
}

TEST_CASE("limits.search.content_results defaults to 1000 and accepts 0 as unlimited") {
    CHECK(parse_config("").config.limits.search.content_results == Limit{1000, "1000"});

    const LoadedConfig zero = parse_config("[limits.search]\ncontent_results = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.limits.search.content_results.unlimited());
    const LoadedConfig quoted = parse_config("limits.search.content_results = \"25\"\n");
    CHECK(quoted.warnings.empty());
    CHECK(quoted.config.limits.search.content_results == Limit{25, "25"});
}

// İlişki araması: komşu adımı sınırdır (0 sınırsız), kapatmak ayrı bir tuning anahtarıdır.
TEST_CASE("relation search defaults to one hop, 0 is unlimited and search_relations turns it off") {
    const Config defaults = parse_config("").config;
    CHECK(defaults.limits.search.relation_depth == Limit{1, "1"});
    CHECK(defaults.tuning.search_relations);

    const LoadedConfig changed =
        parse_config("[limits.search]\nrelation_depth = 0\n[tuning]\nsearch_relations = false\n");
    CHECK(changed.warnings.empty());
    CHECK(changed.config.limits.search.relation_depth.unlimited());
    CHECK_FALSE(changed.config.tuning.search_relations);
    CHECK_FALSE(parse_config("[tuning]\nsearch_relations = 3\n").warnings.empty());
}

// L3: çekirdek, arama ve senkron sınırları.
TEST_CASE("L3 limits keep today's defaults, accept 0 as unlimited and reject a wrong unit") {
    const Limits defaults = parse_config("").config.limits;
    CHECK(defaults.core.folder_path_bytes == Limit{4096, "4096"});
    CHECK(defaults.search.global_results == Limit{200, "200"});
    CHECK(defaults.search.content_hits == Limit{3, "3"});
    CHECK(defaults.search.fuzzy_candidate_chars == Limit{1024, "1024"});
    CHECK(defaults.sync.merge_input_bytes == Limit{4U * 1024U * 1024U, "4MiB"});

    const LoadedConfig zero = parse_config(
        "[limits.core]\nfolder_path_bytes = 0\n[limits.search]\nglobal_results = 0\ncontent_hits = 0\n"
        "fuzzy_candidate_chars = 0\n[limits.sync]\nmerge_input_bytes = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.limits.core.folder_path_bytes.unlimited());
    CHECK(zero.config.limits.search.global_results.unlimited());
    CHECK(zero.config.limits.search.content_hits.unlimited());
    CHECK(zero.config.limits.search.fuzzy_candidate_chars.unlimited());
    CHECK(zero.config.limits.sync.merge_input_bytes.unlimited());

    const LoadedConfig sized =
        parse_config("[limits.core]\nfolder_path_bytes = \"8KiB\"\n[limits.sync]\n"
                     "merge_input_bytes = \"64MiB\"\n[limits.search]\ncontent_hits = \"5MiB\"\n");
    CHECK(sized.config.limits.core.folder_path_bytes.value == 8192U);
    CHECK(sized.config.limits.sync.merge_input_bytes.value == 64U * 1024U * 1024U);
    CHECK(sized.config.limits.search.content_hits == Limit{3, "3"});
    CHECK(has_warning_about(sized, "limits.search.content_hits", "config.invalid_value"));
}

// L2/L5: TUI ve dil sunucusu sınırları config'tedir; varsayılan bugünkü değerdir, 0 = sınırsız.
TEST_CASE("limits.tui and limits.lsp keep today's defaults, accept 0 as unlimited and reject a wrong unit") {
    const Config defaults = parse_config("").config;
    const TuiLimits& tui = defaults.limits.tui;
    CHECK(tui.view_bytes == Limit{0, "0"});  // NY-P: biçimli görünüm parça parça, varsayılan sınırsız
    CHECK(tui.view_lines.value == 100000U);
    CHECK(tui.table_rows == Limit{0, "0"});  // NY-P: satırlar dizinli, varsayılan sınırsız
    CHECK(tui.table_columns.value == 128U);
    CHECK(tui.highlight_bytes.value == 2U * 1024U * 1024U);
    CHECK(tui.highlight_line_bytes.value == 64U * 1024U);
    CHECK(tui.spellcheck_line_bytes.value == 8U * 1024U);
    CHECK(tui.paste_bytes.value == 32U * 1024U * 1024U);
    CHECK(tui.search_history.value == 200U);
    CHECK(tui.undo_bytes.value == 64U * 1024U * 1024U);
    CHECK(tui.save_suggestions.value == 5U);
    CHECK(tui.paste_timeout.value == 1000U);  // ms; 0 = bitiş işaretini sonsuza dek bekler
    const LspLimits& lsp = defaults.limits.lsp;
    CHECK(lsp.note_bytes.value == 2U * 1024U * 1024U);
    CHECK(lsp.body_bytes.value == 16U * 1024U * 1024U);
    CHECK(lsp.message_depth.value == 64U);
    CHECK(lsp.initialize_timeout.value == 10000U);
    CHECK(lsp.write_timeout.value == 5000U);
    CHECK(lsp.shutdown_timeout.value == 1000U);
    CHECK(defaults.tuning.completion_min_prefix_bytes == 2U);
    // Her anahtarın defaults.toml'da değeri var (boş yazım = dosyada unutulmuş anahtar, sessizce sınırsız).
    for (const auto& [key, text] : limit_entries(defaults)) {
        CAPTURE(key);
        CHECK_FALSE(text.empty());
    }

    const LoadedConfig zero = parse_config(
        "[limits.tui]\nview_bytes = 0\ntable_rows = 0\nundo_bytes = 0\n[limits.lsp]\nbody_bytes = 0\n"
        "write_timeout = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.limits.tui.view_bytes.unlimited());
    CHECK(zero.config.limits.tui.table_rows.unlimited());
    CHECK(zero.config.limits.tui.undo_bytes.unlimited());
    CHECK(zero.config.limits.lsp.body_bytes.unlimited());
    CHECK(zero.config.limits.lsp.write_timeout.unlimited());

    const LoadedConfig sized = parse_config(
        "[limits.tui]\nview_bytes = \"4MiB\"\ntable_rows = \"5KiB\"\n[limits.lsp]\ninitialize_timeout = \"2min\"\n"
        "[tuning]\ncompletion_min_prefix_bytes = 0\n");
    CHECK(sized.config.limits.tui.view_bytes.value == 4U * 1024U * 1024U);
    CHECK(sized.config.limits.lsp.initialize_timeout.value == 120000U);
    CHECK(sized.config.limits.tui.table_rows.unlimited());  // geçersiz değer varsayılana (0) döner
    CHECK(has_warning_about(sized, "limits.tui.table_rows", "config.invalid_value"));
    CHECK(sized.config.tuning.completion_min_prefix_bytes == 2U);  // alt sınır: 0 geçersiz
    CHECK(has_warning_about(sized, "tuning.completion_min_prefix_bytes", "config.invalid_value"));

    Config c = defaults;
    REQUIRE(set_config_value(c, "limits.lsp.message_values", "0"));
    CHECK(c.limits.lsp.message_values.unlimited());
    const auto entries = limit_entries(c);
    const auto listed = [&](std::string_view key) {
        return std::ranges::find(entries, key, &std::pair<std::string, std::string>::first) != entries.end();
    };
    CHECK(listed("limits.lsp.shutdown_timeout"));
}

TEST_CASE("limits.app.recent_commands defaults to 20 and accepts 0 as unlimited") {
    CHECK(parse_config("").config.limits.app.recent_commands == Limit{20, "20"});

    const LoadedConfig zero = parse_config("[limits.app]\nrecent_commands = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.limits.app.recent_commands.unlimited());
    const LoadedConfig sized = parse_config("[limits.app]\nrecent_commands = \"5MiB\"\n");
    CHECK(sized.config.limits.app.recent_commands == Limit{20, "20"});
    CHECK(has_warning_about(sized, "limits.app.recent_commands", "config.invalid_value"));
}

TEST_CASE("limits.app.trash_undo_steps defaults to 0 (unlimited) and takes a count") {
    CHECK(parse_config("").config.limits.app.trash_undo_steps.unlimited());

    const LoadedConfig sized = parse_config("[limits.app]\ntrash_undo_steps = 50\n");
    CHECK(sized.warnings.empty());
    CHECK(sized.config.limits.app.trash_undo_steps == Limit{50, "50"});
    const LoadedConfig bytes = parse_config("[limits.app]\ntrash_undo_steps = \"5MiB\"\n");
    CHECK(bytes.config.limits.app.trash_undo_steps.unlimited());
    CHECK(has_warning_about(bytes, "limits.app.trash_undo_steps", "config.invalid_value"));
}

// app.tabs ve app.map_nodes yalnız app/arayüz testlerindeydi; anahtar ekleme kuralının (4) config testi.
TEST_CASE("limits.app.tabs defaults to 0 (unlimited), limits.app.map_nodes to 2000; both take a count only") {
    const LoadedConfig defaults = parse_config("");
    CHECK(defaults.config.limits.app.tabs.unlimited());
    CHECK(defaults.config.limits.app.map_nodes == Limit{2000, "2000"});

    const LoadedConfig set = parse_config("[limits.app]\ntabs = 8\nmap_nodes = 0\n");
    CHECK(set.warnings.empty());
    CHECK(set.config.limits.app.tabs == Limit{8, "8"});
    CHECK(set.config.limits.app.map_nodes.unlimited());

    const LoadedConfig bad = parse_config("[limits.app]\ntabs = \"5MiB\"\nmap_nodes = -1\n");
    CHECK(bad.config.limits.app.tabs.unlimited());
    CHECK(bad.config.limits.app.map_nodes == Limit{2000, "2000"});
    CHECK(has_warning_about(bad, "limits.app.tabs", "config.invalid_value"));
    CHECK(has_warning_about(bad, "limits.app.map_nodes", "config.invalid_value"));

    Config config = defaults.config;
    REQUIRE(set_config_value(config, "limits.app.tabs", "3"));
    REQUIRE(set_config_value(config, "limits.app.map_nodes", "0"));
    CHECK(config.limits.app.tabs == Limit{3, "3"});
    CHECK(config.limits.app.map_nodes.unlimited());
    CHECK_FALSE(set_config_value(config, "limits.app.tabs", "2s"));
}

// NX-B (L1): harita çevre derinliğinin sabit tavanı (MapOptions::kMaxDepth = 6) config'e taşındı.
// NY-P: önizleme parçalarının düşman girdi korumaları config'te (kural: kodda sabit sınır yok); 0 = sınırsız.
TEST_CASE("limits.tui preview chunk guards default to 128KiB, 64 and 4KiB, accept 0 and round-trip") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    const Config defaults = parse_config("").config;
    CHECK(defaults.limits.tui.preview_definition_bytes == Limit{128U * 1024U, "128KiB"});
    CHECK(defaults.limits.tui.preview_fence_info_bytes == Limit{64, "64"});
    CHECK(defaults.limits.tui.preview_table_header_bytes == Limit{4U * 1024U, "4KiB"});

    const LoadedConfig zero = parse_config(
        "[limits.tui]\npreview_definition_bytes = 0\npreview_fence_info_bytes = 0\npreview_table_header_bytes = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.limits.tui.preview_definition_bytes.unlimited());
    CHECK(zero.config.limits.tui.preview_fence_info_bytes.unlimited());
    CHECK(zero.config.limits.tui.preview_table_header_bytes.unlimited());
    const LoadedConfig bad = parse_config("[limits.tui]\npreview_definition_bytes = \"2s\"\n");
    CHECK(bad.config.limits.tui.preview_definition_bytes == Limit{128U * 1024U, "128KiB"});
    CHECK(has_warning_about(bad, "limits.tui.preview_definition_bytes", "config.invalid_value"));

    Config config = defaults;
    REQUIRE(set_config_value(config, "limits.tui.preview_definition_bytes", "1MiB"));
    REQUIRE(set_config_value(config, "limits.tui.preview_fence_info_bytes", "16"));
    REQUIRE(set_config_value(config, "limits.tui.preview_table_header_bytes", "0"));
    REQUIRE(save_config(config).has_value());
    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.limits.tui.preview_definition_bytes == Limit{1024U * 1024U, "1MiB"});
    CHECK(reloaded.config.limits.tui.preview_fence_info_bytes == Limit{16, "16"});
    CHECK(reloaded.config.limits.tui.preview_table_header_bytes.unlimited());
}

TEST_CASE("limits.app.map_depth defaults to 6, takes a count, accepts 0 as unlimited and round-trips") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    const Config defaults = parse_config("").config;
    CHECK(defaults.limits.app.map_depth == Limit{6, "6"});

    const LoadedConfig zero = parse_config("[limits.app]\nmap_depth = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.limits.app.map_depth.unlimited());
    const LoadedConfig bad = parse_config("[limits.app]\nmap_depth = \"5MiB\"\n");
    CHECK(bad.config.limits.app.map_depth == Limit{6, "6"});
    CHECK(has_warning_about(bad, "limits.app.map_depth", "config.invalid_value"));

    Config config = defaults;
    CHECK_FALSE(set_config_value(config, "limits.app.map_depth", "2s"));
    REQUIRE(set_config_value(config, "limits.app.map_depth", "12"));
    const auto entries = limit_entries(config);
    const auto listed =
        std::ranges::find(entries, std::string_view("limits.app.map_depth"), &std::pair<std::string, std::string>::first);
    REQUIRE(listed != entries.end());
    CHECK(listed->second == "12");
    REQUIRE(save_config(config).has_value());
    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.limits.app.map_depth == Limit{12, "12"});
}

TEST_CASE("limits.app.query_cell_bytes defaults to 4KiB, takes a size and accepts 0 as unlimited") {
    const Config defaults = parse_config("").config;
    CHECK(defaults.limits.app.query_cell_bytes == Limit{4096, "4KiB"});
    const LoadedConfig zero = parse_config("[limits.app]\nquery_cell_bytes = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.limits.app.query_cell_bytes.unlimited());
    const LoadedConfig sized = parse_config("[limits.app]\nquery_cell_bytes = \"64KiB\"\n");
    CHECK(sized.config.limits.app.query_cell_bytes.cap() == 64U * 1024U);
    const LoadedConfig bad = parse_config("[limits.app]\nquery_cell_bytes = \"2s\"\n");
    CHECK(bad.config.limits.app.query_cell_bytes == Limit{4096, "4KiB"});
    CHECK(has_warning_about(bad, "limits.app.query_cell_bytes", "config.invalid_value"));
}

TEST_CASE("an invalid [limits] value warns through the config path and keeps the default") {
    for (const std::string value : {"-1", "\"lots\"", "\"5MiB\"", "true", "1.5"}) {
        CAPTURE(value);
        const LoadedConfig loaded = parse_config("[limits.search]\ncontent_results = " + value + "\n");
        CHECK(loaded.config.limits.search.content_results == Limit{1000, "1000"});
        REQUIRE(loaded.warnings.size() == 1);
        CHECK(loaded.warnings[0].message_key == "config.invalid_value");
        CHECK(arg_of(loaded.warnings[0], "key") == "limits.search.content_results");
        CHECK(arg_of(loaded.warnings[0], "reason") == "must_be_limit_count");
    }
    const LoadedConfig unknown = parse_config("[limits]\nnope = 1\n[limits.search]\ntypo = 2\n");
    CHECK(has_warning_about(unknown, "limits.nope", "config.unknown_key"));
    CHECK(has_warning_about(unknown, "limits.search.typo", "config.unknown_key"));
    CHECK(has_warning_about(parse_config("limits = 3\n"), "limits", "config.invalid_value"));
}

TEST_CASE("tuning.max_note_bytes accepts 0 as unlimited and size units") {
    const LoadedConfig zero = parse_config("[tuning]\nmax_note_bytes = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.tuning.max_note_bytes.unlimited());
    const LoadedConfig units = parse_config("[tuning]\nmax_note_bytes = \"64MiB\"\n");
    CHECK(units.warnings.empty());
    CHECK(units.config.tuning.max_note_bytes.value == 64U * 1024U * 1024U);
    const LoadedConfig bad = parse_config("[tuning]\nmax_note_bytes = -1\n");
    CHECK(bad.config.tuning.max_note_bytes.value == 10485760U);
    CHECK(has_warning_about(bad, "tuning.max_note_bytes", "config.invalid_value"));
}

// NY-G: bildirim süresi; 0 = kendiliğinden kapanmaz (sabit tavan yok), süre birimi yazılabilir.
TEST_CASE("tuning.notice_ms defaults to 4 s, accepts 0 as never and duration units") {
    CHECK(parse_config("").config.tuning.notice_ms == Limit{4000, "4000"});
    const LoadedConfig zero = parse_config("[tuning]\nnotice_ms = 0\n");
    CHECK(zero.warnings.empty());
    CHECK(zero.config.tuning.notice_ms.unlimited());
    const LoadedConfig units = parse_config("[tuning]\nnotice_ms = \"6s\"\n");
    CHECK(units.warnings.empty());
    CHECK(units.config.tuning.notice_ms.value == 6000U);
    const LoadedConfig bad = parse_config("[tuning]\nnotice_ms = -1\n");
    CHECK(bad.config.tuning.notice_ms.value == 4000U);
    CHECK(has_warning_about(bad, "tuning.notice_ms", "config.invalid_value"));
}

TEST_CASE("set_config_value sets limits keys with the file rules and limit_entries lists them") {
    Config c = parse_config("").config;
    const auto entry = [&c](std::string_view key) {
        const auto entries = limit_entries(c);
        const auto found = std::ranges::find(entries, key, &std::pair<std::string, std::string>::first);
        return found == entries.end() ? std::string("<missing>") : found->second;
    };
    CHECK(entry("limits.search.content_results") == "1000");

    REQUIRE(set_config_value(c, "limits.search.content_results", "0"));
    CHECK(c.limits.search.content_results.unlimited());
    CHECK(entry("limits.search.content_results") == "0");

    for (const char* bad : {"-3", "abc", "5MiB", ""}) {
        CAPTURE(bad);
        const auto set = set_config_value(c, "limits.search.content_results", bad);
        REQUIRE_FALSE(set.has_value());
        CHECK(arg_of(set.error(), "key") == "limits.search.content_results");
    }
    CHECK(c.limits.search.content_results.unlimited());  // reddedilen değer dokunmaz
    CHECK_FALSE(set_config_value(c, "limits.search.nope", "1"));
    CHECK_FALSE(set_config_value(c, "limits.search", "1"));
    // Ayar ekranları config_entries'i listeler; [limits] yalnız dosyada ve CLI'da (iki arayüz eşit).
    for (const auto& [key, value] : config_entries(c))
        CHECK_FALSE(key.starts_with("limits."));
}

// L4: graf, kullanıcı sorgusu ve görünüm sınırları config'tedir; varsayılan bugünkü değerdir, 0 = sınırsız.
TEST_CASE("limits.graph, limits.query and limits.view default to today's values and accept 0 and units") {
    const Limits defaults = parse_config("").config.limits;
    CHECK(defaults.graph.frame_bytes == Limit{64U * 1024U * 1024U, "64MiB"});
    CHECK(defaults.graph.frame_ops == Limit{65536, "65536"});
    CHECK(defaults.graph.group_bytes == Limit{std::uint64_t{1} << 30U, "1GiB"});
    CHECK(defaults.graph.group_ops == Limit{std::uint64_t{1} << 22U, "4194304"});
    CHECK(defaults.graph.peers == Limit{256, "256"});
    CHECK(defaults.graph.hlc_skew == Limit{24U * 60U * 60U * 1000U, "24h"});
    CHECK(defaults.graph.import_line_bytes == Limit{128U * 1024U * 1024U, "128MiB"});
    CHECK(defaults.graph.import_line_values == Limit{100000, "100000"});
    CHECK(defaults.query.rows == Limit{100000, "100000"});
    CHECK(defaults.query.duration == Limit{5000, "5s"});
    CHECK(defaults.query.text == Limit{64U * 1024U, "64KiB"});
    CHECK(defaults.view == defaults.query);  // görünüm profili korumalı kalır: bugünkü değerler

    const LoadedConfig set = parse_config(
        "[limits.graph]\nframe_ops = 10\nset_aside_logs = 0\ngroup_bytes = 0\nimport_line_values = 0\n"
        "[limits.query]\nrows = 0\nduration = \"2min\"\n"
        "[limits.view]\nrows = 3\nmemory = \"1MiB\"\n");
    INFO((set.warnings.empty() ? std::string() : set.warnings.front().message_key));
    CHECK(set.warnings.empty());
    CHECK(set.config.limits.graph.frame_ops == Limit{10, "10"});
    CHECK(set.config.limits.graph.set_aside_logs.unlimited());
    CHECK(set.config.limits.graph.group_bytes.unlimited());
    CHECK(set.config.limits.graph.import_line_values.unlimited());
    CHECK(set.config.limits.query.rows.unlimited());
    CHECK(set.config.limits.query.duration.value == 120000);
    CHECK(set.config.limits.view.rows == Limit{3, "3"});
    CHECK(set.config.limits.view.memory.value == 1024U * 1024U);
    CHECK(set.config.limits.view.duration == defaults.view.duration);  // profiller ayrı

    const LoadedConfig bad = parse_config(
        "[limits.view]\nduration = \"5MiB\"\n[limits.graph]\npeers = -1\ngroup_bytes = \"5s\"\n");
    CHECK(has_warning_about(bad, "limits.view.duration"));
    CHECK(has_warning_about(bad, "limits.graph.peers"));
    CHECK(has_warning_about(bad, "limits.graph.group_bytes"));
    CHECK(bad.config.limits == defaults);
}

// Bütçenin sabit tavanı (32.768) kalktı; L4b'den beri çerçeve sınırı da onu kırpmaz (işlem kayıtlara bölünür).
TEST_CASE("tuning.graph_batch_ops has no fixed ceiling") {
    const LoadedConfig big =
        parse_config("[tuning]\ngraph_batch_ops = 100000\n[limits.graph]\nframe_ops = 0\n");
    CHECK(big.warnings.empty());
    CHECK(big.config.tuning.graph_batch_ops == 100000);
}

TEST_CASE("save_config keeps the [limits] section as the user wrote it") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    // Sonraki sürümün anahtarı ve birimli yazım da korunur.
    std::ofstream(config_file(), std::ios::binary)
        << "[limits.search]\ncontent_results = \"0\"\nfuture_key = \"64MiB\"\n"
           "[tuning]\nmax_note_bytes = \"64MiB\"\n";
    Config c = load_config().config;
    REQUIRE(c.limits.search.content_results.unlimited());
    c.language = "en";  // başka bir ayar kaydı
    REQUIRE(save_config(c).has_value());

    const std::string text = slurp(config_file());
    CHECK(text.find("content_results = \"0\"") != std::string::npos);
    CHECK(text.find("future_key = \"64MiB\"") != std::string::npos);
    const std::size_t tuning_line = text.find("max_note_bytes = ");
    REQUIRE(tuning_line != std::string::npos);
    CHECK(text.substr(tuning_line, text.find('\n', tuning_line) - tuning_line).find("64MiB") !=
          std::string::npos);
    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.config.limits == c.limits);
    CHECK(reloaded.config.tuning == c.tuning);
    CHECK(has_warning_about(reloaded, "limits.search.future_key", "config.unknown_key"));
}

TEST_CASE("save_config writes a limits value changed in memory and nothing when untouched") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    Config c = parse_config("").config;
    REQUIRE(save_config(c).has_value());
    CHECK(slurp(config_file()).find("limits") == std::string::npos);  // varsayılan dosyaya gömülmez

    REQUIRE(set_config_value(c, "limits.search.content_results", "0"));
    REQUIRE(save_config(c).has_value());
    const LoadedConfig reloaded = load_config();
    CHECK(reloaded.warnings.empty());
    CHECK(reloaded.config.limits.search.content_results == Limit{0, "0"});
}
