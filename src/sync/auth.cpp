#include "sync/auth.hpp"

#include "core/i18n.hpp"
#include "core/process.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <git2.h>
#include <keychain/keychain.h>
#include <system_error>
#include <utility>

namespace noteapp::sync {

namespace {

// Kasa kaydı: servis `noteapp`, hesap = host (SPEC.md §8). Windows Credential Manager'da
// "noteapp.noteapp/<host>" adıyla görünür.
constexpr const char* kKeychainPackage = "noteapp";
constexpr const char* kKeychainService = "noteapp";
constexpr const char* kTokenEnv = "NOTEAPP_GIT_TOKEN";
// GitHub token'la gelen kullanıcı adına bakmaz; bu ad, token kimliklerinin yerleşik kullanıcı adıdır.
constexpr const char* kTokenUser = "x-access-token";
constexpr std::string_view kBlank = " \t\r\n";

core::Error sync_error(std::string key, core::Args args = {}) {
    return {core::Error::Code::Io, std::move(key), std::move(args)};
}

// Host adları büyük/küçük harfe duyarsızdır; kasa hesabı tek biçimde tutulur.
std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

core::Error keychain_failure(const keychain::Error& error) {
    return sync_error("sync.keychain_unavailable", {{"detail", error.message}});
}

}  // namespace

RemoteEndpoint parse_remote(std::string_view url) {
    RemoteEndpoint endpoint;
    std::string_view authority;
    if (const auto scheme_end = url.find("://"); scheme_end != std::string_view::npos) {
        const std::string scheme = lowercase(url.substr(0, scheme_end));
        if (scheme == "https")
            endpoint.auth = Auth::Https;
        else if (scheme == "ssh" || scheme == "git+ssh" || scheme == "ssh+git")
            endpoint.auth = Auth::Ssh;
        authority = url.substr(scheme_end + 3);
        authority = authority.substr(0, authority.find('/'));
    } else if (const auto colon = url.find(':');
               // Tek harf + ':' Windows sürücüsüdür (C:/…); ':'dan önce ayırıcı varsa yerel yoldur.
               colon != std::string_view::npos && colon > 1 &&
               url.substr(0, colon).find_first_of("/\\") == std::string_view::npos) {
        endpoint.auth = Auth::Ssh;  // scp biçimi: [kullanıcı@]host:yol
        authority = url.substr(0, colon);
    }
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        endpoint.userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }
    // "[::1]" gibi köşeli IPv6 adresindeki ':' port ayırıcısı değildir.
    if (const auto colon = authority.rfind(':');
        colon != std::string_view::npos && authority.find(']', colon) == std::string_view::npos) {
        endpoint.port = authority.substr(colon + 1);
        authority = authority.substr(0, colon);
    }
    endpoint.host = lowercase(authority);
    return endpoint;
}

std::string without_userinfo(std::string url) {
    const RemoteEndpoint endpoint = parse_remote(url);
    if (endpoint.auth == Auth::Ssh || endpoint.userinfo.empty())
        return url;
    // ssh dışında kullanıcı bilgisi yalnız "://"den hemen sonra gelir (scp biçimi hep ssh'tır).
    return url.erase(url.find("://") + 3, endpoint.userinfo.size() + 1);
}

std::expected<RemoteEndpoint, core::Error> validate_remote(std::string_view url) {
    const RemoteEndpoint endpoint = parse_remote(url);
    // Önce bu: adres, içindeki kimlikle birlikte hata argümanına (oradan log/durum dosyasına) girmesin.
    if (endpoint.auth != Auth::Ssh && !endpoint.userinfo.empty())
        return std::unexpected(sync_error("sync.remote_has_credentials"));
    // '-' ile başlayan host, ssh'a seçenek olarak geçebilirdi (-oProxyCommand=…). Şemasız yerel yol var olan
    // bir klasör olmalı: Windows'ta libgit2 var olmayan "C:/…" yolunu scp biçimi sayıp ön kontrolsüz ssh
    // başlatır (host "C").
    std::error_code ec;
    const bool missing_local =
        endpoint.auth == Auth::None && url.find("://") == std::string_view::npos &&
        !std::filesystem::is_directory(std::filesystem::path(std::u8string(url.begin(), url.end())), ec);
    if (url.empty() || missing_local ||
        (endpoint.auth != Auth::None && (endpoint.host.empty() || endpoint.host.starts_with('-'))))
        return std::unexpected(sync_error("sync.remote_invalid", {{"remote", std::string(url)}}));
    return endpoint;
}

