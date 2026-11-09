#include "net/http.hpp"
#include "net/timeout.hpp"

#include "core/i18n.hpp"
#include "core/version.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdint>
#include <doctest/doctest.h>
#include <algorithm>
#include <expected>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include "core/process.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <unistd.h>
#endif

using namespace noteapp;
using namespace std::chrono_literals;

namespace {

// Takılan sunucu testi kilitlemesin: her istek kısa sürelerle sınırlı (üretimin 30/900 sn'si değil). Sıfır süreyi
// sınayan test bunları ezer ve iptalle biter; son güvence tests/net/CMakeLists.txt'teki ctest TIMEOUT'u.
constexpr std::chrono::seconds kTestConnectTimeout{10};
constexpr std::chrono::seconds kTestTotalTimeout{20};

net::HttpRequest request_for(std::string url, core::Limit max_bytes = {}) {
    net::HttpRequest request;
    request.url = std::move(url);
    request.max_bytes = std::move(max_bytes);
    request.connect_timeout = kTestConnectTimeout;
    request.total_timeout = kTestTotalTimeout;
    return request;
}

std::string error_key(const std::expected<net::HttpResponse, core::Error>& result) {
    return result ? std::string("<ok>") : result.error().message_key;
}

std::string arg(const core::Error& error, const std::string& name) {
    for (const auto& [key, value] : error.args) {
        if (key == name)
            return value;
    }
    return {};
}

}  // namespace

// Ağa çıkmayan kurallar: Wine dahil her platformda koşar.

TEST_CASE("http and non-https addresses are refused before any connection, naming only scheme and host") {
    for (const std::string url : {"http://127.0.0.1:9/ok?q=1", "ftp://127.0.0.1:9/ok"}) {
        const auto result = net::http_get(request_for(url), {});
        REQUIRE_MESSAGE(error_key(result) == "net.https_only", url << " -> " << error_key(result));
        CHECK(arg(result.error(), "url") == url.substr(0, url.find(':')) + "://127.0.0.1");
    }
    for (const std::string url : {"not a url", "https://"}) {
        const auto result = net::http_get(request_for(url), {});
        CHECK_MESSAGE(error_key(result) == "net.invalid_url", url << " -> " << error_key(result));
    }
}

namespace {

bool mentions(const core::Error& error, std::string_view text) {
    return std::ranges::any_of(error.args,
                               [text](const auto& entry) { return entry.second.find(text) != std::string::npos; });
}

}  // namespace

TEST_CASE("an address with a user name or password is refused and the password is not echoed") {
    const auto result = net::http_get(request_for("https://user:hunter2@127.0.0.1:9/ok"), {});
    REQUIRE(error_key(result) == "net.https_only");
    CHECK_FALSE(mentions(result.error(), "hunter2"));
    CHECK(arg(result.error(), "url") == "https://127.0.0.1");
}

TEST_CASE("an unparsable address with credentials is not echoed at all") {
    const auto result = net::http_get(request_for("https://u:s3cret@bad host/"), {});
    REQUIRE(error_key(result) == "net.invalid_url");
    CHECK_FALSE(mentions(result.error(), "s3cret"));
    CHECK(result.error().args.empty());
}

TEST_CASE("a stop requested before the call returns cancelled without connecting") {
    std::stop_source stop;
    stop.request_stop();
    const auto result = net::http_get(request_for("https://127.0.0.1:9/ok"), stop.get_token());
    CHECK(error_key(result) == "net.cancelled");
}

TEST_CASE("headers outside the allowlist and line breaks are programming errors") {
    for (const auto& header : std::vector<std::pair<std::string, std::string>>{
             {"Authorization", "token x"}, {"cookie", "a=b"}, {"Proxy-Authorization", "x"}, {"Host", "evil"},
             {"X-Test", "x"}, {"Accept", "a\r\nInjected: 1"}, {"Accept:", "x"}, {"", "x"}}) {
        CAPTURE(header.first);
        net::HttpRequest request = request_for("https://127.0.0.1:9/ok");
        request.headers.push_back(header);
        CHECK_THROWS_AS(static_cast<void>(net::http_get(request, {})), std::invalid_argument);
    }
}

