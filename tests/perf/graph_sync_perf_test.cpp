// SPEC-graph kabul 8 (yeni cihaz) ve tasarım riski 4 (günlük büyümesi). Yalnız kamu API'si.
#include "graph/graph.hpp"
#include "graph/op.hpp"
#include "graph/oplog.hpp"
#include "support/test_support.hpp"
#include "wine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace noteapp::graph;
using noteapp::test::TempDir;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kNotes = 5000;
constexpr double kFreshDeviceBudgetMs = 10000.0;

double time_ms(const std::function<void()>& work) {
    const Clock::time_point start = Clock::now();
    work();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> samples) {
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

template <class T> T must(Result<T> result) {
    REQUIRE_MESSAGE(result.has_value(), (result.has_value() ? "" : result.error().message_key));
    return std::move(*result);
}

void must(const Result<void>& result) {
    REQUIRE_MESSAGE(result.has_value(), (result.has_value() ? "" : result.error().message_key));
}

// Yaklaşık 2 KB düzyazı; düzenleme gerçekçi: birkaç sözcük değişir.
std::string body_text(int note, int edit) {
    static const std::vector<std::string> kWords = {"graph", "merge", "device", "frame", "commit", "edit",
                                                    "kenar", "düğüm", "işlem",  "kayıt", "sürüm",  "metin"};
    std::string body;
    body.reserve(2100);
    std::uint32_t state = static_cast<std::uint32_t>(note) * 2654435761U + 7U;
    while (body.size() < 2000) {
        state = state * 1103515245U + 12345U;
        body += kWords[(state >> 16U) % kWords.size()];
        body += (state >> 8U) % 13U == 0 ? ".\n" : " ";
    }
    return body + "edit " + std::to_string(edit) + "\n";
}

NodeId note_id(int note) {
    const std::string name = std::to_string(note);
    return derive_node_id("perf", std::as_bytes(std::span(name.data(), name.size())));
}

EdgeId link_id(int note) {
    const std::string name = std::to_string(note);
    return derive_edge_id("perf", std::as_bytes(std::span(name.data(), name.size())));
}

void save_note(Graph& graph, int note, int edit) {
    WriteTxn txn = must(graph.write());
    const NodeId id = note_id(note);
    must(txn.put_node(id));
    must(txn.add_label(id, "Item"));
    must(txn.set_property(id, "name", "item " + std::to_string(note)));
    must(txn.set_property(id, "body", body_text(note, edit)));
    must(txn.set_property(id, "updated_at", Timestamp{1700000000000 + edit}));
    if (note > 0)
        must(txn.put_edge(link_id(note), id, "LINK", note_id(note - 1)));
    must(txn.commit());
}

fs::path only_log_in(const fs::path& directory) {
    for (const fs::directory_entry& entry : fs::directory_iterator(directory))
        if (entry.path().extension() == ".oplog")
            return entry.path();
    FAIL("no log in directory");
    return {};
}

void fill_with_small_frames(Graph& graph, int frames) {
    for (int i = 0; i < frames; ++i) {
        WriteTxn txn = must(graph.write());
        must(txn.put_node(note_id(i)));
        must(txn.commit());
    }
}

}  // namespace

TEST_CASE("fresh_device_5000_notes_under_10s") {
    TempDir dir;
    const fs::path outbox = dir.path() / "out";
    Graph source = must(Graph::open(dir.path() / "source.db"));
    const double author_ms = time_ms([&] {
        for (int note = 0; note < kNotes; ++note)
            save_note(source, note, 0);
    });
    must(source.export_log(outbox));
    const std::vector<fs::path> logs{only_log_in(outbox)};

    Graph fresh = must(Graph::open(dir.path() / "fresh.db"));
    MergeReport report;
    const double ingest_ms = time_ms([&] { report = must(fresh.ingest_logs(logs)); });
    std::printf("graph_sync fresh_device_notes=%d log_bytes=%ju author_ms=%.0f ingest_ms=%.0f\n", kNotes,
                fs::file_size(logs[0]), author_ms, ingest_ms);
    CHECK(report.bad_frames.empty());
    CHECK(report.stalled.empty());
    CHECK(ingest_ms <= kFreshDeviceBudgetMs);
    ReadTxn txn = must(fresh.read());
    CHECK(*txn.has_node(note_id(kNotes - 1)));
    CHECK(*txn.property(note_id(kNotes - 1), "body") == Value{body_text(kNotes - 1, 0)});
}