std::expected<std::optional<std::string>, core::Error> read_stored_token(std::string_view host) {
    keychain::Error error;
    std::string token = keychain::getPassword(kKeychainPackage, kKeychainService, lowercase(host), error);
    if (error.type == keychain::ErrorType::NotFound)
        return std::nullopt;
    if (error)
        return std::unexpected(keychain_failure(error));
    if (token.empty())
        return std::nullopt;
    return token;
}

std::expected<std::string, core::Error> resolve_token(std::string_view host, const TokenReader& stored) {
    if (auto token = core::get_env_utf8(kTokenEnv))
        return std::move(*token);
    auto token = stored(host);
    if (!token)
        return std::unexpected(token.error());
    if (!*token)
        return std::unexpected(sync_error("sync.token_missing", {{"host", std::string(host)}}));
    return std::move(**token);
}

std::expected<void, core::Error> store_token(std::string_view host, std::string_view token) {
    // Yapıştırılan token çoğu kez satır sonu taşır; token'larda boşluk yoktur.
    const auto first = token.find_first_not_of(kBlank);
    if (first == std::string_view::npos)
        return std::unexpected(sync_error("sync.token_missing", {{"host", std::string(host)}}));
    token = token.substr(first, token.find_last_not_of(kBlank) - first + 1);
    keychain::Error error;
    keychain::setPassword(kKeychainPackage, kKeychainService, lowercase(host), std::string(token), error);
    if (error)
        return std::unexpected(keychain_failure(error));
    return {};
}

std::expected<void, core::Error> clear_token(std::string_view host) {
    keychain::Error error;
    keychain::deletePassword(kKeychainPackage, kKeychainService, lowercase(host), error);
    if (error && error.type != keychain::ErrorType::NotFound)
        return std::unexpected(keychain_failure(error));
    return {};
}

bool has_token(std::string_view host) {
    const auto token = read_stored_token(host);
    return token && token->has_value();
}

std::expected<void, core::Error> check_ssh_host_known(const RemoteEndpoint& endpoint) {
    // known_hosts varsayılan dışı portu "[host]:port" diye kaydeder.
    const std::string name = endpoint.port.empty() || endpoint.port == "22"
                                 ? endpoint.host
                                 : std::format("[{}]:{}", endpoint.host, endpoint.port);
    // ssh-keygen bulduğu satırı (açık host anahtarı) stdout'a yazar: TUI ekranını bozmasın diye çıktı atılır.
    // ponytail: ~/.ssh/config takma adları (Host … HostName) görülmez.
    const auto exit_code = core::run_and_wait({"ssh-keygen", "-F", name}, {}, true);
    if (!exit_code) {
        if (exit_code.error().message_key == "error.process_not_found")
            return std::unexpected(sync_error("sync.ssh_unavailable"));
        return std::unexpected(exit_code.error());
    }
    if (*exit_code != 0)
        return std::unexpected(sync_error("sync.ssh_host_unknown", {{"host", endpoint.host}}));
    return {};
}

std::expected<Credentials, core::Error> prepare_credentials(std::string_view url) {
    const auto endpoint = validate_remote(url);
    if (!endpoint)
        return std::unexpected(endpoint.error());
    Credentials credentials;
    credentials.host = endpoint->host;
    if (endpoint->auth == Auth::Ssh) {
        if (auto known = check_ssh_host_known(*endpoint); !known)
            return std::unexpected(known.error());
    } else if (endpoint->auth == Auth::Https) {
        auto token = resolve_token(endpoint->host);
        if (!token)
            return std::unexpected(token.error());
        credentials.token = std::move(*token);
    }
    return credentials;
}

int acquire_credentials(git_credential** out, const char* url, unsigned int allowed_types,
                        Credentials& credentials) {
    if (!credentials.token)
        return GIT_PASSTHROUGH;
    const RemoteEndpoint target = parse_remote(url != nullptr ? url : "");
    // İkinci çağrı = sunucu ilk kimliği reddetti; aynı token'ı yeniden vermek sonsuz döngüdür. Token yalnız
    // remote'un kendi host'una, https ile ve kullanıcı/parola olarak gider (yönlendirme, düz http'ye düşme).
    if (++credentials.calls > 1 || target.auth != Auth::Https || target.host != credentials.host ||
        (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) == 0) {
        credentials.error = sync_error("sync.auth_failed", {{"host", credentials.host}});
        return GIT_EAUTH;
    }
    return git_credential_userpass_plaintext_new(out, kTokenUser, credentials.token->c_str());
}

}  // namespace noteapp::sync
