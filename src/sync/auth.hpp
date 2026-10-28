#pragma once

#include "core/error.hpp"

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct git_credential;

namespace noteapp::sync {

// Kimlik doğrulama türü; notlar reposunun (izole) config'inden okunan remote URL'sinden türetilir:
// kullanıcının url.*.insteadOf ayarı uygulanmaz (SPEC-sync "Uygulama kararları" 2).
enum class Auth { None, Https, Ssh };

struct RemoteEndpoint {
    Auth auth = Auth::None;
    std::string host;      // küçük harfli; yerel remote'ta boş. Kasa hesabı bu addır.
    std::string port;      // URL'de yazılıysa
    std::string userinfo;  // "kullanıcı[:parola]@" kısmı; https'te kimlik sızıntısı demektir, reddedilir
};

// https:// → Https; ssh://, git+ssh://, ssh+git:// ve scp biçimi [kullanıcı@]host:yol → Ssh; yerel yol,
// file://, http:// (token düz metin gitmesin) ve diğerleri → None.
RemoteEndpoint parse_remote(std::string_view url);

// Elle yazılmış adreste kimlik olabilir: gösterilen/loglanan adresten (ssh dışında) "kullanıcı[:parola]@"
// çıkarılır. ssh'ta kullanıcı adı (git@) sır değildir, olduğu gibi kalır.
std::string without_userinfo(std::string url);

// Remote adresi sınırda bir kez doğrulanır: ssh dışında URL'ye gömülü kullanıcı bilgisi (token sızıntısı)
// "sync.remote_has_credentials" — adres hata argümanına bile girmez; boş adres, boş/'-' ile başlayan host
// (ssh'a seçenek enjeksiyonu) ya da var olmayan yerel klasör "sync.remote_invalid" {remote}.
std::expected<RemoteEndpoint, core::Error> validate_remote(std::string_view url);

// Kasa okuması: kayıt yoksa nullopt; kasa yoksa ya da erişilemiyorsa "sync.keychain_unavailable".
using TokenReader =
    std::function<std::expected<std::optional<std::string>, core::Error>(std::string_view host)>;
std::expected<std::optional<std::string>, core::Error> read_stored_token(std::string_view host);

// Token sırası: NOTEAPP_GIT_TOKEN (kalıcı saklanmaz) → kasa (`stored`; testte enjekte edilir). İkisi de
// yoksa "sync.token_missing" {host}. Hata hiçbir zaman token'ı taşımaz.
std::expected<std::string, core::Error> resolve_token(std::string_view host,
                                                      const TokenReader& stored = read_stored_token);

// OS kasası (servis `noteapp`, hesap = host). Düz dosyaya asla düşülmez: kasa yoksa (Linux'ta Secret
// Service) "sync.keychain_unavailable". Baştaki/sondaki boşluk atılır; boş token "sync.token_missing".
std::expected<void, core::Error> store_token(std::string_view host, std::string_view token);
std::expected<void, core::Error> clear_token(std::string_view host);  // kayıt yoksa da başarılı
bool has_token(std::string_view host);                                // kasa hatasında false

// SSH ağ işleminden önce `ssh-keygen -F <host>` (port yazılıysa "[host]:port"): Win32-OpenSSH bilinmeyen
// host anahtarında onay bekleyip asılı kalır (SPEC-sync karar 3). Kayıt yoksa "sync.ssh_host_unknown"
// {host}; ssh-keygen bulunamazsa "sync.ssh_unavailable".
std::expected<void, core::Error> check_ssh_host_known(const RemoteEndpoint& endpoint);

// Bir ağ işleminin (ls, fetch, push) kimlik durumu; libgit2 credential callback'inin yükü. Her işlem
// kendi kopyasıyla başlar: tek deneme kuralı işlem başınadır.
struct Credentials {
    std::string host;                  // kimlik yalnız bu host'a verilir
    std::optional<std::string> token;  // yalnız Https
    int calls = 0;
    std::optional<core::Error> error;  // callback'in reddetme nedeni; işlem hatası bununla çevrilir
};

// Ağa çıkmadan, hiçbir şeye dokunmadan: Https → token (yoksa sync.token_missing); Ssh → host anahtarı
// ön kontrolü; None → boş kimlik. URL'de kullanıcı bilgisi olan https remote "sync.remote_has_credentials".
std::expected<Credentials, core::Error> prepare_credentials(std::string_view url);

// libgit2 credential callback'inin gövdesi: ilk çağrıda userpass("x-access-token", token); ikinci çağrı
// (sunucu reddetti), başka bir host, https olmayan URL ya da userpass'a izin vermeyen istek →
// credentials.error = "sync.auth_failed" {host} ve < 0 (sonsuz deneme yok). Token yoksa GIT_PASSTHROUGH.
int acquire_credentials(git_credential** out, const char* url, unsigned int allowed_types,
                        Credentials& credentials);

}  // namespace noteapp::sync
