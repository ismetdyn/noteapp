// Cihazlar arası gerçek taşıma: her cihazın kendi graph.db'si var, aralarında yalnız oplog dosyaları dolaşır.
#include "graph/id.hpp"
#include "graph/op.hpp"
#include "graph/oplog.hpp"
#include "sync_test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sqlite3.h>
#include <string>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;
using noteapp::test::slurp;
using noteapp::test::TempDir;
using test::as_bytes;
using test::begin_read;
using test::begin_write;
using test::Device;
using test::edge_named;
using test::node_named;
using test::open_graph;
using test::RawDb;
using test::write_file;
using test::write_log;

namespace fs = std::filesystem;

namespace {

const NodeId kNode = node_named("n");
const NodeId kOther = node_named("m");
const EdgeId kEdge = edge_named("e");

void create_node(WriteTxn& txn) {
    GRAPH_OK(txn.put_node(kNode));
}

std::vector<Version> versions_of(Graph& graph, const OwnerId& owner, std::string_view key) {
    ReadTxn txn = begin_read(graph);
    Result<std::vector<Version>> versions = txn.versions(owner, key);
    REQUIRE(versions.has_value());
    return std::move(*versions);
}

bool has_node(Graph& graph, const NodeId& id) {
    ReadTxn txn = begin_read(graph);
    const Result<bool> found = txn.has_node(id);
    REQUIRE(found.has_value());
    return *found;
}

}  // namespace

TEST_CASE("concurrent_set_keeps_both_heads_in_any_order") {
    Device a;
    Device b;
    a.commit(create_node);
    CHECK(b.ingest({a.exported()}).bad_frames.empty());
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kNode, "x", std::string("from a"))); });
    b.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kNode, "x", std::string("from b"))); });
    const fs::path log_a = a.exported();
    const fs::path log_b = b.exported();

    Device together;
    const MergeReport report = together.ingest({log_a, log_b});
    REQUIRE(report.conflicts.size() == 1);
    CHECK(report.conflicts[0].key == "x");
    CHECK(std::get<NodeId>(report.conflicts[0].owner) == kNode);
    CHECK(versions_of(together.graph(), kNode, "x").size() == 2);

    Device reversed;
    reversed.ingest({log_b, log_a});
    Device one_by_one;
    one_by_one.ingest({log_a});
    one_by_one.ingest({log_b});
    Device waits_then_recovers;  // B, A'nın ilk çerçevesine bağlı: tek başına durur, A gelince ilerler
    CHECK(waits_then_recovers.ingest({log_b}).stalled.size() == 1);
    waits_then_recovers.ingest({log_b, log_a});

    a.ingest({log_b, log_a});
    b.ingest({log_a, log_b});
    CHECK(reversed.dump() == together.dump());
    CHECK(one_by_one.dump() == together.dump());
    CHECK(waits_then_recovers.dump() == together.dump());
    CHECK(a.dump() == together.dump());
    CHECK(b.dump() == together.dump());

    // Yeni yazma iki head'i de görür ve yerine geçer.
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kNode, "x", std::string("resolved"))); });
    CHECK(versions_of(a.graph(), kNode, "x").size() == 1);
}

TEST_CASE("delete_vs_edit_converges_in_any_order") {
    Device a;
    Device b;
    a.commit([](WriteTxn& txn) {
        create_node(txn);
        GRAPH_OK(txn.add_label(kNode, "Keep"));
    });
    b.ingest({a.exported()});
    b.commit([](WriteTxn& txn) { GRAPH_OK(txn.delete_node(kNode, true)); });
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kNode, "x", std::int64_t{1})); });
    const fs::path log_a = a.exported();
    const fs::path log_b = b.exported();
    CHECK_FALSE(has_node(b.graph(), kNode));

    Device together;
    together.ingest({log_a, log_b});
    Device reversed;
    reversed.ingest({log_b, log_a});
    Device delete_first;
    delete_first.ingest({log_a});
    delete_first.ingest({log_b});

    a.ingest({log_b});
    b.ingest({log_a});
    CHECK(has_node(b.graph(), kNode));  // silmenin görmediği düzenleme düğümü geri getirir
    CHECK(reversed.dump() == together.dump());
    CHECK(delete_first.dump() == together.dump());
    CHECK(a.dump() == together.dump());
    CHECK(b.dump() == together.dump());
    CHECK(has_node(together.graph(), kNode));
}

