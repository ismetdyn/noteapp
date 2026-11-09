#include "net/http.hpp"

#include "core/version.hpp"
#include "net/timeout.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// curl.h Windows'ta winsock2.h üzerinden windows.h'yi çeker: NOMINMAX ondan önce tanımlı olmalı (std::max).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "core/process.hpp"
#endif

#include <curl/curl.h>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on
#endif

namespace noteapp::net {

namespace {

// GitHub'ın varlık indirmesi bir iki yönlendirmedir (api → objects.githubusercontent.com); sınır döngüyü keser.
constexpr long kMaxRedirects = 5;
// İstekte de yönlendirmede de tek izinli şema; http'ye düşen yönlendirme reddedilir.
constexpr const char* kAllowedProtocols = "https";
constexpr long kVerifyHostName = 2;  // CURLOPT_SSL_VERIFYHOST: sertifika adı ana bilgisayarla eşleşmeli
constexpr std::string_view kLineBreaks{"\r\n\0", 3};

struct EasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};
using UniqueEasy = std::unique_ptr<CURL, EasyDeleter>;
struct UrlDeleter {
    void operator()(CURLU* handle) const noexcept { curl_url_cleanup(handle); }
};
using UniqueUrl = std::unique_ptr<CURLU, UrlDeleter>;
struct SlistDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};
using UniqueSlist = std::unique_ptr<curl_slist, SlistDeleter>;
struct CurlStringDeleter {
    void operator()(char* text) const noexcept { curl_free(text); }
};
using UniqueCurlString = std::unique_ptr<char, CurlStringDeleter>;
struct MultiDeleter {
    void operator()(CURLM* handle) const noexcept { curl_multi_cleanup(handle); }
};
using UniqueMulti = std::unique_ptr<CURLM, MultiDeleter>;

// İptal sinyali gelmese de curl_multi_poll en geç bu kadar uyur (curl'ün kendi zamanlayıcıları ayrıca uyandırır).
constexpr int kPollMilliseconds = 1000;

// curl_global_init süreçte bir kez (sync/git_runtime.cpp'deki magic static deseni: diğer iş parçacıkları
// bitmesini bekler). curl_global_cleanup bilerek çağrılmaz: başka iş parçacığında süren aktarım varken
// çağrılamaz; kaynakları süreç sonu bırakır.
CURLcode init_curl_once() {
    static const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    return code;
}

core::Error net_error(std::string key, std::vector<std::pair<std::string, std::string>> args) {
    return core::Error{core::Error::Code::Io, std::move(key), std::move(args)};
}

// Seçenek reddi derleme yapılandırması hatasıdır (ör. https kısıtı kurulamadı): istek bu durumda gönderilmez.
template <class Value> void set_option(CURL* handle, CURLoption option, Value value) {
    const CURLcode code = curl_easy_setopt(handle, option, value);
    if (code == CURLE_OUT_OF_MEMORY)
        throw std::bad_alloc();
    if (code != CURLE_OK)
        throw std::logic_error(std::string("curl_easy_setopt: ") + curl_easy_strerror(code));
}

std::optional<std::string> url_part(CURLU* url, CURLUPart part) {
    char* raw = nullptr;
    if (curl_url_get(url, part, &raw, 0) != CURLUE_OK)
        return std::nullopt;
    const UniqueCurlString owned(raw);
    return std::string(raw);
}

struct ParsedUrl {
    UniqueUrl handle;
    std::string host;
    std::string origin;  // "şema://ana-bilgisayar": iletilere giren tek adres biçimi
};

