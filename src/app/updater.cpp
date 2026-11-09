#include "app/updater.hpp"

#include "core/atomic_file.hpp"
#include "core/json_parser.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/text.hpp"
#include "graph/id.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace noteapp::app {

namespace {

namespace fs = std::filesystem;
using std::chrono::seconds;
using std::chrono::system_clock;

// Güncellemenin tek kaynağı bu depodur; config'e konmaz (başka depodan paket kurdurmak bir ayar olmamalı).
constexpr std::string_view kLatestReleaseUrl =
    "https://api.github.com/repos/ismetdyn/noteapp/releases/latest";
// Her varlık bu önek + "<tag>/<ad>" olmalıdır: aynı release'in github.com adresi.
constexpr std::string_view kDownloadUrlPrefix = "https://github.com/ismetdyn/noteapp/releases/download/";
constexpr std::string_view kReleasePageUrlPrefix = "https://github.com/ismetdyn/noteapp/releases/tag/";
constexpr std::string_view kAcceptJson = "application/vnd.github+json";
constexpr std::string_view kSumsName = "SHA256SUMS";

// Biçim sabiti, kapasite değil: release > assets > varlık > uploader > alan 4 düzeydir; payı aşan cevap
// bozuktur. Değer sayısını api_bytes sınırlar.
constexpr int kReleaseJsonDepth = 8;

constexpr long kHttpOk = 200;
constexpr long kHttpForbidden = 403;
constexpr long kHttpTooManyRequests = 429;
// GitHub'ın birincil sınırı saatliktir; bu kadar uzaktaki (ya da negatif) sıfırlanma zamanı bozuk sayılır ve
// zamansız ileti gösterilir.
constexpr std::int64_t kMaxRateLimitWaitSeconds = 24LL * 60 * 60;

constexpr std::size_t kSha256HexDigits = 64;
constexpr std::int64_t kMillisPerSecond = 1000;

// Biçim sabiti, kapasite değil (kullanıcı verisi büyütmez): dosyada yalnız iki anahtar var, en uzun hâli
// ~60 bayt. Bundan büyüğü elle bozulmuştur, okunmaz (varsayılana düşülür).
constexpr std::string_view kStateFileName = "update-state.toml";
constexpr std::uintmax_t kStateFileMaxBytes = 4096;
constexpr std::string_view kLastCheckKey = "last_check_unix";
constexpr std::string_view kLastSeenKey = "last_seen_version";

core::Error update_error(std::string key, std::vector<std::pair<std::string, std::string>> args = {}) {
    return core::Error{core::Error::Code::Io, std::move(key), std::move(args)};
}

struct Version {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    auto operator<=>(const Version&) const = default;
};

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Tam olarak X.Y.Z (ondalık rakamlar); işaret, boşluk, ön/son ek ve taşan sayı reddedilir.
std::optional<Version> parse_version(std::string_view text) {
    std::array<std::uint64_t, 3> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        const std::size_t dot = last ? text.size() : text.find('.');
        if (dot == std::string_view::npos)
            return std::nullopt;
        const std::string_view digits = text.substr(0, dot);
        if (digits.empty() || !std::ranges::all_of(digits, is_digit))
            return std::nullopt;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parts[i]);
        if (error != std::errc{})
            return std::nullopt;
        text.remove_prefix(last ? dot : dot + 1);
    }
    return Version{parts[0], parts[1], parts[2]};
}

const core::Json* field(const core::Json& object, const std::string& key, core::Json::Kind kind) {
    if (object.kind != core::Json::Kind::Object)
        return nullptr;
    const auto found = object.fields.find(key);
    return found != object.fields.end() && found->second.kind == kind ? &found->second : nullptr;
}

// API'nin `size` alanı: negatif olmayan tam sayı.
std::optional<std::uint64_t> json_size(const core::Json& value) {
    const std::string& text = value.text;
    if (text.empty() || !std::ranges::all_of(text, is_digit))
        return std::nullopt;
    std::uint64_t size = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), size);
    if (error != std::errc{})
        return std::nullopt;
    return size;
}

