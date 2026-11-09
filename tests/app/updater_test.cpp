// app::updater: GitHub'daki son sürümün denetimi ve doğrulanmış indirme. HTTP sahte HttpGet ile verilir;
// testler ağa çıkmaz.
#include "app/updater.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "core/version.hpp"
#include "net/http.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using noteapp::app::AssetKind;
using noteapp::app::check_latest;
using noteapp::app::download_verified;
using noteapp::app::HttpGet;
using noteapp::app::ReleaseInfo;
using noteapp::app::should_check_on_startup;
using noteapp::app::UpdateCheck;
using noteapp::app::UpdateSettings;
using noteapp::app::UpdateState;
using noteapp::app::UpToDate;
using noteapp::core::Error;
using noteapp::core::Limit;
using noteapp::net::HttpRequest;
using noteapp::net::HttpResponse;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kApiUrl = "https://api.github.com/repos/ismetdyn/noteapp/releases/latest";
constexpr std::string_view kDownloadPrefix = "https://github.com/ismetdyn/noteapp/releases/download/";
// FIPS 180-2 örneği: SHA-256("abc").
constexpr std::string_view kPayload = "abc";
constexpr std::string_view kPayloadSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::string download_url(std::string_view tag, std::string_view name) {
    return std::string(kDownloadPrefix) + std::string(tag) + "/" + std::string(name);
}

struct Asset {
    std::string name;
    std::string url;
    std::uint64_t size = 3;
};

std::vector<Asset> assets_for(std::string_view tag, std::string_view version) {
    const std::string v(version);
    std::vector<Asset> out;
    for (const std::string& name : {"noteapp-" + v + "-x86_64.AppImage",
                                    "noteapp-" + v + "-windows-x86_64.zip", std::string("SHA256SUMS")})
        out.push_back({name, download_url(tag, name)});
    return out;
}

std::string release_json(std::string_view tag, const std::vector<Asset>& assets) {
    std::string json =
        R"({"tag_name":")" + std::string(tag) + R"(","name":"r","author":{"login":"x"},"assets":[)";
    for (std::size_t i = 0; i < assets.size(); ++i) {
        json += (i == 0 ? "" : ",");
        json += R"({"name":")" + assets[i].name + R"(","size":)" + std::to_string(assets[i].size) +
                R"(,"uploader":{"login":"x"},"browser_download_url":")" + assets[i].url + R"("})";
    }
    return json + "]}";
}

std::string release_json(std::string_view tag, std::string_view version) {
    return release_json(tag, assets_for(tag, version));
}

// HttpResponse'un isteğe bağlı başlıkları; verilmeyen başlık cevapta yok demektir.
struct Headers {
    std::optional<long> remaining;
    std::optional<std::int64_t> reset;
    std::optional<std::int64_t> retry_after;
};

HttpResponse reply(long status, std::string body = {}, const Headers& headers = {}) {
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    response.ratelimit_remaining = headers.remaining;
    response.ratelimit_reset = headers.reset;
    response.retry_after = headers.retry_after;
    return response;
}

Headers exhausted_until(std::int64_t reset) {
    Headers headers;
    headers.remaining = 0;
    headers.reset = reset;
    return headers;
}

HttpResponse ok(std::string body) {
    return reply(200, std::move(body));
}

// URL -> cevap; istenen her istek kaydedilir. Bilinmeyen URL 404'tür.
struct FakeHttp {
    std::map<std::string, std::expected<HttpResponse, Error>> responses;
    std::vector<HttpRequest> requests;

    HttpGet get() {
        return [this](const HttpRequest& request, std::stop_token) -> std::expected<HttpResponse, Error> {
            requests.push_back(request);
            const auto found = responses.find(request.url);
            if (found == responses.end())
                return reply(404);
            return found->second;
        };
    }
};

UpdateSettings settings(std::string current = "1.2.3") {
    UpdateSettings s;
    s.limits = {.api_bytes = {1024 * 1024, "1MiB"},
                .sums_bytes = {64 * 1024, "64KiB"},
                .download_bytes = {512 * 1024 * 1024, "512MiB"},
                .connect_timeout = {30000, "30s"},
                .transfer_timeout = {900000, "15min"},
                .install_timeout = {600000, "10min"},
                .tool_output_bytes = {1024 * 1024, "1MiB"}};
    s.current_version = std::move(current);
    return s;
}