TEST_CASE("partial_tail_applies_on_next_ingest") {
    Device a;
    a.commit(create_node);
    const std::uintmax_t first_frame_end = fs::file_size(a.exported());
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kNode, "x", std::int64_t{1})); });
    const std::string full = slurp(a.exported());
    REQUIRE(full.size() > first_frame_end + 4);

    Device b;
    const fs::path inbox = b.dir() / a.log_path().filename();
    write_file(inbox, std::string_view(full).substr(0, first_frame_end + 4));
    MergeReport partial = b.ingest({inbox});
    CHECK(partial.bad_frames.empty());
    CHECK(partial.stalled.empty());
    CHECK(has_node(b.graph(), kNode));
    CHECK(versions_of(b.graph(), kNode, "x").empty());

    write_file(inbox, full);
    CHECK(b.ingest({inbox}).bad_frames.empty());
    CHECK(b.dump() == a.dump());
}

TEST_CASE("crash_between_commit_and_export_loses_nothing") {
    Device a;
    a.commit(create_node);
    const std::uintmax_t exported_size = fs::file_size(a.exported());
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kNode, "x", std::int64_t{1})); });
    a.close();  // commit oldu, export olmadı
    a.reopen();
    const std::string full = slurp(a.exported());
    CHECK(full.size() > exported_size);
    Device fresh;
    fresh.ingest({a.log_path()});
    CHECK(fresh.dump() == a.dump());

    // Dosyaya yazarken her çökme noktası: sonraki export aynı baytlara varır.
    for (std::size_t size = 0; size < full.size(); ++size) {
        INFO("size=" << size);
        write_file(a.log_path(), std::string_view(full).substr(0, size));
        CHECK(slurp(a.exported()) == full);
    }
    fs::remove(a.log_path());
    const fs::path regenerated = a.exported();
    REQUIRE_FALSE(regenerated.empty());
    CHECK(slurp(regenerated) == full);
}

TEST_CASE("export_with_nothing_new_leaves_the_file_untouched") {
    Device a;
    a.commit(create_node);
    const fs::path log = a.exported();
    const auto written_at = fs::last_write_time(log);
    const std::string before = slurp(log);
    a.exported();
    CHECK(slurp(log) == before);
    CHECK(fs::last_write_time(log) == written_at);
}

TEST_CASE("reapplying_any_log_changes_nothing") {
    Device a;
    a.commit(create_node);
    a.commit([](WriteTxn& txn) {
        GRAPH_OK(txn.add_label(kNode, "L"));
        GRAPH_OK(txn.set_property(kNode, "x", std::int64_t{1}));
    });
    const fs::path log = a.exported();
    const std::string a_before = a.dump();
    const MergeReport own = a.ingest({log});
    CHECK(own.bad_frames.empty());
    CHECK(own.stalled.empty());
    CHECK(a.dump() == a_before);

    Device b;
    b.ingest({log});
    const std::string once = b.dump();
    RawDb raw(b.db());
    const std::int64_t ops_once = raw.scalar("SELECT count(*) FROM ops");
    REQUIRE(ops_once == 2);
    const MergeReport again = b.ingest({log, log});
    CHECK(again.bad_frames.empty());
    CHECK(again.stalled.empty());
    CHECK(again.conflicts.empty());
    CHECK(b.dump() == once);
    CHECK(raw.scalar("SELECT count(*) FROM ops") == ops_once);
}

TEST_CASE("log_name_must_match_the_device_in_its_header") {
    Device a;
    a.commit(create_node);
    const fs::path log = a.exported();
    Device b;
    const fs::path renamed = b.dir() / "00000000000000000000000000000000.oplog";
    fs::copy_file(log, renamed);
    const MergeReport report = b.ingest({renamed});
    REQUIRE(report.bad_frames.size() == 1);
    CHECK(report.bad_frames[0].reason == "device_name_mismatch");
    CHECK_FALSE(has_node(b.graph(), kNode));
}