// Nadir yol ölçülür: silmeyle çakışan tek uzak yazma tüm graph'ı ops'tan yeniden kurar.
TEST_CASE("rebuild_and_session_cost_at_5000_notes") {
    TempDir dir;
    const fs::path out_a = dir.path() / "a";
    const fs::path out_b = dir.path() / "b";
    Graph a = must(Graph::open(dir.path() / "a.db"));
    for (int note = 0; note < kNotes; ++note)
        save_note(a, note, 0);
    must(a.export_log(out_a));
    Graph b = must(Graph::open(dir.path() / "b.db"));
    const std::vector<fs::path> log_a{only_log_in(out_a)};
    must(b.ingest_logs(log_a));

    // Sıradan uzak düzenleme: yeniden kurma yok, yalnız oturumun ops taraması.
    save_note(a, 10, 1);
    must(a.export_log(out_a));
    const double plain_ingest_ms = time_ms([&] { must(b.ingest_logs(log_a)); });
    const double noop_ingest_ms = time_ms([&] { must(b.ingest_logs(log_a)); });

    // A silmeyi görmeden düzenler, B siler: B'de düzenleme gelince silme etkisiz kalır.
    {
        WriteTxn txn = must(b.write());
        must(txn.delete_node(note_id(20), true));
        must(txn.commit());
    }
    save_note(a, 20, 1);
    must(a.export_log(out_a));
    const double rebuild_ingest_ms = time_ms([&] { must(b.ingest_logs(log_a)); });
    std::printf("graph_sync plain_ingest_one_frame_ms=%.1f noop_ingest_ms=%.1f rebuild_ingest_ms=%.1f\n",
                plain_ingest_ms, noop_ingest_ms, rebuild_ingest_ms);
    ReadTxn txn = must(b.read());
    CHECK(*txn.has_node(note_id(20)));
    // Yeniden kurma, yeni cihaz kurulumundan pahalı olamaz; aynı bütçe.
    CHECK(rebuild_ingest_ms <= kFreshDeviceBudgetMs);
}