// Varsayılan: AppImage kurulumu soruyor.
UpdateCheck latest(FakeHttp& http, const UpdateSettings& s = settings(),
                   AssetKind kind = AssetKind::AppImage) {
    return check_latest(http.get(), s, kind, {});
}

std::string error_key(const auto& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error().message_key;
}

std::string arg(const Error& error, std::string_view name) {
    for (const auto& [key, value] : error.args)
        if (key == name)
            return value;
    return "<none>";
}

ReleaseInfo available(FakeHttp& http, const UpdateSettings& s = settings()) {
    const auto result = latest(http, s);
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<ReleaseInfo>(*result));
    return std::get<ReleaseInfo>(*result);
}

// 1.3.0 release'i ve verilen SHA256SUMS ile hazır sahte sunucu.
FakeHttp release_with_sums(std::string sums, std::string payload = std::string(kPayload)) {
    FakeHttp http;
    http.responses.emplace(kApiUrl, ok(release_json("v1.3.0", "1.3.0")));
    http.responses.emplace(download_url("v1.3.0", "SHA256SUMS"), ok(std::move(sums)));
    http.responses.emplace(download_url("v1.3.0", "noteapp-1.3.0-x86_64.AppImage"), ok(std::move(payload)));
    return http;
}

std::string sums_line(std::string_view hash, std::string_view name) {
    return std::string(hash) + "  " + std::string(name) + "\n";
}

const std::string kOtherHash(64, '0');

}  // namespace

TEST_CASE("updater: a strictly newer tag is available with the assets of that release") {
    FakeHttp http;
    http.responses.emplace(kApiUrl, ok(release_json("v1.3.0", "1.3.0")));
    const ReleaseInfo release = available(http);

    CHECK(release.version == "1.3.0");
    CHECK(release.tag == "v1.3.0");
    CHECK(release.appimage.name == "noteapp-1.3.0-x86_64.AppImage");
    CHECK(release.appimage.url == download_url("v1.3.0", "noteapp-1.3.0-x86_64.AppImage"));
    CHECK(release.appimage.size == 3U);
    CHECK(release.sums.url == download_url("v1.3.0", "SHA256SUMS"));

    REQUIRE(http.requests.size() == 1);
    const HttpRequest& request = http.requests.front();
    CHECK(request.url == kApiUrl);
    CHECK(std::ranges::find(request.headers,
                            std::pair<std::string, std::string>{"Accept", "application/vnd.github+json"}) !=
          request.headers.end());
    CHECK(request.max_bytes == Limit{1024 * 1024, "1MiB"});
    CHECK(request.connect_timeout == std::chrono::seconds(30));
    CHECK(request.total_timeout == std::chrono::seconds(900));

    const auto windows = latest(http, settings(), AssetKind::WindowsZip);
    REQUIRE(windows.has_value());
    CHECK(std::get<ReleaseInfo>(*windows).windows_zip.name == "noteapp-1.3.0-windows-x86_64.zip");
    CHECK(std::get<ReleaseInfo>(*windows).asset(AssetKind::WindowsZip).url ==
          download_url("v1.3.0", "noteapp-1.3.0-windows-x86_64.zip"));
}

TEST_CASE("updater: versions compare numerically and a tag without v is accepted") {
    FakeHttp http;
    http.responses.emplace(kApiUrl, ok(release_json("10.0.0", "10.0.0")));
    CHECK(available(http, settings("9.99.99")).tag == "10.0.0");
}

TEST_CASE("updater: an equal or older release is up to date, never a downgrade") {
    for (const auto& [tag, version] : std::vector<std::pair<std::string, std::string>>{
             {"v1.2.3", "1.2.3"}, {"v1.2.2", "1.2.2"}, {"v1.1.9", "1.1.9"}, {"v0.9.9", "0.9.9"}}) {
        CAPTURE(tag);
        FakeHttp http;
        http.responses.emplace(kApiUrl, ok(release_json(tag, version)));
        const auto result = latest(http, settings("1.2.3"));
        REQUIRE(result.has_value());
        REQUIRE(std::holds_alternative<UpToDate>(*result));
        CHECK(std::get<UpToDate>(*result).latest_version == version);
    }
}