TEST_CASE("same_device_and_seq_with_other_bytes_is_a_bad_frame") {
    const DeviceId forged{std::byte{9}};
    Frame frame;
    frame.seq = 1;
    frame.hlc = 5;
    frame.ops.emplace_back(NodePut{kNode});
    const std::string first = encode_log_header(forged) + test::whole_record(encode_frame(frame));
    frame.ops[0] = NodePut{kOther};
    const std::string second = encode_log_header(forged) + test::whole_record(encode_frame(frame));

    Device b;
    const fs::path inbox = b.dir() / log_file_name(forged);
    write_file(inbox, first);
    CHECK(b.ingest({inbox}).bad_frames.empty());
    const std::string dump = b.dump();

    write_file(inbox, second);
    const MergeReport report = b.ingest({inbox});
    REQUIRE(report.bad_frames.size() == 1);
    CHECK(report.bad_frames[0].seq == 1);
    CHECK(report.bad_frames[0].reason == "seq_digest_mismatch");
    CHECK(b.dump() == dump);
}

namespace {

struct ThreeFrames {
    std::string log;
    fs::path name;
    std::vector<std::string> prefix_dumps;  // i çerçeve uygulanmış halin dump'ı
    std::vector<std::size_t> frame_ends;
};

ThreeFrames three_frames(Device& a) {
    ThreeFrames result;
    Device empty;
    result.prefix_dumps.push_back(empty.dump());
    const auto step = [&](const auto& change) {
        a.commit(change);
        const std::string log = slurp(a.exported());
        result.frame_ends.push_back(log.size());
        result.log = log;
        result.prefix_dumps.push_back(a.dump());
    };
    step(create_node);
    step([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "L")); });
    step([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kNode, "x", std::int64_t{1})); });
    result.name = a.log_path().filename();
    return result;
}

}  // namespace

TEST_CASE("truncation_at_every_byte_applies_only_whole_frames") {
    Device a;
    const ThreeFrames frames = three_frames(a);
    for (std::size_t size = 0; size < frames.log.size(); ++size) {
        INFO("size=" << size);
        Device b;
        const fs::path inbox = b.dir() / frames.name;
        write_file(inbox, std::string_view(frames.log).substr(0, size));
        const MergeReport report = b.ingest({inbox});
        CHECK(report.bad_frames.empty());
        const auto whole =
            std::ranges::count_if(frames.frame_ends, [&](std::size_t end) { return end <= size; });
        CHECK(b.dump() == frames.prefix_dumps[static_cast<std::size_t>(whole)]);
    }
}

TEST_CASE("byte_flip_sweep_through_ingest_applies_a_valid_prefix_only") {
    Device a;
    const ThreeFrames frames = three_frames(a);
    int reported = 0;
    for (std::size_t at = 0; at < frames.log.size(); ++at) {
        INFO("at=" << at);
        std::string damaged = frames.log;
        damaged[at] = static_cast<char>(static_cast<unsigned char>(damaged[at]) ^ 0x10U);
        Device b;
        const fs::path inbox = b.dir() / frames.name;
        write_file(inbox, damaged);
        const MergeReport report = b.ingest({inbox});
        reported += report.bad_frames.empty() ? 0 : 1;
        CHECK(std::ranges::contains(frames.prefix_dumps, b.dump()));
        CHECK(b.dump() != frames.prefix_dumps.back());
    }
    CHECK(reported > 0);
}

TEST_CASE("stalled_log_is_reported_and_recovers_from_the_source_ops") {
    Device a;
    const ThreeFrames frames = three_frames(a);

    // C, A'nın tamamını görüp yazar: çerçevesi A'nın bozuk kısmına bağlıdır.
    Device c;
    c.ingest({a.log_path()});
    c.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "FromC")); });
    const fs::path log_c = c.exported();

    std::string damaged = frames.log;
    damaged[frames.frame_ends[1] - 1] ^= 0x01;  // ikinci çerçevenin son baytı
    write_file(a.log_path(), damaged);

    Device b;
    const MergeReport first = b.ingest({a.log_path(), log_c});
    REQUIRE(first.bad_frames.size() == 1);
    CHECK(first.stalled.size() == 2);
    CHECK(b.dump() == frames.prefix_dumps[1]);

    // Kaynak kendi dosyasını ops'tan yeniden üretir.
    CHECK(slurp(a.exported()) == frames.log);
    const MergeReport second = b.ingest({a.log_path(), log_c});
    CHECK(second.bad_frames.empty());
    CHECK(second.stalled.empty());
    CHECK(b.dump() == c.dump());
}