// Tarama yalnız ingest'tedir: uygulama açılışı ve sıradan yazma günlük büyüdükçe yavaşlamaz.
TEST_CASE("graph_open_and_plain_write_do_not_grow_with_ops") {
    constexpr int kSmallFrames = 1000;
    constexpr int kLargeFrames = 20000;
    constexpr int kRuns = 41;
    // Mikrosaniye düzeyinde ölçülür: mutlak pay ölçüm gürültüsü içindir, doğrusal büyüme 20 kat olurdu.
    constexpr double kAllowedRatio = 2.0;
    constexpr double kNoiseMs = 2.0;

    TempDir dir;
    const fs::path small_path = dir.path() / "small.db";
    const fs::path large_path = dir.path() / "large.db";
    for (const auto& [path, frames] :
         {std::pair{small_path, kSmallFrames}, std::pair{large_path, kLargeFrames}}) {
        Graph graph = must(Graph::open(path));
        fill_with_small_frames(graph, frames);
    }
    // T93: örnekler küçük ve büyük günlük arasında dönüşümlü alınır. Wine'da yazma süresi zaman zaman bir
    // süreliğine katlanır (41 örneklik medyan 2,6 ms'den 11 ms'ye); arka arkaya ölçülen iki medyandan biri bu
    // döneme denk gelirse oran büyüme sanılıyordu. Dönüşümlü örnekte yavaş dönem iki tarafı birlikte etkiler.
    std::vector<double> small_opens;
    std::vector<double> large_opens;
    for (int run = 0; run < kRuns; ++run) {
        small_opens.push_back(time_ms([&] { must(Graph::open(small_path)); }));
        large_opens.push_back(time_ms([&] { must(Graph::open(large_path)); }));
    }
    Graph small_graph = must(Graph::open(small_path));
    Graph large_graph = must(Graph::open(large_path));
    int edit = 0;
    const auto edit_once = [&](Graph& graph) {
        return time_ms([&] {
            WriteTxn txn = must(graph.write());
            must(txn.set_property(note_id(0), "k", std::int64_t{++edit}));
            must(txn.commit());
        });
    };
    std::vector<double> small_writes;
    std::vector<double> large_writes;
    for (int run = 0; run < kRuns; ++run) {
        small_writes.push_back(edit_once(small_graph));
        large_writes.push_back(edit_once(large_graph));
    }
    // Yerli: medyan. T93: Wine'da dönüşümlü örnekte de büyük günlüğün medyanı tek başına 2,8 ms'den 11,4
    // ms'ye çıkabiliyor (yavaşlık yalnız bir dosyaya denk geliyor). Wine'da en hızlı örnek karşılaştırılır:
    // günlüğe bağlı gerçek bir maliyet her örnekte, en hızlısında da vardır (her yazmada 8 tam günlük
    // taraması: en hızlılar 1,0'a karşı 5,1 ms, düşer).
    const bool wine = noteapp::test::running_under_wine();
    const auto pick = [wine](const std::vector<double>& samples) {
        return wine ? std::ranges::min(samples) : median(samples);
    };
    const double small_open = pick(small_opens);
    const double large_open = pick(large_opens);
    const double small_write = pick(small_writes);
    const double large_write = pick(large_writes);
    std::printf("graph_sync %s open_ms frames_%d=%.3f frames_%d=%.3f write_ms frames_%d=%.3f frames_%d=%.3f; "
                "write medians %.3f %.3f\n",
                wine ? "min" : "median", kSmallFrames, small_open, kLargeFrames, large_open, kSmallFrames,
                small_write, kLargeFrames, large_write, median(small_writes), median(large_writes));
    CHECK(large_open <= small_open * kAllowedRatio + kNoiseMs);
    CHECK(large_write <= small_write * kAllowedRatio + kNoiseMs);
}

// Karar girdisi: tam gövdeyle her kayıt ne kadar büyütüyor. Sıkıştırma v1'de yok, sayılar bunun bedeli.
TEST_CASE("log_growth_for_100_edits_of_a_2_kb_item") {
    constexpr int kEdits = 100;
    TempDir dir;
    const fs::path outbox = dir.path() / "out";
    const fs::path db = dir.path() / "graph.db";
    std::uintmax_t first_save = 0;
    {
        Graph graph = must(Graph::open(db));
        save_note(graph, 0, 0);
        must(graph.export_log(outbox));
        first_save = fs::file_size(only_log_in(outbox));
        for (int edit = 1; edit <= kEdits; ++edit) {
            WriteTxn txn = must(graph.write());
            must(txn.set_property(note_id(0), "body", body_text(0, edit)));
            must(txn.set_property(note_id(0), "updated_at", Timestamp{1700000000000 + edit}));
            must(txn.commit());
        }
        must(graph.export_log(outbox));
    }
    const fs::path log = only_log_in(outbox);
    const std::uintmax_t log_bytes = fs::file_size(log);
    const std::uintmax_t db_bytes = fs::file_size(db);
    std::printf("graph_sync growth edits=%d body_bytes=%zu first_save_bytes=%ju log_bytes=%ju "
                "per_edit_bytes=%ju db_bytes=%ju\n",
                kEdits, body_text(1, 0).size(), first_save, log_bytes, (log_bytes - first_save) / kEdits,
                db_bytes);
    // Tam gövde: düzenleme başına en az bir gövde, sabit ek yük de sınırlı kalmalı.
    CHECK((log_bytes - first_save) / kEdits >= body_text(1, 0).size());
    CHECK((log_bytes - first_save) / kEdits <= body_text(1, 0).size() + 200);
    if (const char* keep = std::getenv("NOTEAPP_PERF_KEEP_LOG"))
        fs::copy_file(log, keep, fs::copy_options::overwrite_existing);
}

