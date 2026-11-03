#include "support/test_support.hpp"
#include "tui/lsp_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <future>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using noteapp::tui::LspClient;
namespace lsp = noteapp::core::lsp;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kArrival = 5s;  // sanitizer altında süreç başlatma yavaş; olay bu süre içinde gelmeli
constexpr auto kShutdownBound = 1500ms;
// UI'nin tek bir çağrıyı beklemeyi göze alabileceği süre; bir kare 16 ms.
constexpr auto kUiCallBudget = 5ms;
constexpr std::string_view kUri = "file:///workspace/a.py";
constexpr std::size_t kLargeTextBytes = 256U * 1024U;       // yazma parçasının (64 KiB) birkaç katı
constexpr std::size_t kHugeLineBytes = 2U * 1024U * 1024U;  // tek satırlık 2 MiB'lık belge
// 1000 tanının çevrimi satırı bir kez yürümeli; tanı başına yürüyüş (2000 × 2 MiB) dakikalar sürerdi.
// Debug'da 2 MiB'ın JSON'a yazılıp iki uçta ayrıştırılması tek başına ~1 s; sınır ikisini ayıracak kadar
// geniş.
constexpr auto kHeavyConversionBudget = 10s;

struct Probe {
    std::atomic<int> notified{0};
};

std::unique_ptr<LspClient>
make_client(Probe& probe, const char* mode,
            std::chrono::milliseconds initialize_timeout = LspClient::kDefaultInitializeTimeout) {
    return std::make_unique<LspClient>(LspClient::Options{.language = "fake",
                                                          .command = {NOTEAPP_FAKE_LSP_SERVER, mode},
                                                          .root_uri = "file:///workspace",
                                                          .initialize_timeout = initialize_timeout},
                                       [&probe] { ++probe.notified; });
}

template <class Predicate> bool eventually(Predicate predicate, std::chrono::milliseconds limit = kArrival) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::string first_message(const LspClient& client, std::string_view uri = kUri) {
    const auto list = client.diagnostics(std::string(uri));
    return list.empty() ? std::string() : list.front().message;
}

// İlk tanı geldiyse initialize bitmiş ve belge sunucudadır.
void open_and_wait(LspClient& client, std::string text, std::int64_t version = 1) {
    client.open(std::string(kUri), "python", std::move(text), version);
    REQUIRE(eventually([&] { return !client.diagnostics(std::string(kUri)).empty(); }));
}

std::chrono::milliseconds destroy_timed(std::unique_ptr<LspClient>& client) {
    const auto start = Clock::now();
    client.reset();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

std::optional<std::vector<lsp::CompletionItem>> wait_completion(LspClient& client, LspClient::Ticket ticket,
                                                                std::chrono::milliseconds limit = kArrival) {
    std::optional<std::vector<lsp::CompletionItem>> items;
    eventually([&] { return (items = client.take_completion(ticket)).has_value(); }, limit);
    return items;
}

}  // namespace

TEST_CASE("diagnostics_arrive_with_byte_ranges_and_notify_fires") {
    Probe probe;
    auto client = make_client(probe, "normal");
    open_and_wait(*client, "hello world\nsecond");
    const auto list = client->diagnostics(std::string(kUri));
    REQUIRE(list.size() == 1);
    CHECK(list[0].message == "count=1 text=hello world second");
    CHECK(list[0].severity == lsp::DiagnosticSeverity::Error);
    CHECK(list[0].range.start.line == 0);
    CHECK(list[0].range.end.character == 11);
    // Bildirim durum yayınlandıktan sonra, kilit dışında gelir: gözlemci tanıyı bildirimden önce görebilir.
    CHECK(eventually([&] { return probe.notified > 0; }));
    CHECK_FALSE(client->stopped());
}