TEST_CASE("a zero connect timeout means unlimited, not curl's 300 s default; zero total stays zero") {
    constexpr long kLongMax = std::numeric_limits<long>::max();
    CHECK(net::detail::curl_connect_timeout(0s) == kLongMax);
    CHECK(net::detail::curl_connect_timeout(30s) == 30);
    CHECK(net::detail::curl_total_timeout(0s) == 0);  // CURLOPT_TIMEOUT=0 curl'de sınırsızdır
    CHECK(net::detail::curl_total_timeout(900s) == 900);
    // long'a sığmayan süre (Windows'ta long 32 bit) taşmaz, en büyük long'a kırpılır.
    const std::chrono::seconds huge{std::numeric_limits<std::chrono::seconds::rep>::max()};
    CHECK(net::detail::curl_connect_timeout(huge) == kLongMax);
    CHECK(net::detail::curl_total_timeout(huge) == kLongMax);
    CHECK_THROWS_AS(static_cast<void>(net::detail::curl_connect_timeout(-1s)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(net::detail::curl_total_timeout(-1s)), std::invalid_argument);
}

TEST_CASE("every net error key is in the en and tr catalogs") {
    for (const char* language : {"en", "tr"}) {
        core::init_i18n(language);
        for (const char* key : {"net.invalid_url", "net.https_only", "net.cancelled", "net.too_large", "net.timeout",
                                "net.tls_failed", "net.failed"}) {
            CAPTURE(language);
            CAPTURE(key);
            CHECK(core::tr(key) != key);
        }
    }
    core::init_i18n("en");
}

// Yerel HTTPS sunucusuyla uçtan uca testler yalnız Linux'ta. Windows'ta (Wine) curl Schannel kullanır: güven
// deposu Windows sertifika deposudur, test CA'sı SSL_CERT_FILE ile verilemez; deponun içine yazmak test
// önekinin kayıt defterini değiştirmek demektir. Sunucu da Python ister, çapraz derleme imajında yoktur.
// Aynı kod yolu (curl seçenekleri, geri çağrılar, hata eşlemesi) Linux'ta sınanır; Windows'a özgü yalnız
// vekil okuma ve Schannel iptal ayarıdır.
#ifndef _WIN32

namespace {

namespace fs = std::filesystem;

// Süreç boyunca tek sunucu (https_server.py): anahtar üretimi ve Python açılışı her testte tekrarlanmaz.
class HttpsServer {
public:
    HttpsServer() {
        const auto python = core::find_program("python3");
        REQUIRE_MESSAGE(python.has_value(), "python3 is required for the net tests (docker/fedora.Containerfile)");
        // Sunucu ayrık (setsid) başlar: bu sürecin pid'ini yoklar, test süreci ölünce (öldürülse de) kendisi çıkar.
        auto spawned = core::spawn_detached({test::utf8(*python), NOTEAPP_NET_TEST_SERVER, test::utf8(dir_.path()),
                                             std::to_string(getpid())});
        REQUIRE_MESSAGE(spawned.has_value(), "could not start the HTTPS test server");
        process_.emplace(std::move(*spawned));
        // Kurucu yarıda kalırsa yıkıcı koşmaz: sunucu burada öldürülür, hata aynen yayılır.
        try {
            wait_until_ready();
        } catch (...) {
            core::terminate(*process_);
            throw;
        }
    }
    ~HttpsServer() {
        if (process_)
            core::terminate(*process_);
    }
    HttpsServer(const HttpsServer&) = delete;
    HttpsServer& operator=(const HttpsServer&) = delete;

    std::string url(const std::string& path) const {
        return "https://127.0.0.1:" + std::to_string(trusted_port_) + path;
    }
    std::string untrusted_url(const std::string& path) const {
        return "https://127.0.0.1:" + std::to_string(untrusted_port_) + path;
    }
    // TCP bağlantısını kabul eder, TLS el sıkışmasına hiç cevap vermez.
    std::string silent_url(const std::string& path) const {
        return "https://127.0.0.1:" + std::to_string(silent_port_) + path;
    }
    // Test CA'sının imzaladığı, ama adı wrong.example olan sertifika (ad doğrulaması).
    std::string wrong_host_url(const std::string& path) const {
        return "https://127.0.0.1:" + std::to_string(wrong_host_port_) + path;
    }
    fs::path ca_file() const { return dir_.path() / "ca.pem"; }
    fs::path empty_ca_dir() const { return dir_.path() / "no-ca-dir"; }

private:
    void wait_until_ready() {
        const fs::path ready = dir_.path() / "ready.txt";
        for (auto waited = 0ms; !fs::exists(ready); waited += kPoll) {
            const auto exited = core::wait_with_timeout(*process_, kPoll);
            REQUIRE_MESSAGE((exited && !exited->has_value()),
                            "HTTPS test server exited: " << test::slurp(dir_.path() / "server.log"));
            REQUIRE_MESSAGE(waited < kStartTimeout, "HTTPS test server did not become ready");
        }
        std::ifstream(ready) >> trusted_port_ >> untrusted_port_ >> silent_port_ >> wrong_host_port_;
        REQUIRE((trusted_port_ > 0 && untrusted_port_ > 0 && silent_port_ > 0 && wrong_host_port_ > 0));
        fs::create_directories(empty_ca_dir());
    }

    static constexpr auto kPoll = 50ms;
    static constexpr auto kStartTimeout = 30s;
    test::TempDir dir_;
    std::optional<core::ProcessHandle> process_;
    int trusted_port_ = 0;
    int untrusted_port_ = 0;
    int silent_port_ = 0;
    int wrong_host_port_ = 0;
};

const HttpsServer& server() {
    static const HttpsServer instance;
    return instance;
}

// OpenSSL'in varsayılan güveni (CURL_CA_FALLBACK) yalnız test CA'sı olur; vekil ortamı yerel sunucuyu
// saptırmasın.
struct TrustTestCa {
    test::ScopedEnv cert_file{"SSL_CERT_FILE", test::utf8(server().ca_file())};
    test::ScopedEnv cert_dir{"SSL_CERT_DIR", test::utf8(server().empty_ca_dir())};
    test::ScopedEnv https_proxy{"https_proxy", std::string()};
    test::ScopedEnv https_proxy_upper{"HTTPS_PROXY", std::string()};
    test::ScopedEnv all_proxy{"all_proxy", std::string()};
    test::ScopedEnv all_proxy_upper{"ALL_PROXY", std::string()};
};

constexpr std::uint64_t kBigBodyBytes = 1000;  // https_server.py BIG_BODY_BYTES

core::Limit bytes(std::uint64_t value) {
    return core::Limit{value, std::to_string(value) + "B"};
}

}  // namespace

TEST_CASE("a server signed by the trusted CA returns its body, status and rate-limit headers") {
    const TrustTestCa trust;
    net::HttpRequest request = request_for(server().url("/ok"));
    request.headers.emplace_back("Accept", "application/vnd.github+json");
    const auto result = net::http_get(request, {});
    REQUIRE_MESSAGE(result.has_value(), error_key(result) << " " << (result ? "" : arg(result.error(), "detail")));
    CHECK(result->status == 200);
    CHECK(result->body == "hello noteapp/" + std::string(core::version()) + " application/vnd.github+json agents=1");
    CHECK(result->ratelimit_remaining == 42);
    CHECK(result->ratelimit_reset == 1700000000);
    CHECK(result->retry_after == 7);
}

TEST_CASE("a caller's User-Agent replaces the default one instead of adding a second") {
    const TrustTestCa trust;
    net::HttpRequest request = request_for(server().url("/ok"));
    request.headers.emplace_back("user-agent", "custom/1");
    const auto result = net::http_get(request, {});
    REQUIRE(result.has_value());
    CHECK(result->body == "hello custom/1 */* agents=1");  // */*: Accept verilmeyince curl'ün varsayılanı
}

TEST_CASE("a followed redirect reports the final response's status and rate-limit headers") {
    const TrustTestCa trust;
    const auto result = net::http_get(request_for(server().url("/redirect-ok")), {});
    REQUIRE(result.has_value());
    CHECK(result->status == 200);
    CHECK(result->ratelimit_remaining == 42);  // 302'nin 1/111/99'u değil, /ok'un değerleri
    CHECK(result->ratelimit_reset == 1700000000);
    CHECK(result->retry_after == 7);
}

TEST_CASE("exactly five redirects are followed, the sixth is refused") {
    const TrustTestCa trust;
    const auto five = net::http_get(request_for(server().url("/hops/5")), {});
    REQUIRE_MESSAGE(five.has_value(), error_key(five));
    CHECK(five->body == "arrived");
    CHECK(error_key(net::http_get(request_for(server().url("/hops/6")), {})) == "net.failed");
}

TEST_CASE("a non-2xx status is a response, not an error") {
    const TrustTestCa trust;
    const auto result = net::http_get(request_for(server().url("/limited")), {});
    REQUIRE(result.has_value());
    CHECK(result->status == 403);
    CHECK(result->ratelimit_remaining == 0);
    CHECK(result->ratelimit_reset == 1700000123);
    CHECK_FALSE(result->retry_after.has_value());
}

TEST_CASE("malformed, overflowing and past rate-limit headers are absent, not guessed") {
    const TrustTestCa trust;
    const auto result = net::http_get(request_for(server().url("/bad-headers")), {});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->ratelimit_remaining.has_value());
    CHECK_FALSE(result->ratelimit_reset.has_value());
    CHECK_FALSE(result->retry_after.has_value());
}