TEST_CASE("missing_dependency_alone_reports_stalled_without_a_bad_frame") {
    Device a;
    a.commit(create_node);
    const fs::path log_a = a.exported();
    Device c;
    c.ingest({log_a});
    c.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "L")); });

    Device b;
    const MergeReport waiting = b.ingest({c.exported()});
    CHECK(waiting.bad_frames.empty());
    REQUIRE(waiting.stalled.size() == 1);
    CHECK(waiting.stalled[0].reason == "missing_dependency");
    CHECK_FALSE(has_node(b.graph(), kNode));

    CHECK(b.ingest({c.log_path(), log_a}).stalled.empty());
    CHECK(b.dump() == c.dump());
}

TEST_CASE("bytes_values_survive_the_log_exactly") {
    const std::string special("\0\r\n\xEF\xBB\xBF\xFF\xFE line\r\n\0", 17);
    const std::string large(10U * 1024U * 1024U, '\xC3');
    Device a;
    a.commit([&](WriteTxn& txn) {
        create_node(txn);
        GRAPH_OK(txn.set_property(kNode, "special", Bytes{special}));
        GRAPH_OK(txn.set_property(kNode, "large", Bytes{large}));
        GRAPH_OK(txn.set_property(kNode, "text", std::string("metin \xC3\xBC")));
    });
    Device b;
    CHECK(b.ingest({a.exported()}).bad_frames.empty());
    ReadTxn txn = begin_read(b.graph());
    CHECK(*txn.property(kNode, "special") == Value{Bytes{special}});
    const Result<Value> read_large = txn.property(kNode, "large");
    REQUIRE(read_large.has_value());
    CHECK(std::get<Bytes>(*read_large).data == large);
    CHECK(*txn.property(kNode, "text") == Value{std::string("metin \xC3\xBC")});
}

TEST_CASE("restored_old_database_catches_up_from_its_own_log") {
    Device a;
    a.commit(create_node);
    a.close();
    const fs::path backup = a.dir() / "backup.db";
    fs::copy_file(a.db(), backup);
    a.reopen();
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "After")); });
    const std::string log_before = slurp(a.exported());
    const std::string dump_before = a.dump();

    a.close();
    fs::copy_file(backup, a.db(), fs::copy_options::overwrite_existing);
    fs::remove(a.db().string() + "-wal");
    fs::remove(a.db().string() + "-shm");
    a.reopen();
    CHECK(a.ingest({a.log_path()}).bad_frames.empty());
    CHECK(a.dump() == dump_before);
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "Later")); });
    const std::string extended = slurp(a.exported());
    CHECK(extended.starts_with(log_before));
    Device fresh;
    CHECK(fresh.ingest({a.log_path()}).bad_frames.empty());
    CHECK(fresh.dump() == a.dump());
}

TEST_CASE("copied_database_on_a_second_device_is_detected_and_not_corrupting") {
    Device a;
    a.commit(create_node);
    (void)a.exported();
    a.close();
    Device twin;
    twin.close();
    fs::copy_file(a.db(), twin.db(), fs::copy_options::overwrite_existing);
    fs::remove(twin.db().string() + "-wal");
    fs::remove(twin.db().string() + "-shm");
    a.reopen();
    twin.reopen();

    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "OnA")); });
    twin.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "OnTwin")); });
    const fs::path log_a = a.exported();
    const fs::path log_twin = twin.exported();
    REQUIRE(log_a.filename() == log_twin.filename());

    const std::string dump_a = a.dump();
    const MergeReport report = a.ingest({log_twin});
    REQUIRE(report.bad_frames.size() == 1);
    CHECK(report.bad_frames[0].reason == "seq_digest_mismatch");
    CHECK(a.dump() == dump_a);

    // İkizin günlüğü bu cihazın dosyasının üzerine gelirse (git) export onu kenara alır, kendininkini üretir.
    const std::string twin_bytes = slurp(log_twin);
    write_file(log_a, twin_bytes);
    const Result<ExportReport> exported = a.graph().export_log(a.outbox());
    REQUIRE(exported.has_value());
    REQUIRE(exported->set_aside.has_value());
    CHECK(exported->reason == "device_forked");
    CHECK(slurp(*exported->set_aside) == twin_bytes);
    Device fresh;
    CHECK(fresh.ingest({log_a}).bad_frames.empty());
    CHECK(fresh.dump() == dump_a);
}