// Adres curl'ün kendi ayrıştırıcısıyla çözülür ve aktarıma aynı tutamaç verilir (CURLOPT_CURLU): denetlenen
// ile gidilen adres ayrışmaz. Kullanıcı adı/parola içeren adres reddedilir. İletiye ham adres hiç girmez
// (kimlik bilgisi, yol, sorgu taşıyabilir): çözülemeyende hiçbir parçası, çözülende yalnız şema + ana bilgisayar.
std::expected<ParsedUrl, core::Error> parse_https_url(const std::string& text) {
    UniqueUrl url(curl_url());
    if (!url)
        throw std::bad_alloc();
    // CURLU_NON_SUPPORT_SCHEME: bu yapıda derlenmemiş şema (ftp://) da çözülür ve "yalnız https" diye reddedilir.
    if (curl_url_set(url.get(), CURLUPART_URL, text.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        return std::unexpected(net_error("net.invalid_url", {}));
    const std::optional<std::string> scheme = url_part(url.get(), CURLUPART_SCHEME);
    std::optional<std::string> host = url_part(url.get(), CURLUPART_HOST);
    if (!scheme || !host || host->empty())
        return std::unexpected(net_error("net.invalid_url", {}));
    std::string origin = *scheme + "://" + *host;
    const bool has_credentials =
        url_part(url.get(), CURLUPART_USER).has_value() || url_part(url.get(), CURLUPART_PASSWORD).has_value();
    if (has_credentials || scheme != "https")
        return std::unexpected(net_error("net.https_only", {{"url", std::move(origin)}}));
    return ParsedUrl{std::move(url), std::move(*host), std::move(origin)};
}

bool equals_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// Güncelleme denetiminin gönderdiği başlıklar (GitHub API). İzin listesi: kimlik bilgisi (Authorization,
// Proxy-Authorization, Cookie) ya da curl'ün kendi yönettiği başlık (Host, Content-Length) yanlışlıkla
// eklenemez. Yeni başlık gerekirse buraya eklenir.
constexpr std::string_view kUserAgent = "User-Agent";
constexpr std::string_view kAllowedHeaders[] = {"Accept", kUserAgent, "X-GitHub-Api-Version"};

// Başlıkları çağıran kod yazar, kullanıcı değil: izin dışı ad ya da satır sonu (başlık enjeksiyonu) bir
// hatadır, yanıt değil; fırlatılır. User-Agent verilmezse "noteapp/<sürüm>" gönderilir.
UniqueSlist request_headers(const HttpRequest& request) {
    UniqueSlist list;
    const auto append = [&list](const std::string& line) {
        // Başarıda zincirin başı döner (boş listede yeni düğüm), hatada nullptr ve liste olduğu gibi kalır.
        curl_slist* head = curl_slist_append(list.get(), line.c_str());
        if (head == nullptr)
            throw std::bad_alloc();
        static_cast<void>(list.release());
        list.reset(head);
    };
    bool has_user_agent = false;
    for (const auto& [name, value] : request.headers) {
        const bool allowed = std::ranges::any_of(
            kAllowedHeaders, [&name](std::string_view allowed_name) { return equals_ignoring_case(name, allowed_name); });
        if (!allowed)
            throw std::invalid_argument("net::http_get: header not allowed: " + name);
        if (value.find_first_of(kLineBreaks) != std::string::npos)
            throw std::invalid_argument("net::http_get: line break in header " + name);
        has_user_agent = has_user_agent || equals_ignoring_case(name, kUserAgent);
        // Boş değerli "Ad:" curl'de başlığı siler; boş değer "Ad;" ile gönderilir.
        append(value.empty() ? name + ";" : name + ": " + value);
    }
    if (!has_user_agent)
        append(std::string(kUserAgent) + ": noteapp/" + std::string(core::version()));
    return list;
}

struct Transfer {
    const core::Limit& max_bytes;
    std::stop_token stop;
    std::string body;
    bool cancelled = false;
    bool too_large = false;
    bool out_of_memory = false;
};

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) noexcept {
    auto& transfer = *static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;  // size her zaman 1 (CURLOPT_WRITEFUNCTION)
    if (transfer.stop.stop_requested()) {
        transfer.cancelled = true;
        return CURL_WRITEFUNC_ERROR;
    }
    if (!transfer.max_bytes.allows(std::uint64_t{transfer.body.size()} + bytes)) {
        transfer.too_large = true;
        return CURL_WRITEFUNC_ERROR;
    }
    // İstisna C kodunun içinden geçemez: bayrakla taşınır, perform dönünce yeniden fırlatılır.
    try {
        transfer.body.append(data, bytes);
    } catch (const std::bad_alloc&) {
        transfer.out_of_memory = true;
        return CURL_WRITEFUNC_ERROR;
    }
    return bytes;
}

// Bağlantı kurulduktan sonra çağrılır (iptalin asıl yolu perform()'daki döngüdür). dltotal yanıtın
// Content-Length'idir (bilinmiyorsa 0): sınırı aşan gövde ilk baytı beklemeden kesilir.
int on_progress(void* user, curl_off_t download_total, curl_off_t downloaded, curl_off_t, curl_off_t) noexcept {
    auto& transfer = *static_cast<Transfer*>(user);
    if (transfer.stop.stop_requested()) {
        transfer.cancelled = true;
        return 1;
    }
    const curl_off_t expected = std::max(download_total, downloaded);
    if (expected > 0 && !transfer.max_bytes.allows(static_cast<std::uint64_t>(expected))) {
        transfer.too_large = true;
        return 1;
    }
    return 0;
}

std::optional<std::string_view> response_header(CURL* handle, const char* name) {
    curl_header* header = nullptr;
    if (curl_easy_header(handle, name, 0, CURLH_HEADER, -1, &header) != CURLHE_OK || header->value == nullptr)
        return std::nullopt;
    std::string_view value(header->value);
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return std::string_view{};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

// Yalnız tam ondalık tam sayı; artık karakter ya da taşma nullopt (sunucu yanıtı güvenilmezdir).
template <class Number> std::optional<Number> header_number(CURL* handle, const char* name) {
    const std::optional<std::string_view> text = response_header(handle, name);
    if (!text)
        return std::nullopt;
    Number value{};
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size())
        return std::nullopt;
    return value;
}

// Retry-After saniye ya da HTTP tarihidir. Saniye burada ayrıştırılır ("0" geçerlidir, negatif yok sayılır).
// Tarihi curl saniyeye çevirir (CURLINFO_RETRY_AFTER), ama geçmiş tarih de çözülemeyen değer de orada 0'dır ve
// birbirinden ayrılamaz: tarih biçiminde yalnız ileri bir tarih (> 0 sn) değer olur, gerisi nullopt.
std::optional<std::int64_t> retry_after(CURL* handle) {
    if (const auto seconds = header_number<std::int64_t>(handle, "retry-after"))
        return *seconds >= 0 ? seconds : std::nullopt;
    if (!response_header(handle, "retry-after"))
        return std::nullopt;
    curl_off_t seconds = 0;
    if (curl_easy_getinfo(handle, CURLINFO_RETRY_AFTER, &seconds) != CURLE_OK || seconds <= 0)
        return std::nullopt;
    return std::int64_t{seconds};
}

core::Error transfer_error(CURLcode code, const Transfer& transfer, const ParsedUrl& url, const HttpRequest& request) {
    if (transfer.cancelled)
        return net_error("net.cancelled", {{"host", url.host}});
    if (transfer.too_large)
        return net_error("net.too_large", {{"host", url.host}, {"limit", request.max_bytes.text}});
    // {detail} yalnız hata kodunun sabit metnidir. curl'ün ayrıntılı hata arabelleği (CURLOPT_ERRORBUFFER)
    // kullanılmaz: vekil adresini olduğu gibi yazar (lib/proxy.c "Unsupported proxy syntax in '%s'"), ortamdaki
    // https_proxy kullanıcı:parola taşıyabilir ve ileti ekrana ve log'a gider.
    const std::string detail = curl_easy_strerror(code);
    switch (code) {
    case CURLE_UNSUPPORTED_PROTOCOL:  // yönlendirme https dışına çıktı
        return net_error("net.https_only", {{"url", url.origin}});
    case CURLE_OPERATION_TIMEDOUT:
        return net_error("net.timeout", {{"host", url.host}, {"detail", detail}});
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_INVALIDCERTSTATUS:
        return net_error("net.tls_failed", {{"host", url.host}, {"detail", detail}});
    default:
        return net_error("net.failed", {{"host", url.host}, {"detail", detail}});
    }
}

#ifdef _WIN32
struct GlobalFreeDeleter {
    void operator()(wchar_t* text) const noexcept { GlobalFree(text); }
};
using UniqueGlobalString = std::unique_ptr<wchar_t, GlobalFreeDeleter>;

std::string utf8_from_wide(const wchar_t* wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();  // sondaki NUL
    return out;
}

// Internet Seçenekleri'nin vekil dizgesi: "ana:port", "http=a:80;https=b:443" ya da ';'/boşlukla ayrılmış
// liste. Protokol başına biçimde https girdisi yoksa https doğrudan gider (Windows'un kendi davranışı).
std::optional<std::string> https_entry(std::string_view list) {
    std::optional<std::string> first;
    bool per_protocol = false;
    for (std::size_t start = 0; start < list.size();) {
        const std::size_t end = std::min(list.find_first_of("; \t", start), list.size());
        const std::string_view entry = list.substr(start, end - start);
        start = end + 1;
        if (entry.empty())
            continue;
        if (const std::size_t equals = entry.find('='); equals != std::string_view::npos) {
            per_protocol = true;
            if (equals_ignoring_case(entry.substr(0, equals), "https") && equals + 1 < entry.size())
                return std::string(entry.substr(equals + 1));
        } else if (!first) {
            first = std::string(entry);
        }
    }
    return per_protocol ? std::nullopt : first;
}

// Kurumsal ağda vekil genelde ortam değişkeninde değil Internet Seçenekleri'ndedir; curl onu kendiliğinden
// okumaz. Ortam değişkeni (curl'ün okuduğu https_proxy/all_proxy) varsa o kazanır: CURLOPT_PROXY verilmez.
// Kapsam dışı, bilinen sapmalar: (1) otomatik algılama/PAC betiği okunmaz (WinHttpGetProxyForUrl gerekir);
// yalnız PAC tanımlıysa vekil kullanılmaz, bağlantı doğrudan denenir. (2) Atlama listesi (lpszProxyBypass)
// curl'e verilmez (biçimi no_proxy'ye birebir çevrilmez): listede olan bir ana bilgisayar da vekilden geçer;
// atlamak gerekirse NO_PROXY ortam değişkeni geçerlidir.
std::optional<std::string> windows_proxy() {
    for (const char* name : {"https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"}) {
        if (core::get_env_utf8(name))
            return std::nullopt;
    }
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config))
        return std::nullopt;
    const UniqueGlobalString auto_config_url(config.lpszAutoConfigUrl);
    const UniqueGlobalString proxy(config.lpszProxy);
    const UniqueGlobalString bypass(config.lpszProxyBypass);
    if (!proxy)
        return std::nullopt;
    return https_entry(utf8_from_wide(proxy.get()));
}
#endif