TEST_CASE("updater: the running version is the built version unless a test overrides it") {
    CHECK(UpdateSettings{}.current_version == noteapp::core::version());
}

TEST_CASE("updater: a tag that is not exactly v?X.Y.Z is rejected") {
    for (const std::string tag : {"v1.3", "v1.3.0-rc1", "V1.3.0", " v1.3.0", "v1.3.0 ", "v1.3.0.1", "vv1.3.0",
                                  "v1..0", "v+1.3.0", "v1.3.x", "", "v99999999999999999999.0.0"}) {
        CAPTURE(tag);
        FakeHttp http;
        http.responses.emplace(kApiUrl, ok(release_json(tag, "1.3.0")));
        CHECK(error_key(latest(http)) == "update.bad_tag");
    }
}

TEST_CASE("updater: this install kind's asset and SHA256SUMS are required, the other platform's is not") {
    // assets_for sırası: AppImage, zip, SHA256SUMS.
    for (const auto& [drop, kind, missing] :
         {std::tuple{0, AssetKind::AppImage, true}, std::tuple{1, AssetKind::AppImage, false},
          std::tuple{2, AssetKind::AppImage, true}, std::tuple{0, AssetKind::WindowsZip, false},
          std::tuple{1, AssetKind::WindowsZip, true}, std::tuple{2, AssetKind::WindowsZip, true}}) {
        CAPTURE(drop);
        CAPTURE(static_cast<int>(kind));
        auto assets = assets_for("v1.3.0", "1.3.0");
        assets.erase(assets.begin() + drop);
        FakeHttp http;
        http.responses.emplace(kApiUrl, ok(release_json("v1.3.0", assets)));
        const UpdateCheck result = latest(http, settings(), kind);
        if (missing) {
            CHECK(error_key(result) == "update.missing_asset");
        } else {
            REQUIRE(result.has_value());
            const auto& release = std::get<ReleaseInfo>(*result);
            const char* name = kind == AssetKind::AppImage ? "noteapp-1.3.0-x86_64.AppImage"
                                                           : "noteapp-1.3.0-windows-x86_64.zip";
            CHECK(release.asset(kind).url == download_url("v1.3.0", name));
            CHECK(release.asset(kind == AssetKind::AppImage ? AssetKind::WindowsZip : AssetKind::AppImage)
                      .name.empty());
        }
    }
    // Öbür platformun varlığının adresi bozuk olsa da bu kurulumun güncellemesi engellenmez.
    auto foreign = assets_for("v1.3.0", "1.3.0");
    foreign[1].url = "https://evil.example/zip";
    FakeHttp tolerant;
    tolerant.responses.emplace(kApiUrl, ok(release_json("v1.3.0", foreign)));
    CHECK(latest(tolerant).has_value());
    auto assets = assets_for("v1.3.0", "1.3.0");
    assets[0].name = "noteapp-1.2.0-x86_64.AppImage";
    assets[0].url = download_url("v1.3.0", assets[0].name);
    FakeHttp http;
    http.responses.emplace(kApiUrl, ok(release_json("v1.3.0", assets)));
    CHECK(error_key(latest(http)) == "update.missing_asset");
}

TEST_CASE("updater: a download url outside this release is rejected") {
    const std::string name = "noteapp-1.3.0-x86_64.AppImage";
    for (const std::string& url :
         {"https://github.com/someone/noteapp/releases/download/v1.3.0/" + name, download_url("v1.2.0", name),
          "http://github.com/ismetdyn/noteapp/releases/download/v1.3.0/" + name,
          download_url("v1.3.0", name) + "?x=1",
          "https://github.com.evil.example/ismetdyn/noteapp/releases/download/v1.3.0/" + name,
          download_url("v1.3.0", "SHA256SUMS")}) {
        CAPTURE(url);
        auto assets = assets_for("v1.3.0", "1.3.0");
        assets[0].url = url;
        FakeHttp http;
        http.responses.emplace(kApiUrl, ok(release_json("v1.3.0", assets)));
        CHECK(error_key(latest(http)) == "update.bad_asset_url");
    }
}

