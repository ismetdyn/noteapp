#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "sync/auth.hpp"
#include "sync/git_runtime.hpp"
#include "sync/sync_service.hpp"
#include "sync_fixture.hpp"

#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <git2.h>
#include <git2/sys/credential.h>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using noteapp::core::Error;
using noteapp::sync::Auth;
using noteapp::sync::Credentials;
using noteapp::sync::SyncService;
using namespace noteapp::sync_test;

namespace {

constexpr const char* kUrl = "https://github.com/kisi/notlar.git";

Credentials github_credentials(std::optional<std::string> token) {
    Credentials credentials;
    credentials.host = "github.com";
    credentials.token = std::move(token);
    return credentials;
}

std::expected<std::optional<std::string>, Error> no_keychain_entry(std::string_view) {
    return std::nullopt;
}

// Hata metni ya da argümanlarından biri `secret`'i içeriyor mu.
bool mentions(const Error& error, std::string_view secret) {
    if (noteapp::core::format_error(error).find(secret) != std::string::npos)
        return true;
    for (const auto& [name, value] : error.args)
        if (value.find(secret) != std::string::npos)
            return true;
    return false;
}

std::vector<std::string> files_containing(const fs::path& root, std::string_view needle) {
    std::vector<std::string> hits;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && read_file(entry.path()).find(needle) != std::string::npos)
            hits.push_back(utf8(entry.path()));
    return hits;
}

}  // namespace

TEST_CASE("parse_remote derives the auth kind and host from the URL scheme") {
    struct Row {
        std::string_view url;
        Auth auth;
        std::string_view host;
        std::string_view port;
    };
    for (const Row& row : {
             Row{"https://github.com/kisi/notlar.git", Auth::Https, "github.com", ""},
             Row{"HTTPS://GitHub.COM/kisi/notlar", Auth::Https, "github.com", ""},
             Row{"https://git.example.com:8443/notlar.git", Auth::Https, "git.example.com", "8443"},
             Row{"git@github.com:kisi/notlar.git", Auth::Ssh, "github.com", ""},
             Row{"github.com:kisi/notlar.git", Auth::Ssh, "github.com", ""},
             Row{"ssh://git@github.com/kisi/notlar.git", Auth::Ssh, "github.com", ""},
             Row{"ssh://git@git.example.com:2222/notlar.git", Auth::Ssh, "git.example.com", "2222"},
             Row{"git+ssh://git@github.com/kisi/notlar.git", Auth::Ssh, "github.com", ""},
             Row{"ssh+git://git@github.com/kisi/notlar.git", Auth::Ssh, "github.com", ""},
             Row{"http://github.com/kisi/notlar.git", Auth::None, "github.com", ""},
             Row{"file:///srv/git/notlar.git", Auth::None, "", ""},
             Row{"/srv/git/notlar.git", Auth::None, "", ""},
             Row{"C:/Users/kişi/uzak depo.git", Auth::None, "", ""},
             Row{"C:\\Users\\kişi\\uzak depo.git", Auth::None, "", ""},
             Row{"../uzak depo.git", Auth::None, "", ""},
             Row{"uzak depo.git", Auth::None, "", ""},
         }) {
        CAPTURE(row.url);
        const noteapp::sync::RemoteEndpoint endpoint = noteapp::sync::parse_remote(row.url);
        CHECK(endpoint.auth == row.auth);
        CHECK(endpoint.host == row.host);
        CHECK(endpoint.port == row.port);
    }
    CHECK(noteapp::sync::parse_remote("https://kisi:gizli@github.com/n.git").userinfo == "kisi:gizli");
    CHECK(noteapp::sync::parse_remote("https://ghp_x@github.com/n.git").host == "github.com");
    CHECK(noteapp::sync::parse_remote("ssh://git@github.com/n.git").userinfo == "git");
}

TEST_CASE("token order: NOTEAPP_GIT_TOKEN first, the keychain is not even asked") {
    const EnvGuard env("NOTEAPP_GIT_TOKEN", fs::path("env-token"));
    int asked = 0;
    const auto token = noteapp::sync::resolve_token(
        "github.com", [&](std::string_view) -> std::expected<std::optional<std::string>, Error> {
            ++asked;
            return std::string("keychain-token");
        });
    REQUIRE_MESSAGE(token.has_value(), describe(token));
    CHECK(*token == "env-token");
    CHECK(asked == 0);
}

TEST_CASE("token order: without NOTEAPP_GIT_TOKEN the host's keychain entry is used") {
    const EnvGuard env("NOTEAPP_GIT_TOKEN", std::nullopt);
    std::string asked_host;
    const auto token = noteapp::sync::resolve_token(
        "github.com", [&](std::string_view host) -> std::expected<std::optional<std::string>, Error> {
            asked_host = host;
            return std::string("keychain-token");
        });
    REQUIRE_MESSAGE(token.has_value(), describe(token));
    CHECK(*token == "keychain-token");
    CHECK(asked_host == "github.com");
}