// Sıra bozulursa (ingest'ten önce export) günlük kaybolmaz: bayt bayt kenarda kalır ve raporlanır.
TEST_CASE("export_before_ingest_on_a_restored_database_sets_the_log_aside") {
    Device a;
    a.commit(create_node);
    a.close();
    const fs::path backup = a.dir() / "backup.db";
    fs::copy_file(a.db(), backup);
    a.reopen();
    a.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(kNode, "After")); });
    const std::string log_before = slurp(a.exported());

    a.close();
    fs::copy_file(backup, a.db(), fs::copy_options::overwrite_existing);
    fs::remove(a.db().string() + "-wal");
    fs::remove(a.db().string() + "-shm");
    a.reopen();
    const Result<ExportReport> behind = a.graph().export_log(a.outbox());
    REQUIRE(behind.has_value());
    REQUIRE(behind->set_aside.has_value());
    CHECK(behind->reason == "log_ahead_of_store");
    CHECK(slurp(*behind->set_aside) == log_before);
}

namespace {

// Dürüst yazar: yalnız kamu API'si. Kenar kimlikleri uçlara bağlıdır, tablo sabittir.
struct EdgePlan {
    EdgeId id;
    NodeId src;
    NodeId dst;
};

class RandomWriter {
public:
    explicit RandomWriter(std::uint32_t seed) : random_(seed) {
        for (int i = 0; i < 5; ++i)
            nodes_.push_back(node_named("node" + std::to_string(i)));
        for (int i = 0; i < 4; ++i)
            edges_.push_back({edge_named("edge" + std::to_string(i)), nodes_[static_cast<std::size_t>(i)],
                              nodes_[static_cast<std::size_t>(i + 1)]});
    }

    std::size_t pick(std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(random_);
    }

    void change(Device& device) {
        device.commit([&](WriteTxn& txn) {
            const std::size_t ops = 1 + pick(3);
            for (std::size_t i = 0; i < ops; ++i)
                one_op(txn);
        });
    }

private:
    bool alive(WriteTxn& txn, const NodeId& node) { return *txn.has_node(node); }

    void one_op(WriteTxn& txn) {
        const NodeId node = nodes_[pick(nodes_.size())];
        const bool has = alive(txn, node);
        switch (pick(8)) {
        case 0:
            GRAPH_OK(txn.put_node(node));
            break;
        case 1:
            if (has)
                GRAPH_OK(txn.delete_node(node, true));
            break;
        case 2:
            if (has)
                GRAPH_OK(txn.add_label(node, "L" + std::to_string(pick(2))));
            break;
        case 3:
            if (has)
                GRAPH_OK(txn.remove_label(node, "L" + std::to_string(pick(2))));
            break;
        case 4:
            if (has)
                GRAPH_OK(txn.set_property(node, "k" + std::to_string(pick(2)), std::int64_t(pick(100))));
            break;
        default:
            edge_op(txn, edges_[pick(edges_.size())]);
            break;
        }
    }

    void edge_op(WriteTxn& txn, const EdgePlan& edge) {
        switch (pick(3)) {
        case 0:
            if (alive(txn, edge.src) && alive(txn, edge.dst))
                GRAPH_OK(txn.put_edge(edge.id, edge.src, "REL", edge.dst));
            break;
        case 1: {
            const Result<void> removed = txn.delete_edge(edge.id);
            if (!removed)
                REQUIRE(removed.error().code == Error::Code::NotFound);
            break;
        }
        default: {
            const Result<void> set = txn.set_property(edge.id, "w", std::int64_t(pick(100)));
            if (!set)
                REQUIRE(set.error().code == Error::Code::NotFound);
            break;
        }
        }
    }