// 0 = sınırsız aynen geçer; sıfırdan büyük süre saniyeye yukarı yuvarlanır (250ms sınırsız olmasın).
seconds whole_seconds(const core::Limit& limit) {
    const std::uint64_t whole = (limit.value + kMillisPerSecond - 1) / kMillisPerSecond;
    return seconds(static_cast<seconds::rep>(whole));
}

net::HttpRequest request_for(std::string url, const core::Limit& max_bytes, const UpdateSettings& settings) {
    net::HttpRequest request;
    request.url = std::move(url);
    // GitHub, User-Agent'sız API isteğini reddeder.
    request.headers.emplace_back("User-Agent", "noteapp/" + settings.current_version);
    request.max_bytes = max_bytes;
    request.connect_timeout = whole_seconds(settings.limits.connect_timeout);
    request.total_timeout = whole_seconds(settings.limits.transfer_timeout);
    return request;
}

std::string reset_time_text(std::int64_t unix_seconds) {
    const std::tm local = core::local_tm(system_clock::time_point(seconds(unix_seconds)));
    std::array<char, 32> text{};
    return std::string(text.data(), std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M", &local));
}

// GitHub: retry-after varsa o kadar saniye beklenir; yoksa x-ratelimit-reset (unix saniye) zamanına dek.
core::Error rate_limited(const net::HttpResponse& response) {
    const std::int64_t now = std::chrono::floor<seconds>(system_clock::now()).time_since_epoch().count();
    std::optional<std::int64_t> reset;
    if (response.retry_after && *response.retry_after >= 0 &&
        *response.retry_after <= kMaxRateLimitWaitSeconds)
        reset = now + *response.retry_after;
    else if (response.ratelimit_reset && *response.ratelimit_reset >= 0 &&
             *response.ratelimit_reset <= now + kMaxRateLimitWaitSeconds)
        reset = response.ratelimit_reset;
    if (!reset)
        return update_error("update.rate_limited_no_reset");
    return update_error("update.rate_limited", {{"reset", reset_time_text(*reset)}});
}

// Başarılı (200) cevap ya da anlamlı hata. 403/429 ve hakkı bitmiş başarısız cevap rate limit'tir.
std::expected<net::HttpResponse, core::Error> fetch(const HttpGet& get, const net::HttpRequest& request,
                                                    const std::stop_token& stop) {
    if (stop.stop_requested())
        return std::unexpected(update_error("update.cancelled"));
    auto response = get(request, stop);
    if (!response || response->status == kHttpOk)
        return response;
    if (response->status == kHttpForbidden || response->status == kHttpTooManyRequests ||
        response->ratelimit_remaining == 0L)
        return std::unexpected(rate_limited(*response));
    return std::unexpected(
        update_error("update.http_status", {{"status", std::to_string(response->status)}}));
}

std::string download_url(std::string_view tag, std::string_view name) {
    return std::string(kDownloadUrlPrefix) + std::string(tag) + "/" + std::string(name);
}

// İstenen varlığı ve SHA256SUMS'u adla bulur; ikisi de aynı release'in adresini taşımalıdır.
std::expected<ReleaseInfo, core::Error> release_assets(ReleaseInfo info, AssetKind kind,
                                                       const core::Json& assets) {
    struct Wanted {
        std::string name;
        ReleaseAsset* out;
    };
    const std::array wanted = {
        kind == AssetKind::AppImage
            ? Wanted{"noteapp-" + info.version + "-x86_64.AppImage", &info.appimage}
            : Wanted{"noteapp-" + info.version + "-windows-x86_64.zip", &info.windows_zip},
        Wanted{std::string(kSumsName), &info.sums},
    };
    for (const core::Json& asset : assets.items) {
        const core::Json* name = field(asset, "name", core::Json::Kind::String);
        if (name == nullptr)
            return std::unexpected(update_error("update.bad_response"));
        const auto match = std::ranges::find(wanted, name->text, &Wanted::name);
        if (match == wanted.end())
            continue;
        const core::Json* size = field(asset, "size", core::Json::Kind::Number);
        const core::Json* url = field(asset, "browser_download_url", core::Json::Kind::String);
        const std::optional<std::uint64_t> bytes = size != nullptr ? json_size(*size) : std::nullopt;
        if (!bytes || url == nullptr || !match->out->name.empty())  // aynı ad iki kez: hangisi?
            return std::unexpected(update_error("update.bad_response"));
        if (url->text != download_url(info.tag, match->name))
            return std::unexpected(update_error("update.bad_asset_url", {{"name", match->name}}));
        *match->out = ReleaseAsset{.name = match->name, .url = url->text, .size = *bytes};
    }
    for (const Wanted& entry : wanted)
        if (entry.out->name.empty())
            return std::unexpected(update_error("update.missing_asset", {{"name", entry.name}}));
    return info;
}

// `sha256sum` çıktısı: "<64 hex><boşluk><' ' metin | '*' ikili><ad>". Her satır bu biçimde olmalı; varlığın
// satırı tam bir kez bulunur. Küçük harfli hex döner.
std::expected<std::string, core::Error> sums_entry(std::string_view sums, std::string_view name) {
    std::optional<std::string> found;
    while (!sums.empty()) {
        const std::size_t end = sums.find('\n');
        const std::string_view line = sums.substr(0, end);
        sums = end == std::string_view::npos ? std::string_view{} : sums.substr(end + 1);
        const std::string_view hash = line.substr(0, kSha256HexDigits);
        if (line.size() <= kSha256HexDigits + 2 || !std::ranges::all_of(hash, is_hex_digit) ||
            line[kSha256HexDigits] != ' ' ||
            (line[kSha256HexDigits + 1] != ' ' && line[kSha256HexDigits + 1] != '*'))
            return std::unexpected(update_error("update.sums_malformed"));
        if (line.substr(kSha256HexDigits + 2) != name)
            continue;
        if (found)
            return std::unexpected(update_error("update.sums_duplicate", {{"name", std::string(name)}}));
        found.emplace(hash);
        std::ranges::transform(*found, found->begin(), [](char c) {
            return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
        });
    }
    if (!found)
        return std::unexpected(update_error("update.sums_missing", {{"name", std::string(name)}}));
    return *found;
}

std::string sha256_hex(std::string_view bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    constexpr unsigned kNibbleBits = 4;
    constexpr unsigned kNibbleMask = 0x0F;
    const auto digest = graph::sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
    std::string out;
    out.reserve(kSha256HexDigits);
    for (const std::byte b : digest) {
        const auto value = std::to_integer<unsigned>(b);
        out += kDigits[value >> kNibbleBits];
        out += kDigits[value & kNibbleMask];
    }
    return out;
}

core::Error too_large(const ReleaseAsset& asset, const core::Limit& limit) {
    return update_error("update.too_large",
                        {{"name", asset.name}, {"size", std::to_string(asset.size)}, {"limit", limit.text}});
}

fs::path state_file() {
    return core::state_dir() / kStateFileName;
}

}  // namespace