TEST_CASE(
    "NOTEAPP_GIT_TOKEN goes only to NOTEAPP_GIT_TOKEN_HOST (default github.com), others ask the keychain") {
    const EnvGuard env("NOTEAPP_GIT_TOKEN", fs::path("env-token"));
    const auto keychain = [](std::string_view host) -> std::expected<std::optional<std::string>, Error> {
        return "keychain-" + std::string(host);
    };
    const auto token_for = [&](std::string_view host, const noteapp::sync::TokenReader& reader) {
        const auto token = noteapp::sync::resolve_token(host, reader);
        return token ? *token : token.error().message_key;
    };
    {
        const EnvGuard bound("NOTEAPP_GIT_TOKEN_HOST", std::nullopt);
        CHECK(token_for("github.com", keychain) == "env-token");
        CHECK(token_for("evil.example", keychain) == "keychain-evil.example");
        CHECK(token_for("evil.example", no_keychain_entry) == "sync.token_missing");
    }
    {
        const EnvGuard bound("NOTEAPP_GIT_TOKEN_HOST", fs::path("Git.Example.COM"));
        CHECK(token_for("git.example.com", keychain) == "env-token");
        CHECK(token_for("github.com", keychain) == "keychain-github.com");
    }
}

TEST_CASE("no token anywhere is sync.token_missing; an unavailable keychain is reported as such") {
    const EnvGuard env("NOTEAPP_GIT_TOKEN", std::nullopt);
    const auto missing = noteapp::sync::resolve_token("github.com", no_keychain_entry);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message_key == "sync.token_missing");
    CHECK(missing.error().args == noteapp::core::Args{{"host", "github.com"}});

    const auto unavailable = noteapp::sync::resolve_token(
        "github.com", [](std::string_view) -> std::expected<std::optional<std::string>, Error> {
            return std::unexpected(
                Error{Error::Code::Io, "sync.keychain_unavailable", {{"detail", "no Secret Service"}}});
        });
    REQUIRE_FALSE(unavailable.has_value());
    CHECK(unavailable.error().message_key == "sync.keychain_unavailable");
}

TEST_CASE("store_token refuses an empty or blank token before touching the keychain") {
    for (const std::string_view token : {"", " \t\r\n"}) {
        const auto stored = noteapp::sync::store_token("github.com", token);
        REQUIRE_FALSE(stored.has_value());
        CHECK(stored.error().message_key == "sync.token_missing");
    }
}

// Elle: gerçek OS kasasına yazar; varsayılan koşuda kullanıcının kasasına dokunulmaz.
// Koşturma: sync_tests --no-skip -tc="*keychain*"
TEST_CASE("store_token, has_token and clear_token round-trip through the OS keychain (manual)" *
          doctest::skip()) {
    const EnvGuard env("NOTEAPP_GIT_TOKEN", std::nullopt);
    const std::string host = "noteapp-test.invalid";
    REQUIRE(noteapp::sync::store_token(host, "  test-token-çğü\n").has_value());
    CHECK(noteapp::sync::has_token(host));
    CHECK(noteapp::sync::has_token("NOTEAPP-TEST.invalid"));  // host büyük/küçük harfe duyarsız
    const auto token = noteapp::sync::resolve_token(host);
    REQUIRE_MESSAGE(token.has_value(), describe(token));
    CHECK(*token == "test-token-çğü");
    REQUIRE(noteapp::sync::clear_token(host).has_value());
    CHECK_FALSE(noteapp::sync::has_token(host));
    CHECK(noteapp::sync::clear_token(host).has_value());  // kayıt yokken de başarılı
}

TEST_CASE("acceptance 7: the credentials callback gives x-access-token once, the second call fails") {
    const noteapp::sync::GitRuntime runtime;  // git_credential ayırıcısı için
    Credentials credentials = github_credentials("secret-token");
    git_credential* given = nullptr;
    REQUIRE(noteapp::sync::acquire_credentials(&given, kUrl, GIT_CREDENTIAL_USERPASS_PLAINTEXT,
                                               credentials) == 0);
    REQUIRE(given != nullptr);
    REQUIRE(given->credtype == GIT_CREDENTIAL_USERPASS_PLAINTEXT);
    const auto* userpass = reinterpret_cast<const git_credential_userpass_plaintext*>(given);
    CHECK(std::string(userpass->username) == "x-access-token");
    CHECK(std::string(userpass->password) == "secret-token");
    git_credential_free(given);
    CHECK_FALSE(credentials.error.has_value());

    // Sunucu ilk kimliği reddetti: aynı token'ı yeniden vermek sonsuz döngü olurdu.
    given = nullptr;
    CHECK(noteapp::sync::acquire_credentials(&given, kUrl, GIT_CREDENTIAL_USERPASS_PLAINTEXT, credentials) <
          0);
    CHECK(given == nullptr);
    CHECK(credentials.calls == 2);
    REQUIRE(credentials.error.has_value());
    CHECK(credentials.error->message_key == "sync.auth_failed");
    CHECK(credentials.error->args == noteapp::core::Args{{"host", "github.com"}});
}