void check_multi(CURLMcode code) {
    if (code == CURLM_OUT_OF_MEMORY)
        throw std::bad_alloc();
    if (code != CURLM_OK)
        throw std::logic_error(std::string("curl multi: ") + curl_multi_strerror(code));
}

// curl_easy_perform değil: curl ilerleme geri çağrısını yalnız bağlantı kurulduktan sonra çağırır (multi.c,
// Curl_conn_is_connected); cevap vermeyen bir TLS el sıkışmasında iptal bağlantı zaman aşımına dek (0'da
// sınırsız) görülmezdi. Burada döngü bizim: stop_callback curl_multi_poll'u curl_multi_wakeup ile uyandırır ve
// iptal her evrede hemen görülür. Dönüş aktarımın sonucudur; iptalde transfer.cancelled kurulur.
CURLcode perform(CURL* easy, Transfer& transfer) {
    const UniqueMulti multi(curl_multi_init());
    if (!multi)
        throw std::bad_alloc();
    CURLM* handle = multi.get();
    check_multi(curl_multi_add_handle(handle, easy));
    // Ters yıkım sırası: önce stop_callback kalkar, sonra tutamaç multi'den çıkar, en son multi yıkılır.
    struct Detach {
        CURLM* multi;
        CURL* easy;
        ~Detach() { curl_multi_remove_handle(multi, easy); }
    } const detach{handle, easy};
    const std::stop_callback wake(transfer.stop, [handle] { curl_multi_wakeup(handle); });

    for (int running = 1;;) {
        check_multi(curl_multi_perform(handle, &running));
        if (running == 0)
            break;
        if (transfer.stop.stop_requested()) {
            transfer.cancelled = true;
            return CURLE_ABORTED_BY_CALLBACK;
        }
        check_multi(curl_multi_poll(handle, nullptr, 0, kPollMilliseconds, nullptr));
    }
    int queued = 0;
    for (const CURLMsg* message; (message = curl_multi_info_read(handle, &queued)) != nullptr;) {
        if (message->msg == CURLMSG_DONE && message->easy_handle == easy)
            return message->data.result;
    }
    throw std::logic_error("curl multi: transfer ended without a result");
}