// Bulgu 4: düşmanca log. Aynı kenara tekrarlanan put, aynı düğüme binlerce yazma ve silme. Süre bütçesi
// kuadratik davranışı yakalar; eşzamanlı yerel yazıcı, batch'lerin yazma kilidini gerçekten bıraktığını
// gösterir.
TEST_CASE("adversarial_log_stays_within_budget_and_releases_the_writer_lock") {
    constexpr int kEdgePuts = 30000;
    constexpr int kNodeOps = 20000;
    constexpr double kBudgetMs = 20000.0;
    using namespace noteapp::graph::detail;

    const NodeId node = note_id(1);
    const NodeId other = note_id(2);
    const EdgeId edge = link_id(1);
    const DeviceId hostile{std::byte{0xAA}};
    const DeviceId rival{std::byte{0xBB}};
    const auto record_of = [](std::uint64_t seq, std::vector<Op> ops) {
        Frame frame;
        frame.seq = seq;
        frame.hlc = seq;
        frame.ops = std::move(ops);
        return encode_log_part(seq, 0, true, encode_frame(frame));
    };
    TempDir dir;
    const fs::path hostile_log = dir.path() / log_file_name(hostile);
    const fs::path rival_log = dir.path() / log_file_name(rival);
    {
        std::ofstream out(hostile_log, std::ios::binary);
        out << encode_log_header(hostile);
        std::uint64_t seq = 0;
        out << record_of(++seq, {NodePut{node}, NodePut{other}});
        for (int i = 0; i < kEdgePuts; ++i)
            out << record_of(++seq, {EdgePut{edge, node, "A", other}});
        for (int i = 0; i < kNodeOps; ++i)
            out << record_of(++seq, {PropSet{node, "k", std::int64_t{i}}});
        REQUIRE(out.good());
    }
    {
        // Yazmaları görmeyen silmeler ve aynı kenara rakip kimlik: yeniden kurmayı tetikler, kararlar
        // pahalılaşır.
        std::ofstream out(rival_log, std::ios::binary);
        out << encode_log_header(rival);
        std::uint64_t seq = 0;
        for (int i = 0; i < kNodeOps; ++i)
            out << record_of(++seq, {NodeDel{node}});
        out << record_of(++seq, {EdgePut{edge, node, "B", other}});
        REQUIRE(out.good());
    }

    const fs::path db = dir.path() / "graph.db";
    Graph target = must(Graph::open(db));
    std::atomic<bool> finished = false;
    double slowest_write_ms = 0;
    int failed_writes = 0;
    int writes = 0;
    std::thread writer([&] {
        Result<Graph> local = Graph::open(db);
        REQUIRE(local.has_value());
        while (!finished) {
            const double elapsed = time_ms([&] {
                Result<WriteTxn> txn = local->write();
                if (!txn || !txn->put_node(note_id(3)) || !txn->commit())
                    ++failed_writes;
            });
            slowest_write_ms = std::max(slowest_write_ms, elapsed);
            ++writes;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const std::vector<fs::path> logs{hostile_log, rival_log};
    Result<MergeReport> report = MergeReport{};
    const double ingest_ms = time_ms([&] { report = target.ingest_logs(logs); });
    finished = true;
    writer.join();
    std::printf(
        "graph_sync adversarial_ingest_ms=%.0f concurrent_writes=%d slowest_write_ms=%.0f failed=%d\n",
        ingest_ms, writes, slowest_write_ms, failed_writes);
    REQUIRE(report.has_value());
    CHECK(ingest_ms <= kBudgetMs);
    CHECK(failed_writes == 0);
    CHECK(writes >= 2);  // kilit hiç bırakılmasaydı ilk yazma ingest bitene dek beklerdi
}