TEST_CASE("a certificate the trust store does not know is refused") {
    const TrustTestCa trust;
    const auto result = net::http_get(request_for(server().untrusted_url("/ok")), {});
    CHECK(error_key(result) == "net.tls_failed");
}

TEST_CASE("a certificate from the trusted CA for another host name is refused") {
    const TrustTestCa trust;
    const auto result = net::http_get(request_for(server().wrong_host_url("/ok")), {});
    CHECK(error_key(result) == "net.tls_failed");
}

TEST_CASE("without the test CA the trusted server is refused too (the CA really decides)") {
    const TrustTestCa trust;
    test::ScopedEnv no_file("SSL_CERT_FILE", test::utf8(server().empty_ca_dir() / "missing.pem"));
    const auto result = net::http_get(request_for(server().url("/ok")), {});
    CHECK(error_key(result) == "net.tls_failed");
}

TEST_CASE("a redirect to http is refused") {
    const TrustTestCa trust;
    const auto result = net::http_get(request_for(server().url("/redirect-http")), {});
    CHECK(error_key(result) == "net.https_only");
}

TEST_CASE("a redirect loop stops at the redirect limit") {
    const TrustTestCa trust;
    const auto result = net::http_get(request_for(server().url("/redirect-loop")), {});
    CHECK(error_key(result) == "net.failed");
}