TEST_CASE("updater: malformed, too deep or incomplete JSON is a bad response") {
    std::string deep = R"({"tag_name":"v1.3.0","assets":[],"x":)";
    for (int i = 0; i < 64; ++i)
        deep += "[";
    for (int i = 0; i < 64; ++i)
        deep += "]";
    deep += "}";
    for (const std::string& body :
         {std::string("{"), std::string("[]"), std::string("not json"), deep, std::string(R"({"assets":[]})"),
          std::string(R"({"tag_name":"v1.3.0"})"), std::string(R"({"tag_name":1,"assets":[]})"),
          std::string(R"({"tag_name":"v1.3.0","assets":[{"name":"SHA256SUMS"}]})"),
          std::string(R"({"tag_name":"v1.3.0","assets":[{"name":"SHA256SUMS","size":-1,)"
                      R"("browser_download_url":"x"}]})"),
          std::string(R"({"tag_name":"v1.3.0","assets":[{"name":"SHA256SUMS","size":1.5,)"
                      R"("browser_download_url":"x"}]})")}) {
        CAPTURE(body);
        FakeHttp http;
        http.responses.emplace(kApiUrl, ok(body));
        CHECK(error_key(latest(http)) == "update.bad_response");
    }
}

TEST_CASE("updater: rate limit answers name the reset time") {
    const std::int64_t now =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count();
    const std::int64_t reset = now + 600;
    std::array<char, 32> text{};
    const std::tm local =
        noteapp::core::local_tm(std::chrono::system_clock::time_point(std::chrono::seconds(reset)));
    const std::string expected(text.data(),
                               std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M", &local));

    for (const long status : {403L, 429L}) {
        CAPTURE(status);
        FakeHttp http;
        http.responses.emplace(kApiUrl, reply(status, {}, exhausted_until(reset)));
        const auto result = latest(http);
        CHECK(error_key(result) == "update.rate_limited");
        CHECK(arg(result.error(), "reset") == expected);
    }
    // retry-after başlığı (ikincil sınır) sıfırlanma zamanından önce gelir.
    Headers retry_headers = exhausted_until(now + 2 * 24 * 3600);
    retry_headers.retry_after = 600;
    FakeHttp retry;
    retry.responses.emplace(kApiUrl, reply(429, {}, retry_headers));
    const auto retried = latest(retry, settings());
    CHECK(error_key(retried) == "update.rate_limited");
    CHECK(arg(retried.error(), "reset") != "<none>");

    FakeHttp bare;
    bare.responses.emplace(kApiUrl, reply(403));
    CHECK(error_key(latest(bare, settings())) == "update.rate_limited_no_reset");

    // Saatlik sınırın bir günden uzaktaki ya da negatif sıfırlanma zamanı bozuk başlıktır: zamansız ileti.
    for (const std::int64_t bad : {now + 2 * 24 * 3600, std::int64_t{-1}}) {
        FakeHttp odd;
        odd.responses.emplace(kApiUrl, reply(403, {}, exhausted_until(bad)));
        CHECK(error_key(latest(odd, settings())) == "update.rate_limited_no_reset");
    }

    // Başarısız cevapta kalan hak 0 (ör. 503): yine rate limit.
    FakeHttp exhausted;
    exhausted.responses.emplace(kApiUrl, reply(503, {}, exhausted_until(reset)));
    CHECK(error_key(latest(exhausted, settings())) == "update.rate_limited");

    // Hakkı tüketen son istek başarılıysa cevabı geçerlidir.
    FakeHttp last;
    last.responses.emplace(kApiUrl, reply(200, release_json("v1.3.0", "1.3.0"), exhausted_until(reset)));
    CHECK(available(last).version == "1.3.0");
}

TEST_CASE("updater: other HTTP failures and transport errors are reported, not retried") {
    FakeHttp missing;  // release yok: 404
    const auto result = latest(missing, settings());
    CHECK(error_key(result) == "update.http_status");
    CHECK(arg(result.error(), "status") == "404");
    CHECK(missing.requests.size() == 1);

    FakeHttp broken;
    broken.responses.emplace(kApiUrl, std::unexpected(Error{Error::Code::Io, "net.tls_failed", {}}));
    CHECK(error_key(latest(broken, settings())) == "net.tls_failed");
}

TEST_CASE("updater: a stop request before the call sends nothing") {
    FakeHttp http;
    http.responses.emplace(kApiUrl, ok(release_json("v1.3.0", "1.3.0")));
    std::stop_source stop;
    stop.request_stop();
    CHECK(error_key(check_latest(http.get(), settings(), AssetKind::AppImage, stop.get_token())) ==
          "update.cancelled");
    CHECK(http.requests.empty());
}

TEST_CASE("updater: the package is returned only when its SHA-256 matches its single SHA256SUMS line") {
    FakeHttp http = release_with_sums(sums_line(kOtherHash, "noteapp-1.3.0-windows-x86_64.zip") +
                                      sums_line(kPayloadSha, "noteapp-1.3.0-x86_64.AppImage"));
    const ReleaseInfo release = available(http);
    const auto bytes = download_verified(http.get(), release, AssetKind::AppImage, settings(), {});
    REQUIRE(bytes.has_value());
    CHECK(*bytes == kPayload);
    REQUIRE(http.requests.size() == 3);
    CHECK(http.requests[1].url == release.sums.url);
    CHECK(http.requests[1].max_bytes == Limit{64 * 1024, "64KiB"});
    CHECK(http.requests[2].url == release.appimage.url);
    CHECK(http.requests[2].max_bytes == Limit{512 * 1024 * 1024, "512MiB"});
}

TEST_CASE("updater: sha256sum's binary marker and upper-case hex are accepted") {
    std::string upper(kPayloadSha);
    std::ranges::transform(upper, upper.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::string& sums : {std::string(kPayloadSha) + " *noteapp-1.3.0-x86_64.AppImage\n",
                                    sums_line(upper, "noteapp-1.3.0-x86_64.AppImage")}) {
        CAPTURE(sums);
        FakeHttp http = release_with_sums(sums);
        CHECK(
            download_verified(http.get(), available(http), AssetKind::AppImage, settings(), {}).has_value());
    }
}

TEST_CASE("updater: SHA256SUMS without the line, with two lines or with a malformed line is rejected") {
    const std::string name = "noteapp-1.3.0-x86_64.AppImage";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {sums_line(kPayloadSha, "noteapp-1.3.0-windows-x86_64.zip"), "update.sums_missing"},
        {sums_line(kPayloadSha, name + ".old"), "update.sums_missing"},
        {"", "update.sums_missing"},
        {sums_line(kPayloadSha, name) + sums_line(kPayloadSha, name), "update.sums_duplicate"},
        {sums_line(kPayloadSha.substr(1), name), "update.sums_malformed"},
        {sums_line(std::string(kPayloadSha.substr(1)) + "g", name), "update.sums_malformed"},
        {std::string(kPayloadSha) + " " + name + "\n", "update.sums_malformed"},
        {std::string(kPayloadSha) + "\t" + name + "\n", "update.sums_malformed"},
        {sums_line(kPayloadSha, name) + "garbage\n", "update.sums_malformed"},
    };
    for (const auto& [sums, key] : cases) {
        CAPTURE(sums);
        FakeHttp http = release_with_sums(sums);
        CHECK(error_key(download_verified(http.get(), available(http), AssetKind::AppImage, settings(),
                                          {})) == key);
    }
}