TEST_CASE("the credentials callback never gives the token to another host, over http or as non-userpass") {
    const noteapp::sync::GitRuntime runtime;
    struct Row {
        const char* url;
        unsigned int allowed;
    };
    for (const Row row : {Row{"https://evil.example/kisi/notlar.git", GIT_CREDENTIAL_USERPASS_PLAINTEXT},
                          Row{"http://github.com/kisi/notlar.git", GIT_CREDENTIAL_USERPASS_PLAINTEXT},
                          Row{kUrl, GIT_CREDENTIAL_SSH_KEY}}) {
        CAPTURE(row.url);
        Credentials credentials = github_credentials("secret-token");
        git_credential* given = nullptr;
        CHECK(noteapp::sync::acquire_credentials(&given, row.url, row.allowed, credentials) < 0);
        CHECK(given == nullptr);
        REQUIRE(credentials.error.has_value());
        CHECK(credentials.error->message_key == "sync.auth_failed");
    }
}

TEST_CASE("the credentials callback passes without a token (non-https remotes)") {
    Credentials credentials = github_credentials(std::nullopt);
    git_credential* given = nullptr;
    CHECK(noteapp::sync::acquire_credentials(&given, "ssh://git@github.com/n.git", GIT_CREDENTIAL_SSH_KEY,
                                             credentials) == GIT_PASSTHROUGH);
    CHECK(given == nullptr);
    CHECK_FALSE(credentials.error.has_value());
}

TEST_CASE("SSH precheck asks ssh-keygen -F: a known host passes, an unknown one is sync.ssh_host_unknown") {
    const EnvGuard path("PATH", fs::path(NOTEAPP_FAKE_SSH_DIR));
    for (const std::string_view url :
         {"git@known.example:kisi/notlar.git", "ssh://git@known.example/n.git",
          "ssh://git@known.example:22/n.git", "ssh://git@known.example:2222/n.git"}) {
        CAPTURE(url);
        const auto known = noteapp::sync::check_ssh_host_known(noteapp::sync::parse_remote(url));
        CHECK_MESSAGE(known.has_value(), describe(known));
    }

    const auto unknown =
        noteapp::sync::check_ssh_host_known(noteapp::sync::parse_remote("git@unknown.example:kisi/n.git"));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().message_key == "sync.ssh_host_unknown");
    CHECK(unknown.error().args == noteapp::core::Args{{"host", "unknown.example"}});
    // Port farklıysa known_hosts'ta "[host]:port" aranır: düz host kaydı o portu onaylamaz.
    CHECK_FALSE(
        noteapp::sync::check_ssh_host_known(noteapp::sync::parse_remote("ssh://git@known.example:2200/n.git"))
            .has_value());
}