std::string release_page_url(const ReleaseInfo& release) {
    return std::string(kReleasePageUrlPrefix) + release.tag;
}

UpdateCheck check_latest(const HttpGet& get, const UpdateSettings& settings, AssetKind kind,
                         std::stop_token stop) {
    const std::optional<Version> current = parse_version(settings.current_version);
    if (!current)  // derleme hatası: CMake sürümü X.Y.Z değil
        return std::unexpected(
            update_error("update.bad_current_version", {{"version", settings.current_version}}));
    net::HttpRequest request =
        request_for(std::string(kLatestReleaseUrl), settings.limits.api_bytes, settings);
    request.headers.emplace_back("Accept", kAcceptJson);
    auto response = fetch(get, request, stop);
    if (!response)
        return std::unexpected(std::move(response.error()));

    const std::optional<core::Json> json =
        core::JsonParser(response->body, {.max_depth = kReleaseJsonDepth}).parse();
    const core::Json* tag = json ? field(*json, "tag_name", core::Json::Kind::String) : nullptr;
    const core::Json* assets = json ? field(*json, "assets", core::Json::Kind::Array) : nullptr;
    if (tag == nullptr || assets == nullptr)
        return std::unexpected(update_error("update.bad_response"));
    // Etiket güvenilmez metindir: iletiye konmaz.
    const std::string_view version = std::string_view(tag->text).starts_with('v')
                                         ? std::string_view(tag->text).substr(1)
                                         : std::string_view(tag->text);
    const std::optional<Version> latest = parse_version(version);
    if (!latest)
        return std::unexpected(update_error("update.bad_tag"));
    if (*latest <= *current)  // eşit ya da eski: sürüm düşürülmez
        return UpToDate{std::string(version)};

    ReleaseInfo found;
    found.version = version;
    found.tag = tag->text;
    auto release = release_assets(std::move(found), kind, *assets);
    if (!release)
        return std::unexpected(std::move(release.error()));
    return std::move(*release);
}