    std::mt19937 random_;
    std::vector<NodeId> nodes_;
    std::vector<EdgePlan> edges_;
};

// Gerçek dosya taşıma: rastgele cihaz yazar, rastgele cihaz rastgele günlük alt kümesini, kimi kez yarım
// kopya olarak alır. Sonunda hepsi her şeyi alır; yeni cihaz da aynı yere varmalıdır.
void run_random_schedule(int device_count, std::uint32_t seed) {
    INFO("devices=" << device_count << " seed=" << seed);
    RandomWriter writer(seed);
    std::vector<std::unique_ptr<Device>> devices;
    for (int i = 0; i < device_count; ++i)
        devices.push_back(std::make_unique<Device>());
    const TempDir transit;

    const auto logs = [&] {
        std::vector<fs::path> paths;
        for (const auto& device : devices)
            if (!device->log_path().empty())
                paths.push_back(device->log_path());
        return paths;
    };
    const auto deliver_some = [&](Device& to) {
        std::vector<fs::path> chosen;
        for (const fs::path& log : logs()) {
            if (writer.pick(2) == 0)
                continue;
            if (writer.pick(3) == 0) {
                const std::string bytes = slurp(log);
                const fs::path cut = transit.path() / std::to_string(writer.pick(1000000)) / log.filename();
                fs::create_directories(cut.parent_path());
                write_file(cut, std::string_view(bytes).substr(0, writer.pick(bytes.size() + 1)));
                chosen.push_back(cut);
            } else {
                chosen.push_back(log);
            }
        }
        std::ranges::shuffle(chosen, std::mt19937(seed + static_cast<std::uint32_t>(chosen.size())));
        const MergeReport report = to.ingest(chosen);
        REQUIRE(report.bad_frames.empty());
    };

    for (int step = 0; step < 30; ++step) {
        Device& device = *devices[writer.pick(devices.size())];
        if (writer.pick(3) == 0) {
            deliver_some(device);
        } else {
            writer.change(device);
            (void)device.graph().export_log(device.outbox());
        }
    }
    for (const auto& device : devices)
        REQUIRE(device->graph().export_log(device->outbox()).has_value());
    std::vector<fs::path> all = logs();
    for (const auto& device : devices) {
        std::ranges::shuffle(all, std::mt19937(seed));
        const MergeReport report = device->ingest(all);
        REQUIRE(report.bad_frames.empty());
        REQUIRE(report.stalled.empty());
    }
    const std::string expected = devices[0]->dump();
    for (const auto& device : devices)
        REQUIRE(device->dump() == expected);
    if (all.empty())
        return;
    Device fresh;
    std::ranges::shuffle(all, std::mt19937(seed + 1));
    const MergeReport report = fresh.ingest(all);
    REQUIRE(report.stalled.empty());
    REQUIRE(fresh.dump() == expected);
}

// Elle kurulmuş kötü niyetli günlükler: görmediği düğüme ve kenara yazma, aynı kenar kimliğiyle farklı uçlar,
// görmediği işlemi silme. Doğrulamadan geçer; yakınsamayı bozamaz.
Op hostile_op(std::mt19937& random) {
    const auto pick = [&](std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(random);
    };
    const NodeId node = node_named("h" + std::to_string(pick(3)));
    const NodeId other = node_named("h" + std::to_string(pick(3)));
    const EdgeId edge = edge_named("he" + std::to_string(pick(2)));
    switch (pick(8)) {
    case 0:
        return NodePut{node};
    case 1:
        return NodeDel{node};
    case 2:
        return LabelAdd{node, "L"};
    case 3:
        return LabelDel{node, "L"};
    case 4:
        return EdgePut{edge, node, pick(2) == 0 ? "A" : "B", other};
    case 5:
        return EdgeDel{edge};
    case 6:
        return PropSet{edge, "w", std::int64_t(pick(10))};
    default:
        return PropSet{node, "k", std::int64_t(pick(10))};
    }
}

struct HostileLogs {
    std::vector<DeviceId> devices;
    std::vector<std::vector<std::string>> frames;  // cihaz başına payload
};

HostileLogs hostile_logs(std::mt19937& random) {
    const auto pick = [&](std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(random);
    };
    HostileLogs logs;
    for (int i = 0; i < 3; ++i)
        logs.devices.push_back(DeviceId{static_cast<std::byte>(i + 1)});
    logs.frames.resize(logs.devices.size());
    std::vector<std::uint64_t> latest(logs.devices.size(), 0);
    for (int step = 0; step < 12; ++step) {
        const std::size_t author = pick(logs.devices.size());
        Frame frame;
        frame.seq = ++latest[author];
        frame.hlc = pick(1U << 20U);
        for (std::size_t other = 0; other < logs.devices.size(); ++other)
            if (other != author && latest[other] > 0 && pick(2) == 0)
                frame.deps.push_back(Dot{logs.devices[other], 1 + pick(latest[other])});
        const std::size_t ops = 1 + pick(3);
        for (std::size_t i = 0; i < ops; ++i)
            frame.ops.push_back(hostile_op(random));
        logs.frames[author].push_back(encode_frame(frame));
    }
    return logs;
}

// Uygulanan durum boşsa false: boş dump'ların eşitliği hiçbir şey kanıtlamaz.
bool run_hostile_schedule(std::uint32_t seed) {
    INFO("seed=" << seed);
    std::mt19937 random(seed);
    const HostileLogs logs = hostile_logs(random);
    const TempDir transit;
    std::vector<fs::path> full;
    for (std::size_t i = 0; i < logs.devices.size(); ++i)
        full.push_back(write_log(transit.path() / "full", logs.devices[i], logs.frames[i]));

    std::string expected;
    for (int receiver = 0; receiver < 4; ++receiver) {
        Device device;
        // Önce rastgele önekler, kendi oturumlarında: çerçeveler farklı sırayla ve farklı oturumlarla gelir.
        for (int round = 0; round < 4; ++round) {
            std::vector<fs::path> prefixes;
            for (std::size_t i = 0; i < logs.devices.size(); ++i) {
                const std::size_t count =
                    std::uniform_int_distribution<std::size_t>(0, logs.frames[i].size())(random);
                prefixes.push_back(
                    write_log(transit.path() / std::to_string(receiver) / std::to_string(round),
                              logs.devices[i], std::span(logs.frames[i]).first(count)));
            }
            std::ranges::shuffle(prefixes, random);
            (void)device.ingest(prefixes);
        }
        std::vector<fs::path> shuffled = full;
        std::ranges::shuffle(shuffled, random);
        (void)device.ingest(shuffled);
        const std::string dump = device.dump();
        if (receiver == 0)
            expected = dump;
        REQUIRE(dump == expected);
    }
    return !expected.empty();
}

}  // namespace