TEST_CASE("diagnostics_carry_the_version_they_were_published_for") {
    Probe probe;
    auto client = make_client(probe, "normal");
    open_and_wait(*client, "a\nb\nc");
    CHECK(client->document_diagnostics(std::string(kUri)).version == 1);
    client->change(std::string(kUri), "z", 2);
    // Sunucu yeni metni görene dek tanılar eski sürümle etiketli kalır: UI güncel metne uymadığını bilir.
    CHECK(client->document_diagnostics(std::string(kUri)).version == 1);
    REQUIRE(eventually([&] { return client->document_diagnostics(std::string(kUri)).version == 2; }));
    CHECK(client->document_diagnostics(std::string(kUri)).diagnostics.size() == 1);
    CHECK(client->document_diagnostics("file:///never-opened.py").diagnostics.empty());
}

TEST_CASE("closing_a_document_clears_its_diagnostics") {
    Probe probe;
    auto client = make_client(probe, "normal");
    open_and_wait(*client, "x");
    client->close(std::string(kUri));
    CHECK(client->diagnostics(std::string(kUri)).empty());
}

TEST_CASE("rapid_changes_are_coalesced_and_the_server_sees_the_last_text") {
    Probe probe;
    auto client = make_client(probe, "normal");
    open_and_wait(*client, "v1");
    constexpr int kChanges = 50;
    for (int version = 2; version <= kChanges; ++version)
        client->change(std::string(kUri), "v" + std::to_string(version), version);
    REQUIRE(eventually([&] { return first_message(*client).ends_with("text=v50"); }));
    // Sayaç sunucunun aldığı didOpen + didChange sayısıdır; her değişiklik gitseydi 50 olurdu.
    const std::string message = first_message(*client);
    const int received = std::stoi(message.substr(message.find('=') + 1));
    CHECK(received < kChanges);
}