TEST_CASE("SSH precheck without ssh-keygen on PATH is sync.ssh_unavailable") {
    const TempDir empty;
    const EnvGuard path("PATH", empty.path());
    const auto result =
        noteapp::sync::check_ssh_host_known(noteapp::sync::parse_remote("git@known.example:kisi/n.git"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.ssh_unavailable");
}

TEST_CASE("prepare_credentials: https takes the token up front, ssh runs the precheck, local needs nothing") {
    {
        const EnvGuard env("NOTEAPP_GIT_TOKEN", fs::path("env-token"));
        const auto https = noteapp::sync::prepare_credentials("https://GitHub.com/kisi/notlar.git");
        REQUIRE_MESSAGE(https.has_value(), describe(https));
        CHECK(https->host == "github.com");
        CHECK(https->token == "env-token");
        CHECK(https->calls == 0);
    }
    {
        const EnvGuard path("PATH", fs::path(NOTEAPP_FAKE_SSH_DIR));
        const auto unknown = noteapp::sync::prepare_credentials("git@unknown.example:kisi/n.git");
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().message_key == "sync.ssh_host_unknown");
        const auto known = noteapp::sync::prepare_credentials("git@known.example:kisi/n.git");
        REQUIRE_MESSAGE(known.has_value(), describe(known));
        CHECK_FALSE(known->token.has_value());
    }
    const TempDir local_remote;
    const auto local = noteapp::sync::prepare_credentials(utf8(local_remote.path()));
    REQUIRE_MESSAGE(local.has_value(), describe(local));
    CHECK_FALSE(local->token.has_value());
    // Windows'ta libgit2, var olmayan "C:/…" yolunu scp biçimi sayıp sistem ssh'ını başlatır (host "C"):
    // yerel remote ağa çıkmadan reddedilir.
    const auto missing = noteapp::sync::prepare_credentials(utf8(local_remote.path() / u8"olmayan uzak.git"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message_key == "sync.remote_invalid");

    const auto leaky = noteapp::sync::prepare_credentials("https://kisi:gizli-parola@github.com/kisi/n.git");
    REQUIRE_FALSE(leaky.has_value());
    CHECK(leaky.error().message_key == "sync.remote_has_credentials");
    CHECK_FALSE(mentions(leaky.error(), "gizli-parola"));
}

TEST_CASE("acceptance 7: the token never reaches files, git objects, the log, the status or an error") {
    const SyncFixture fx;
    const std::string marker = "ghp_noteappLeakMarker" + std::to_string(std::random_device{}());
    const EnvGuard env("NOTEAPP_GIT_TOKEN", fs::path(marker));

    // Yerel remote'ta libgit2 callback'i çağırmaz; kurulum + senkron yine de token ortamdayken koşar.
    const fs::path a = fx.root() / u8"A notları";
    write_file(a / u8"ilk not.md", "içerik\n");
    SyncService sync_a(SyncFixture::config_for(a));
    const auto enabled_a = sync_a.enable(utf8(fx.remote()));
    REQUIRE_MESSAGE(enabled_a.has_value(), describe(enabled_a));
    write_file(a / "iki.md", "2\n");
    const auto synced_a = sync_a.sync_now();
    REQUIRE_MESSAGE(synced_a.has_value(), describe(synced_a));
    const fs::path b = fx.root() / u8"B notları";
    SyncService sync_b(SyncFixture::config_for(b));
    const auto enabled_b = sync_b.enable(utf8(fx.remote()));
    REQUIRE_MESSAGE(enabled_b.has_value(), describe(enabled_b));
    const auto synced_b = sync_b.sync_now();
    REQUIRE_MESSAGE(synced_b.has_value(), describe(synced_b));

    // HTTPS yolu doğrudan sürülür: token ortamdan, callback bir kez verir, ikinci çağrı reddedilir.
    std::vector<Error> errors;
    auto credentials = noteapp::sync::prepare_credentials(kUrl);
    REQUIRE_MESSAGE(credentials.has_value(), describe(credentials));
    CHECK(credentials->token == marker);
    git_credential* given = nullptr;
    REQUIRE(noteapp::sync::acquire_credentials(&given, kUrl, GIT_CREDENTIAL_USERPASS_PLAINTEXT,
                                               *credentials) == 0);
    git_credential_free(given);
    CHECK(noteapp::sync::acquire_credentials(&given, kUrl, GIT_CREDENTIAL_USERPASS_PLAINTEXT, *credentials) <
          0);
    REQUIRE(credentials->error.has_value());
    errors.push_back(*credentials->error);
    // Token'ı URL'ye gömen kullanıcıya bile geri yansıtılmaz; senkron durum dosyası ve log da hatayı alır.
    const fs::path c = fx.root() / u8"C notları";
    write_file(c / "not.md", "c\n");
    SyncService sync_c(SyncFixture::config_for(c));
    const auto leaky = sync_c.enable("https://x-access-token:" + marker + "@github.com/kisi/notlar.git");
    REQUIRE_FALSE(leaky.has_value());
    errors.push_back(leaky.error());
    const auto not_initialized = sync_c.sync_now();  // durum dosyasına bir hata da yazılsın
    REQUIRE_FALSE(not_initialized.has_value());

    for (const Error& error : errors) {
        CAPTURE(error.message_key);
        CHECK_FALSE(mentions(error, marker));
    }
    CHECK(files_containing(fx.root(), marker).empty());  // notlar, .git, uzak depo, config, durum, log
    CHECK(fs::exists(noteapp::core::state_dir() / "log.txt"));
    CHECK(fs::exists(noteapp::core::state_dir() / "sync-status.toml"));
    for (const fs::path& repo : {a, b, fx.remote()}) {
        CAPTURE(utf8(repo));
        CHECK_FALSE(objects_contain(repo, marker));
    }
}

TEST_CASE("every T26 message key exists in the catalog") {
    noteapp::core::init_i18n("en");
    for (const char* key :
         {"sync.both_non_empty", "sync.remote_mismatch", "sync.auth_failed", "sync.token_missing",
          "sync.ssh_host_unknown", "sync.ssh_unavailable", "sync.keychain_unavailable",
          "sync.remote_has_credentials", "sync.remote_invalid", "sync.remote_branch_missing"})
        CHECK_MESSAGE(noteapp::core::tr(key) != key, key);
}