TEST_CASE("random_schedules_on_two_three_and_four_devices_converge") {
    for (std::uint32_t seed = 1; seed <= 12; ++seed)
        for (const int devices : {2, 3, 4})
            run_random_schedule(devices, seed * 1000U + static_cast<std::uint32_t>(devices));
}

TEST_CASE("hostile_frames_cannot_break_convergence") {
    int with_state = 0;
    for (std::uint32_t seed = 1; seed <= 40; ++seed)
        with_state += run_hostile_schedule(seed) ? 1 : 0;
    CHECK(with_state >= 30);
}

TEST_CASE("unseen_writes_and_conflicting_edge_identities_converge_across_files") {
    const DeviceId d1{std::byte{1}};
    const DeviceId d2{std::byte{2}};
    Frame put;
    put.seq = 1;
    put.hlc = 1;
    put.ops = {NodePut{kNode}, NodePut{kOther}, EdgePut{kEdge, kNode, "A", kOther}};
    Frame rival;  // aynı kenar kimliği, başka uçlar, görmeden
    rival.seq = 1;
    rival.hlc = 2;
    rival.ops = {PropSet{kNode, "blind", std::int64_t{1}}, EdgePut{kEdge, kOther, "B", kNode},
                 PropSet{kEdge, "w", std::int64_t{2}}};
    const TempDir transit;
    const std::vector<std::string> frames_1{encode_frame(put)};
    const std::vector<std::string> frames_2{encode_frame(rival)};
    const fs::path log_1 = write_log(transit.path(), d1, frames_1);
    const fs::path log_2 = write_log(transit.path(), d2, frames_2);

    Device first;
    first.ingest({log_1});
    const MergeReport reported = first.ingest({log_2});
    Device second;
    second.ingest({log_2});
    second.ingest({log_1});
    Device together;
    together.ingest({log_1, log_2});
    CHECK(first.dump() == second.dump());
    CHECK(first.dump() == together.dump());
    CHECK(reported.stalled.empty());  // durmak cihazları ayrıştırırdı
    REQUIRE(reported.bad_frames.size() == 1);
    CHECK(reported.bad_frames[0].reason == "edge_identity_conflict");
    CHECK(reported.bad_frames[0].seq == 1);
}
