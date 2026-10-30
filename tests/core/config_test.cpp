#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "core/resources.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <string>

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
    const LoadedConfig loaded = parse_config("");
    CHECK(loaded.warnings.empty());
    const Config& c = loaded.config;
    CHECK(c.language == "auto");
    CHECK(c.notes_dir == expand_user_path("~/notes"));
    CHECK(c.editor.empty());
    CHECK(c.default_type == "md");
    CHECK(c.default_folder.empty());
    CHECK(c.types == std::vector<std::string>{"md", "txt", "csv", "cpp", "java", "py", "json", "sh"});
    CHECK_FALSE(c.sync.enabled);
    CHECK(c.sync.branch == "main");
    CHECK(c.sync.idle_seconds == 30);
    CHECK(c.tuning.slug_max_length == 60);
    CHECK(c.tuning.min_confidence == doctest::Approx(0.5));
    CHECK(c.tuning.max_scan_bytes == 65536);
    CHECK(c.tuning.max_note_bytes == 10485760);
    CHECK(c.tuning.rescan_seconds == 5);
    CHECK(c.tuning.autosave_ms == 1000);
    CHECK(c.tuning.exit_timeout_seconds == 10);
    CHECK(c.tuning.log_max_bytes == 1048576);
    CHECK(c.tuning.sync_stale_lock_minutes == 10);
    CHECK(c.tuning.sync_timeout_seconds == 120);
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

TEST_CASE("NOTEAPP_HOME overrides config and state dirs, including non-ASCII paths") {
    const TempDir tmp(kTurkishUncreated);
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    CHECK(config_dir() == tmp.path() / "config");
    CHECK(state_dir() == tmp.path() / "state");
    CHECK(config_file() == tmp.path() / "config" / "config.toml");
}

TEST_CASE("platform default dirs follow SPEC section 8") {
    const ScopedEnv no_home("NOTEAPP_HOME", std::nullopt);
#ifdef _WIN32
    const ScopedEnv roaming("APPDATA", u8"C:\\Users\\İşıl\\AppData\\Roaming");
    const ScopedEnv local("LOCALAPPDATA", u8"C:\\Users\\İşıl\\AppData\\Local");
    CHECK(config_dir() == fs::path(u8"C:\\Users\\İşıl\\AppData\\Roaming") / "noteapp");
    CHECK(state_dir() == fs::path(u8"C:\\Users\\İşıl\\AppData\\Local") / "noteapp");
#elif defined(__APPLE__)
    CHECK(config_dir() == home_dir() / "Library" / "Application Support" / "noteapp");
    CHECK(state_dir() == config_dir());
#else
    {
        const ScopedEnv xdg_config("XDG_CONFIG_HOME", u8"/x/config");
        const ScopedEnv xdg_state("XDG_STATE_HOME", u8"/x/state");
        CHECK(config_dir() == fs::path("/x/config/noteapp"));
        CHECK(state_dir() == fs::path("/x/state/noteapp"));
    }
    const ScopedEnv no_config("XDG_CONFIG_HOME", std::nullopt);
    const ScopedEnv no_state("XDG_STATE_HOME", std::nullopt);
    CHECK(config_dir() == home_dir() / ".config" / "noteapp");
    CHECK(state_dir() == home_dir() / ".local" / "state" / "noteapp");
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
    c.tuning.max_note_bytes += 1;
    c.tuning.rescan_seconds += 1;
    c.tuning.autosave_ms += 1;
    c.tuning.exit_timeout_seconds += 1;
    c.tuning.log_max_bytes += 1;
    c.tuning.sync_stale_lock_minutes += 1;
    c.tuning.sync_timeout_seconds += 1;
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
    CHECK(by_key.at("types") == "md, txt, csv, cpp, java, py, json, sh");
    CHECK(by_key.at("sync.enabled") == "false");
    CHECK(by_key.at("sync.idle_seconds") == "30");
    CHECK(by_key.count("tuning.autosave_ms") == 0);
    CHECK(entries.front().first == "language");
}