TEST_CASE("max_bytes cuts a body that exceeds it, with or without Content-Length") {
    const TrustTestCa trust;
    for (const std::string path : {"/big-sized", "/big-unsized"}) {
        CAPTURE(path);
        const auto over = net::http_get(request_for(server().url(path), bytes(kBigBodyBytes - 1)), {});
        REQUIRE(error_key(over) == "net.too_large");
        CHECK(arg(over.error(), "limit") == "999B");

        const auto exact = net::http_get(request_for(server().url(path), bytes(kBigBodyBytes)), {});
        REQUIRE_MESSAGE(exact.has_value(), path << " -> " << error_key(exact));
        CHECK(exact->body.size() == kBigBodyBytes);

        const auto unlimited = net::http_get(request_for(server().url(path), bytes(0)), {});
        REQUIRE(unlimited.has_value());
        CHECK(unlimited->body.size() == kBigBodyBytes);
    }
}

TEST_CASE("a stop request ends a stalled download within two seconds") {
    const TrustTestCa trust;
    std::stop_source stop;
    std::jthread stopper([&stop] {
        std::this_thread::sleep_for(300ms);
        stop.request_stop();
    });
    const auto start = std::chrono::steady_clock::now();
    const auto result = net::http_get(request_for(server().url("/stall")), stop.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(error_key(result) == "net.cancelled");
    if (test::kWallClockBudgets)
        CHECK(elapsed <= 2s);
}

TEST_CASE("total_timeout ends a stalled download") {
    const TrustTestCa trust;
    net::HttpRequest request = request_for(server().url("/stall"));
    request.total_timeout = 1s;
    const auto result = net::http_get(request, {});
    CHECK(error_key(result) == "net.timeout");
}

TEST_CASE("connect_timeout ends a TLS handshake the server never answers") {
    const TrustTestCa trust;
    net::HttpRequest request = request_for(server().silent_url("/ok"));
    request.connect_timeout = 1s;
    const auto result = net::http_get(request, {});
    CHECK(error_key(result) == "net.timeout");
}

// 0 = sınırsız: ne bağlantı ne toplam süre kesmez, aktarımı yalnız iptal bitirir. (curl'ün 300 sn'lik bağlantı
// varsayılanına düşmediğini eşleme testi gösterir; 300 sn test süresinde beklenmez.)
TEST_CASE("zero timeouts never end an unanswered handshake or a stalled body; only stop does") {
    const TrustTestCa trust;
    for (const std::string& url : {server().silent_url("/ok"), server().url("/stall")}) {
        net::HttpRequest request = request_for(url);
        request.connect_timeout = 0s;
        request.total_timeout = 0s;
        std::stop_source stop;
        // start iş parçacığından önce alınır: alt sınır (>= 2,5 sn) iş parçacığının açılışına bağlı kalmasın.
        const auto start = std::chrono::steady_clock::now();
        std::jthread stopper([&stop] {
            std::this_thread::sleep_for(2500ms);
            stop.request_stop();
        });
        const auto result = net::http_get(request, stop.get_token());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK_MESSAGE(error_key(result) == "net.cancelled", url << " -> " << error_key(result));
        CHECK(elapsed >= 2500ms);
        // İptal el sıkışma evresinde de hemen görülür (curl'ün ilerleme geri çağrısı orada çağrılmaz).
        if (test::kWallClockBudgets)
            CHECK(elapsed <= 2500ms + 2s);
    }
}

#endif