std::expected<std::string, core::Error> download_verified(const HttpGet& get, const ReleaseInfo& release,
                                                          AssetKind kind, const UpdateSettings& settings,
                                                          std::stop_token stop) {
    const ReleaseAsset& asset = release.asset(kind);
    const core::UpdateLimits& limits = settings.limits;
    if (!limits.sums_bytes.allows(release.sums.size))
        return std::unexpected(too_large(release.sums, limits.sums_bytes));
    if (!limits.download_bytes.allows(asset.size))
        return std::unexpected(too_large(asset, limits.download_bytes));

    auto sums = fetch(get, request_for(release.sums.url, limits.sums_bytes, settings), stop);
    if (!sums)
        return std::unexpected(std::move(sums.error()));
    const auto expected_hash = sums_entry(sums->body, asset.name);
    if (!expected_hash)
        return std::unexpected(expected_hash.error());

    auto package = fetch(get, request_for(asset.url, limits.download_bytes, settings), stop);
    if (!package)
        return std::unexpected(std::move(package.error()));
    if (sha256_hex(package->body) != *expected_hash)
        return std::unexpected(update_error("update.checksum_mismatch", {{"name", asset.name}}));
    return std::move(package->body);
}

bool should_check_on_startup(system_clock::time_point now, const UpdateState& state,
                             std::chrono::milliseconds interval) {
    if (interval <= std::chrono::milliseconds::zero())
        return false;
    // Negatif zaman bozuk kayıttır; geleceğe ait son denetim saatin geri gittiğini gösterir: beklenecek süre
    // bilinmez, denetlenir. Karşılaştırma saniyede: dosyadaki büyük değer nanosaniyeye çevrilirken taşmaz.
    const std::int64_t now_seconds = std::chrono::floor<seconds>(now).time_since_epoch().count();
    if (!state.last_check_unix || *state.last_check_unix < 0 || *state.last_check_unix > now_seconds)
        return true;
    return (now_seconds - *state.last_check_unix) * kMillisPerSecond >= interval.count();
}

UpdateState load_update_state() {
    const fs::path file = state_file();
    std::error_code ec;
    if (!fs::exists(file, ec) && !ec)
        return {};  // ilk çalıştırma
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kStateFileMaxBytes) {
        core::log_warn("event=update_state_unreadable reason=" +
                       (ec ? ec.message() : std::string("too_large")));
        return {};
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        core::log_warn("event=update_state_unreadable line=" +
                       std::to_string(parsed.error().source().begin.line));
        return {};
    }
    const toml::table& table = parsed.table();
    return UpdateState{.last_check_unix = table[kLastCheckKey].value_exact<std::int64_t>(),
                       .last_seen_version =
                           table[kLastSeenKey].value_exact<std::string>().value_or(std::string())};
}

std::expected<void, core::Error> save_update_state(const UpdateState& state) {
    toml::table table;
    if (state.last_check_unix)
        table.insert(kLastCheckKey, *state.last_check_unix);
    table.insert(kLastSeenKey, state.last_seen_version);
    std::ostringstream out;
    out << table << "\n";
    std::error_code ignored;  // dizin oluşmazsa yazma aynı nedenle başarısız olur ve hata döner
    fs::create_directories(core::state_dir(), ignored);
    constexpr bool kOwnerOnly = true;
    return core::write_file_atomically(state_file(), out.str(), kOwnerOnly);
}

}  // namespace noteapp::app