TEST_CASE("updater: a package whose SHA-256 differs is not returned") {
    FakeHttp http = release_with_sums(sums_line(kPayloadSha, "noteapp-1.3.0-x86_64.AppImage"), "abd");
    CHECK(error_key(download_verified(http.get(), available(http), AssetKind::AppImage, settings(), {})) ==
          "update.checksum_mismatch");
}

TEST_CASE("updater: a file whose announced size is over the limit is not downloaded") {
    FakeHttp http = release_with_sums(sums_line(kPayloadSha, "noteapp-1.3.0-x86_64.AppImage"));
    const ReleaseInfo release = available(http);

    UpdateSettings small = settings();
    small.limits.download_bytes = {2, "2B"};
    const auto big_package = download_verified(http.get(), release, AssetKind::AppImage, small, {});
    CHECK(error_key(big_package) == "update.too_large");
    CHECK(arg(big_package.error(), "limit") == "2B");
    CHECK(http.requests.size() == 1);  // yalnız API: SUMS de paket de istenmedi

    small = settings();
    small.limits.sums_bytes = {2, "2B"};
    CHECK(error_key(download_verified(http.get(), release, AssetKind::AppImage, small, {})) ==
          "update.too_large");
    CHECK(http.requests.size() == 1);

    // 0 = sınırsız (limits kuralı): bildirilen boyut sınırlanmaz.
    small = settings();
    small.limits.download_bytes = {0, "0"};
    CHECK(download_verified(http.get(), release, AssetKind::AppImage, small, {}).has_value());
}