TEST_CASE("completion_positions_follow_the_negotiated_utf8_encoding") {
    Probe probe;
    auto client = make_client(probe, "normal");
    open_and_wait(*client, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80 bad\n");
    const auto ticket = client->request_completion(std::string(kUri), 1, {.line = 0, .character = 12});
    REQUIRE(ticket);
    const auto items = wait_completion(*client, *ticket);
    REQUIRE(items);
    REQUIRE(items->size() == 2);
    CHECK((*items)[0].label == "pos:0:12");
    CHECK((*items)[1].insert_text == "alpha()");
    CHECK_FALSE(client->take_completion(*ticket));  // bir kez verilir
}

TEST_CASE("utf16_ranges_and_positions_are_converted") {
    Probe probe;
    auto client = make_client(probe, "utf16");
    // İki astral karakter UTF-16'da 4 birim, UTF-8'de 8 bayt: "bad" bayt 9-12, birim 5-8.
    open_and_wait(*client, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80 bad\n");
    const auto list = client->diagnostics(std::string(kUri));
    REQUIRE(list.size() == 2);
    CHECK(list[0].range.start.character == 9);
    CHECK(list[0].range.end.character == 12);
    // Sütuna göre sırasız gelen tanılar da doğru çevrilir ve sıraları korunur: ikinci karakter bayt 4-8.
    CHECK(list[1].message == "second");
    CHECK(list[1].range.start.character == 4);
    CHECK(list[1].range.end.character == 8);
    const auto ticket = client->request_completion(std::string(kUri), 1, {.line = 0, .character = 12});
    REQUIRE(ticket);
    const auto items = wait_completion(*client, *ticket);
    REQUIRE(items);
    CHECK(items->front().label == "pos:0:8");
}

TEST_CASE("stale_completion_is_dropped_after_a_newer_version") {
    Probe probe;
    auto client = make_client(probe, "slow-completion");
    open_and_wait(*client, "v1");
    // Pozitif kontrol: sürüm kıpırdamazsa yavaş cevap da teslim edilir.
    const auto fresh = client->request_completion(std::string(kUri), 1, {});
    REQUIRE(fresh);
    REQUIRE(wait_completion(*client, *fresh));

    const auto stale = client->request_completion(std::string(kUri), 1, {});
    REQUIRE(stale);
    std::this_thread::sleep_for(100ms);  // istek sunucuya ulaşsın; sunucu cevabı geciktirir
    client->change(std::string(kUri), "v2", 2);
    // Sunucu tek iş parçacıklı: v2 tanısı geldiyse completion cevabı ondan önce akıştan okunmuştur.
    REQUIRE(eventually([&] { return first_message(*client).ends_with("text=v2"); }));
    CHECK_FALSE(client->take_completion(*stale));
}

TEST_CASE("a_newer_completion_request_supersedes_the_older_ticket") {
    Probe probe;
    auto client = make_client(probe, "normal");
    open_and_wait(*client, "x");
    const auto older = client->request_completion(std::string(kUri), 1, {});
    const auto newer = client->request_completion(std::string(kUri), 1, {});
    REQUIRE(older);
    REQUIRE(newer);
    REQUIRE(wait_completion(*client, *newer));
    CHECK_FALSE(client->take_completion(*older));
}

TEST_CASE("hung_initialize_times_out_and_ui_calls_stay_fast") {
    Probe probe;
    auto client = make_client(probe, "hang", 300ms);
    auto slowest = Clock::duration::zero();
    const auto timed = [&](auto call) {
        const auto start = Clock::now();
        call();
        slowest = std::max(slowest, Clock::now() - start);
    };
    timed([&] { client->open(std::string(kUri), "python", "x", 1); });
    for (int version = 2; version < 100; ++version) {
        timed([&] { client->change(std::string(kUri), "x" + std::to_string(version), version); });
        timed([&] { (void)client->diagnostics(std::string(kUri)); });
        timed([&] { (void)client->request_completion(std::string(kUri), version, {}); });
        timed([&] { (void)client->stopped(); });
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(eventually([&] { return client->stopped().has_value(); }));
    CHECK(client->stopped() == LspClient::StopReason::InitializeTimeout);
    // Durma, bildirimden önce görünür olur (bildirim kilit dışında gelir); sıra varsayılmaz, bildirim
    // beklenir.
    CHECK(eventually([&] { return probe.notified > 0; }));
    MESSAGE("slowest UI call: " << std::chrono::duration<double, std::micro>(slowest).count() << " us");
    if constexpr (noteapp::test::kWallClockBudgets)
        CHECK(slowest < kUiCallBudget);
    // Durmuş istemci yeni işi sessizce reddeder.
    CHECK_FALSE(client->request_completion(std::string(kUri), 100, {}));
}

TEST_CASE("server_crash_stops_the_client") {
    Probe probe;
    auto client = make_client(probe, "crash");
    client->open(std::string(kUri), "python", "x", 1);
    REQUIRE(eventually([&] { return client->stopped().has_value(); }));
    CHECK(client->stopped() == LspClient::StopReason::Exited);
    CHECK(client->diagnostics(std::string(kUri)).empty());
}

TEST_CASE("oversized_frame_stops_the_client") {
    Probe probe;
    auto client = make_client(probe, "oversize");
    client->open(std::string(kUri), "python", "x", 1);
    REQUIRE(eventually([&] { return client->stopped().has_value(); }));
    CHECK(client->stopped() == LspClient::StopReason::ProtocolError);
}

TEST_CASE("missing_program_stops_the_client") {
    Probe probe;
    LspClient client({.language = "fake", .command = {"noteapp-no-such-lsp-server"}, .root_uri = "file:///w"},
                     [&probe] { ++probe.notified; });
    client.open(std::string(kUri), "python", "x", 1);
    REQUIRE(eventually([&] { return client.stopped().has_value(); }));
    CHECK(client.stopped() == LspClient::StopReason::StartFailed);
}

TEST_CASE("workspace_configuration_request_is_answered_and_the_server_proceeds") {
    Probe probe;
    auto client = make_client(probe, "ask-config");
    open_and_wait(*client, "x");
    CHECK_FALSE(client->stopped());
}

TEST_CASE("hostile_server_data_is_contained") {
    Probe probe;
    auto client = make_client(probe, "hostile");
    client->open(std::string(kUri), "python", "line one\nline two", 1);
    REQUIRE(eventually([&] { return !client->diagnostics(std::string(kUri)).empty(); }));
    const auto list = client->diagnostics(std::string(kUri));
    // Yabancı ve "../" URI'lerin tanısı belgeye düşmez; URI bayt bayt karşılaştırılır.
    REQUIRE(list.size() == 2);
    CHECK(list[0].message == "huge");
    // Devasa satır numarası belgenin son satırına kısılır.
    CHECK(list[0].range.start.line == 1);
    CHECK(list[0].range.end.line == 1);
    CHECK(list[0].range.end.character == 8);
    // Başı sonundan sonra gelen aralık sıraya konur: UI başlangıçtan sona çizer.
    CHECK(list[1].message == "reversed");
    CHECK(list[1].range.start.character == 2);
    CHECK(list[1].range.end.character == 5);
    CHECK(client->diagnostics("file:///unknown.py").empty());

    const auto ticket = client->request_completion(std::string(kUri), 1, {});
    REQUIRE(ticket);
    const auto items = wait_completion(*client, *ticket);
    REQUIRE(items);
    CHECK(items->size() == 2);
    CHECK(std::ranges::none_of(*items, [](const lsp::CompletionItem& item) {
        return item.insert_text.size() > LspClient::kMaxCompletionInsertBytes ||
               item.label.size() > LspClient::kMaxCompletionLabelBytes;
    }));
}

TEST_CASE("shutdown_is_bounded_in_every_mode") {
    for (const std::string mode : {"normal", "hang", "deaf", "crash", "oversize", "ask-config", "utf16",
                                   "slow-completion", "hostile"}) {
        CAPTURE(mode);
        Probe probe;
        auto client = make_client(probe, mode.c_str());
        client->open(std::string(kUri), "python", "x", 1);
        // Sunucu başlasın; hang'de initialize hâlâ bekliyor, deaf hiçbir şeye cevap vermeyecek.
        std::this_thread::sleep_for(200ms);
        if (mode == "slow-completion")
            (void)client->request_completion(std::string(kUri), 1, {});  // cevap uçuştayken yık
        const auto elapsed = destroy_timed(client);
        MESSAGE(mode << " shutdown: " << elapsed.count() << " ms");
        CHECK(elapsed <= kShutdownBound);
    }
}

TEST_CASE("destructor_during_in_flight_completion_is_clean") {
    Probe probe;
    auto client = make_client(probe, "slow-completion");
    open_and_wait(*client, "x");
    REQUIRE(client->request_completion(std::string(kUri), 1, {}));
    std::this_thread::sleep_for(50ms);
    const auto elapsed = destroy_timed(client);
    MESSAGE("in-flight completion shutdown: " << elapsed.count() << " ms");
    CHECK(elapsed <= kShutdownBound);
}

TEST_CASE("unused_client_never_starts_a_server") {
    Probe probe;
    auto client = make_client(probe, "normal");
    client->close(std::string(kUri));
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(client->stopped());
    CHECK(probe.notified == 0);
}

TEST_CASE("log_carries_lifecycle_events_but_never_document_text") {
    noteapp::test::TempDir home;
    const noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    {
        Probe probe;
        auto client = make_client(probe, "normal");
        open_and_wait(*client, "SECRET-CONTENT");
    }
    const std::string log = noteapp::test::slurp(home.path() / "state" / "log.txt");
    CHECK(log.find("event=lsp_started language=fake") != std::string::npos);
    CHECK(log.find("event=lsp_stopped language=fake exit_code=0 reason=shutdown") != std::string::npos);
    CHECK(log.find("SECRET") == std::string::npos);
}

TEST_CASE("flooding_server_cannot_pin_the_worker_during_a_large_write") {
    Probe probe;
    auto client = make_client(probe, "flood");
    client->open(std::string(kUri), "python", "x", 1);
    std::this_thread::sleep_for(300ms);  // initialize bitsin, yağmur başlasın
    // Büyük metin parça parça yazılır; parçalar arasındaki boşaltma sınırsızsa işçi orada kalır.
    client->change(std::string(kUri), std::string(kLargeTextBytes, 'a'), 2);
    std::this_thread::sleep_for(300ms);
    // Yıkım ayrı iş parçacığında: takılan join testi askıda bırakmasın, düşürsün.
    auto done = std::make_shared<std::promise<std::chrono::milliseconds>>();
    auto finished = done->get_future();
    std::thread([raw = client.release(), done] {
        const auto start = Clock::now();
        delete raw;
        done->set_value(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start));
    }).detach();
    REQUIRE(finished.wait_for(kShutdownBound + 1s) == std::future_status::ready);
    const auto elapsed = finished.get();
    MESSAGE("flood shutdown: " << elapsed.count() << " ms");
    CHECK(elapsed <= kShutdownBound);
}

TEST_CASE("oversized_pending_replies_stop_the_client") {
    Probe probe;
    auto client = make_client(probe, "config-bomb");
    client->open(std::string(kUri), "python", "x", 1);
    REQUIRE(eventually([&] { return client->stopped().has_value(); }));
    CHECK(client->stopped() == LspClient::StopReason::ProtocolError);
}

TEST_CASE("many_utf16_diagnostics_on_a_huge_line_convert_in_one_walk") {
    Probe probe;
    auto client = make_client(probe, "utf16-heavy");
    const auto start = Clock::now();
    client->open(std::string(kUri), "python", std::string(kHugeLineBytes, 'a'), 1);
    REQUIRE(
        eventually([&] { return !client->diagnostics(std::string(kUri)).empty(); }, kHeavyConversionBudget));
    const auto elapsed = Clock::now() - start;
    MESSAGE("1000 utf-16 diagnostics on 2 MiB line: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
    const auto list = client->diagnostics(std::string(kUri));
    REQUIRE(list.size() == 1000);
    CHECK(list.front().range.start.character == kHugeLineBytes);
    CHECK(list.back().range.end.character == kHugeLineBytes);
    if constexpr (noteapp::test::kWallClockBudgets)
        CHECK(elapsed < kHeavyConversionBudget);
}

TEST_CASE("slow_reading_server_cannot_stretch_shutdown") {
    Probe probe;
    auto client = make_client(probe, "slow-reader");
    open_and_wait(*client, "x");
    const auto elapsed = destroy_timed(client);
    MESSAGE("slow-reader shutdown: " << elapsed.count() << " ms");
    CHECK(elapsed <= kShutdownBound);
}

#ifndef _WIN32
// log.txt bir FIFO'dur: okuyan açılana dek log yazması bloklanır (Wine'da yavaş dosya G/Ç'sinin abartısı).
// Durma bildirimi log G/Ç'sinin arkasında kalmamalı: UI'nin yeniden çizimi diske bağlı olmasın.
TEST_CASE("stop_notification_does_not_wait_for_logging") {
    noteapp::test::TempDir home;
    const noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    const auto state = home.path() / "state";
    std::filesystem::create_directories(state);
    const auto log = state / "log.txt";
    REQUIRE(mkfifo(log.c_str(), S_IRUSR | S_IWUSR) == 0);
    Probe probe;
    auto client = std::make_unique<LspClient>(LspClient::Options{.language = "fake",
                                                                 .command = {"noteapp-no-such-lsp-server"},
                                                                 .root_uri = "file:///w"},
                                              [&probe] { ++probe.notified; });
    client->open(std::string(kUri), "python", "x", 1);
    REQUIRE(eventually([&] { return client->stopped().has_value(); }));
    CHECK(eventually([&] { return probe.notified > 0; }, 1s));
    // Okuyanı açınca işçinin log yazması biter; okuyan, işçi join edilene dek açık kalır (SIGPIPE olmasın).
    const int reader = ::open(log.c_str(), O_RDONLY | O_NONBLOCK);
    REQUIRE(reader >= 0);
    client.reset();
    ::close(reader);
}
#endif