long clamp_to_long(std::chrono::seconds timeout) {
    if (timeout.count() < 0)
        throw std::invalid_argument("net::http_get: negative timeout");
    return static_cast<long>(std::min<std::chrono::seconds::rep>(timeout.count(), std::numeric_limits<long>::max()));
}

}  // namespace

namespace detail {

long curl_connect_timeout(std::chrono::seconds timeout) {
    return timeout.count() == 0 ? std::numeric_limits<long>::max() : clamp_to_long(timeout);
}

long curl_total_timeout(std::chrono::seconds timeout) {
    return clamp_to_long(timeout);  // curl'de de 0 = sınırsız
}

}  // namespace detail

std::expected<HttpResponse, core::Error> http_get(const HttpRequest& request, std::stop_token stop) {
    // curl_global_init her curl çağrısından (curl_url dahil) önce: ilk kullanım genel başlatmayı bulmalı.
    if (const CURLcode code = init_curl_once(); code != CURLE_OK)
        return std::unexpected(net_error("net.failed", {{"host", ""}, {"detail", curl_easy_strerror(code)}}));
    std::expected<ParsedUrl, core::Error> url = parse_https_url(request.url);
    if (!url)
        return std::unexpected(std::move(url.error()));
    if (stop.stop_requested())
        return std::unexpected(net_error("net.cancelled", {{"host", url->host}}));

    // Tutamağa verilen her şey (adres, başlıklar, aktarım durumu) tutamaktan önce kurulur ve ondan sonra
    // yıkılır: curl_easy_cleanup onlara hâlâ dokunabilir.
    const UniqueSlist headers = request_headers(request);
    Transfer transfer{request.max_bytes, std::move(stop), {}};
    const UniqueEasy easy(curl_easy_init());
    if (!easy)
        throw std::bad_alloc();
    CURL* handle = easy.get();

    set_option(handle, CURLOPT_CURLU, url->handle.get());
    set_option(handle, CURLOPT_PROTOCOLS_STR, kAllowedProtocols);
    set_option(handle, CURLOPT_REDIR_PROTOCOLS_STR, kAllowedProtocols);
    set_option(handle, CURLOPT_FOLLOWLOCATION, CURLFOLLOW_ALL);
    set_option(handle, CURLOPT_MAXREDIRS, kMaxRedirects);
    // Varsayılanlar zaten bunlar; açıkça yazılır ki kapatılmadıkları kodda görünsün. Doğrulama hiçbir koşulda
    // kapanmaz.
    set_option(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    set_option(handle, CURLOPT_SSL_VERIFYHOST, kVerifyHostName);
    // En düşük TLS 1.2 (curl 8.16'dan beri varsayılan da bu): kütüphane ya da yapılandırma değişse de eski
    // sürüme inilmez.
    set_option(handle, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
#ifdef _WIN32
    // Schannel varsayılanda iptal listesine (CRL/OCSP) erişemeyen bağlantıyı reddeder; kurumsal ağlar ve bazı
    // vekiller bu sunuculara erişimi keser, güncelleme denetimi hiç çalışmazdı. Best-effort yalnız "iptal
    // bilgisi alınamadı" hatasını yok sayar: zincir, ad ve süre doğrulaması tam kalır, iptal edildiği bilinen
    // sertifika yine reddedilir. Bedeli: iptal edilmiş ama iptal bilgisine ulaşılamayan bir sertifika kabul
    // edilir. SHA256SUMS bunu telafi etmez: aynı TLS kanalından gelir, ayrı bir güven katmanı değildir.
    set_option(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    if (const std::optional<std::string> proxy = windows_proxy())
        set_option(handle, CURLOPT_PROXY, proxy->c_str());
#endif
    // Sinyalsiz: çok iş parçacıklı süreçte SIGALRM ile ad çözümü zaman aşımı güvensizdir (threaded resolver
    // zaten kullanılır).
    set_option(handle, CURLOPT_NOSIGNAL, 1L);
    set_option(handle, CURLOPT_CONNECTTIMEOUT, detail::curl_connect_timeout(request.connect_timeout));
    set_option(handle, CURLOPT_TIMEOUT, detail::curl_total_timeout(request.total_timeout));
    set_option(handle, CURLOPT_HTTPHEADER, headers.get());
    set_option(handle, CURLOPT_WRITEFUNCTION, static_cast<curl_write_callback>(&on_body));
    set_option(handle, CURLOPT_WRITEDATA, static_cast<void*>(&transfer));
    set_option(handle, CURLOPT_NOPROGRESS, 0L);
    set_option(handle, CURLOPT_XFERINFOFUNCTION, static_cast<curl_xferinfo_callback>(&on_progress));
    set_option(handle, CURLOPT_XFERINFODATA, static_cast<void*>(&transfer));

    const CURLcode code = perform(handle, transfer);
    if (transfer.out_of_memory)
        throw std::bad_alloc();
    if (code != CURLE_OK)
        return std::unexpected(transfer_error(code, transfer, *url, request));

    HttpResponse response;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    response.body = std::move(transfer.body);
    response.ratelimit_remaining = header_number<long>(handle, "x-ratelimit-remaining");
    response.ratelimit_reset = header_number<std::int64_t>(handle, "x-ratelimit-reset");
    response.retry_after = retry_after(handle);
    return response;
}

}  // namespace noteapp::net