TEST_CASE("updater: every request it sends is accepted by the real net::http_get") {
    // http_get izin listesi dışındaki başlıkta std::invalid_argument fırlatır (src/net/http.cpp
    // kAllowedHeaders). Denetim, kapalı yerel porta bağlanmadan önce yapılır: ağa çıkılmaz.
    FakeHttp http = release_with_sums(sums_line(kPayloadSha, "noteapp-1.3.0-x86_64.AppImage"));
    const ReleaseInfo release = available(http);
    REQUIRE(download_verified(http.get(), release, AssetKind::AppImage, settings(), {}).has_value());
    REQUIRE(http.requests.size() == 3);  // API, SHA256SUMS, paket
    for (HttpRequest request : http.requests) {
        CAPTURE(request.url);
        request.url = "https://127.0.0.1:9/update-header-check";
        request.connect_timeout = std::chrono::seconds(1);
        request.total_timeout = std::chrono::seconds(1);
        CHECK_NOTHROW(static_cast<void>(noteapp::net::http_get(request, {})));
    }
}

TEST_CASE("updater: every error key the updater produces has a catalog text") {
    for (const char* key :
         {"update.bad_tag", "update.bad_current_version", "update.missing_asset", "update.bad_asset_url",
          "update.bad_response", "update.rate_limited", "update.rate_limited_no_reset", "update.http_status",
          "update.cancelled", "update.sums_missing", "update.sums_duplicate", "update.sums_malformed",
          "update.checksum_mismatch", "update.too_large"}) {
        CAPTURE(key);
        CHECK(noteapp::core::tr(key) != key);
    }
}

TEST_CASE("updater: startup check runs when never checked, after the interval or when the clock went back") {
    using std::chrono::hours;
    using std::chrono::seconds;
    using std::chrono::system_clock;
    const auto at = [](std::int64_t unix) { return system_clock::time_point(seconds(unix)); };
    const std::int64_t last = 1'800'000'000;
    const UpdateState checked{.last_check_unix = last, .last_seen_version = "1.2.3"};
    const auto day = std::chrono::duration_cast<std::chrono::milliseconds>(hours(24));

    CHECK(should_check_on_startup(at(last), UpdateState{}, day));
    CHECK_FALSE(should_check_on_startup(at(last + 60), checked, day));
    CHECK_FALSE(should_check_on_startup(at(last + 24 * 3600 - 1), checked, day));
    CHECK(should_check_on_startup(at(last + 24 * 3600), checked, day));
    CHECK(should_check_on_startup(at(last - 1), checked, day));  // saat geri gitti: beklenecek süre belirsiz
    CHECK_FALSE(should_check_on_startup(at(last + 365 * 24 * 3600), checked, std::chrono::milliseconds(0)));
    CHECK_FALSE(should_check_on_startup(at(last), UpdateState{}, std::chrono::milliseconds(0)));
}

TEST_CASE("updater: state file round-trips, is owner-only and falls back to defaults when broken") {
    const noteapp::test::TempDir home;
    const noteapp::test::ScopedEnv env("NOTEAPP_HOME", home.path().u8string());
    CHECK(noteapp::app::load_update_state() == UpdateState{});  // ilk çalıştırma

    const UpdateState state{.last_check_unix = 1'800'000'000, .last_seen_version = "1.3.0"};
    REQUIRE(noteapp::app::save_update_state(state).has_value());
    CHECK(noteapp::app::load_update_state() == state);
    const fs::path file = noteapp::core::state_dir() / "update-state.toml";
    REQUIRE(fs::exists(file));
#ifndef _WIN32
    CHECK((fs::status(file).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));
#endif

    for (const std::string& text :
         {std::string("last_check_unix = [\n"), std::string("last_check_unix = \"x\"\n"),
          "last_check_unix = 5\n#" + std::string(8192, 'x') + "\n"}) {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
        CHECK(noteapp::app::load_update_state() == UpdateState{});
    }
}
