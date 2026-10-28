#include "core/config.hpp"
#include "core/paths.hpp"
#include "core/resources.hpp"

#include <algorithm>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>

#ifdef _WIN32
#include <stdlib.h>  // _wputenv_s
#endif

using namespace noteapp::core;
namespace fs = std::filesystem;

namespace {

// Ortam değişkenini ayarlar ve çıkışta eski değeri geri yükler. Windows'ta geniş dizgeyle yazılır:
// ANSI sürümü Türkçe karakterleri bozar (üretim kodu da _wgetenv ile okur).
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::optional<std::u8string>& value) : name_(std::move(name)) {
        if (const auto old = read(name_))
            old_ = *old;
        write(name_, value);
    }
    ~ScopedEnv() { write(name_, old_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    static std::optional<std::u8string> read(const std::string& name) {
#ifdef _WIN32
        const std::wstring wname(name.begin(), name.end());
        const wchar_t* v = _wgetenv(wname.c_str());
        if (v == nullptr)
            return std::nullopt;
        return fs::path(std::wstring(v)).u8string();
#else
        const char* v = std::getenv(name.c_str());
        if (v == nullptr)
            return std::nullopt;
        return std::u8string(v, v + std::char_traits<char>::length(v));
#endif
    }
    static void write(const std::string& name, const std::optional<std::u8string>& value) {
#ifdef _WIN32
        const std::wstring wname(name.begin(), name.end());
        _wputenv_s(wname.c_str(), value ? fs::path(*value).wstring().c_str() : L"");  // boş = silinir
#else
        if (value)
            setenv(name.c_str(), std::string(value->begin(), value->end()).c_str(), 1);
        else
            unsetenv(name.c_str());
#endif
    }

    std::string name_;
    std::optional<std::u8string> old_;
};

// Türkçe karakterli geçici kök: yol/ortam dönüşümlerindeki UTF-8 hatalarını yakalar.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() / fs::path(u8"noteapp-İşğü-" + to_u8(std::to_string(rd())));
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    static std::u8string to_u8(const std::string& s) { return std::u8string(s.begin(), s.end()); }
    fs::path path_;
};

bool has_warning_about(const LoadedConfig& loaded, std::string_view needle) {
    return std::any_of(loaded.warnings.begin(), loaded.warnings.end(),
                       [&](const std::string& w) { return w.find(needle) != std::string::npos; });
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
    CHECK(has_warning_about(loaded, "colour"));
    CHECK(has_warning_about(loaded, "sync.foo"));
    CHECK(has_warning_about(loaded, "tuning.bar"));
    CHECK(has_warning_about(loaded, "extra"));
}

TEST_CASE("parse_config survives a TOML syntax error with defaults and a positioned warning") {
    const Config defaults = parse_config("").config;
    const LoadedConfig loaded = parse_config("language = \"tr\"\neditor = = broken\n");
    CHECK(loaded.config == defaults);  // kısmi uygulama yok: dosya bütün olarak reddedilir
    REQUIRE(loaded.warnings.size() == 1);
    CHECK(loaded.warnings[0].find("line 2") != std::string::npos);
}

TEST_CASE("parse_config expands ~ and environment variables in notes_dir") {
    const TempDir tmp;
    const ScopedEnv env("NOTEAPP_TEST_DIR", tmp.path().u8string());
    CHECK(parse_config(R"(notes_dir = "~/n")").config.notes_dir == home_dir() / "n");
    CHECK(parse_config(R"(notes_dir = "$NOTEAPP_TEST_DIR/n")").config.notes_dir == tmp.path() / "n");
}

TEST_CASE("NOTEAPP_HOME overrides config and state dirs, including non-ASCII paths") {
    const TempDir tmp;
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
    const TempDir tmp;
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

TEST_CASE("load_config with a corrupt file returns defaults and a warning instead of failing") {
    const TempDir tmp;
    const ScopedEnv env("NOTEAPP_HOME", tmp.path().u8string());
    fs::create_directories(config_dir());
    std::ofstream(config_file(), std::ios::binary) << "this is [not toml";

    const LoadedConfig loaded = load_config();
    CHECK(loaded.config == parse_config("").config);
    CHECK_FALSE(loaded.warnings.empty());
}
