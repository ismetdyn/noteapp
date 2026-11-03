// L4b: parçalı çerçeve ve çok çerçeveli işlem (SPEC-graph "Parçalı çerçeve ve çok çerçeveli işlem"). Çerçeve
// sınırı yalnız dosyadaki kaydı sınırlar; işlem ve değer ona sığmak zorunda değildir. Testler küçük çerçeve
// sınırıyla (config limits.graph.*) hızlı kalır.
#include "graph/merge.hpp"
#include "graph/op.hpp"
#include "graph/oplog.hpp"
#include "graph/store.hpp"
#include "sync_test_support.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;
using noteapp::test::TempDir;
using test::begin_read;
using test::Device;
using test::node_named;
using test::write_file;

namespace fs = std::filesystem;

namespace {

const NodeId kN = node_named("n");
const NodeId kM = node_named("m");
const DeviceId kD1{std::byte{1}};

constexpr std::size_t kSmallFrameBytes = 64U * 1024U;
constexpr std::size_t kSmallFrameOps = 16;

GraphLimits small_limits() {
    GraphLimits limits;
    limits.frame_bytes = kSmallFrameBytes;
    limits.frame_ops = kSmallFrameOps;
    return limits;
}

// NUL, CR LF, BOM ve geçersiz UTF-8 baytları içeren belirlenimli içerik.
std::string pattern(std::size_t size) {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<char>((i * 131U + i / 7U) & 0xFFU);
    return bytes;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::uint32_t u32_at(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof value; ++i)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8U);
    return value;
}

int header_version(const fs::path& log) {
    return static_cast<unsigned char>(read_file(log).at(4));
}

// Kayıtların bittiği konumlar (zarf: u32 uzunluk + 8 baytlık özet).
std::vector<std::size_t> record_ends(const std::string& log) {
    std::vector<std::size_t> ends;
    for (std::size_t offset = kLogHeaderBytes; offset + kFrameEnvelopeBytes <= log.size();) {
        offset += kFrameEnvelopeBytes + u32_at(log, offset);
        ends.push_back(offset);
    }
    return ends;
}

template <class Unsigned> std::string le(Unsigned value) {
    std::string out;
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
        out.push_back(static_cast<char>(value >> (i * 8U)));
    return out;
}

// Elle kurulmuş v2 kaydı: tür, grup (seq), sıra, son işareti ve baytlar. İçerik kötü niyetli olabilir.
std::string part(std::uint64_t seq, std::uint32_t index, bool final, std::string_view chunk,
                 std::uint8_t kind = 1) {
    std::string payload(1, static_cast<char>(kind));
    payload += le(seq);
    payload += le(index);
    payload.push_back(final ? '\1' : '\0');
    payload += chunk;
    return encode_log_frame(payload);
}

std::string parts_header(const DeviceId& device, std::uint8_t version = 2) {
    std::string header = encode_log_header(device);
    header[4] = static_cast<char>(version);
    return header;
}

Frame frame_of(std::uint64_t seq, std::vector<Op> ops) {
    Frame frame;
    frame.seq = seq;
    frame.hlc = seq + 1;
    frame.ops = std::move(ops);
    return frame;
}

bool reports_reason(const MergeReport& report, std::string_view reason) {
    return std::ranges::any_of(report.bad_frames,
                               [&](const BadFrameReport& bad) { return bad.reason == reason; });
}

bool has(Graph& graph, const NodeId& id) {
    ReadTxn txn = begin_read(graph);
    const Result<bool> exists = txn.has_node(id);
    REQUIRE(exists.has_value());
    return *exists;
}

}  // namespace

TEST_CASE("a value larger than one frame commits, travels in parts and arrives byte-exact") {
    const std::string body = pattern(std::size_t{1} << 20U);
    Device writer(small_limits());
    writer.commit([&](WriteTxn& txn) {
        GRAPH_OK(txn.put_node(kN));
        GRAPH_OK(txn.set_property(kN, "body", Bytes{body}));
    });
    const fs::path log = writer.exported();
    CHECK(header_version(log) == 2);
    CHECK(record_ends(read_file(log)).size() > body.size() / kSmallFrameBytes);

    Device reader(small_limits());
    const MergeReport report = reader.ingest({log});
    CHECK(report.stalled.empty());
    CHECK(report.bad_frames.empty());
    CHECK(report.partial_tails.empty());
    {
        ReadTxn txn = begin_read(reader.graph());
        const Result<Value> value = txn.property(kN, "body");
        REQUIRE(value.has_value());
        CHECK(*value == Value{Bytes{body}});
    }
    CHECK(reader.dump() == writer.dump());
}

TEST_CASE("a transaction with more operations than frame_ops syncs atomically; half a group stays invisible") {
    constexpr int kNodes = 100;
    Device writer(small_limits());
    writer.commit([](WriteTxn& txn) {
        for (int i = 0; i < kNodes; ++i) {
            const NodeId id = node_named("t" + std::to_string(i));
            GRAPH_OK(txn.put_node(id));
            GRAPH_OK(txn.set_property(id, "i", std::int64_t{i}));
        }
    });
    CHECK(*writer.graph().last_seq() == 1);  // tek işlem, tek dot
    const fs::path log = writer.exported();
    const std::string full = read_file(log);
    const std::vector<std::size_t> ends = record_ends(full);
    REQUIRE(ends.size() >= 2);

    // Yazanın export'u parçalar arasında ölmüş gibi: kayıt sınırında ve kaydın ortasında kesik grup.
    Device reader(small_limits());
    for (const std::size_t cut : {ends.front(), full.size() - 5}) {
        write_file(log, full.substr(0, cut));
        const MergeReport partial = reader.ingest({log});
        CHECK(partial.partial_tails.size() == 1);
        CHECK(partial.stalled.empty());
        CHECK(partial.bad_frames.empty());
        CHECK_FALSE(has(reader.graph(), node_named("t0")));
        CHECK_FALSE(has(reader.graph(), node_named("t99")));
    }

    // Yazanın kesik dosyası ops'tan yeniden üretilir, kenara alınmaz.
    const Result<ExportReport> repaired = writer.graph().export_log(writer.outbox());
    REQUIRE(repaired.has_value());
    CHECK_FALSE(repaired->set_aside.has_value());
    CHECK(read_file(log) == full);

    const MergeReport report = reader.ingest({log});
    CHECK(report.partial_tails.empty());
    CHECK(report.stalled.empty());
    CHECK(has(reader.graph(), node_named("t0")));
    CHECK(has(reader.graph(), node_named("t99")));
    CHECK(reader.dump() == writer.dump());
}

TEST_CASE("a log keeps the old format until a frame needs parts; earlier frames still match after that") {
    Device writer(small_limits());
    Device reader(small_limits());
    writer.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kN)); });
    const fs::path log = writer.exported();
    CHECK(header_version(log) == 1);
    CHECK(reader.ingest({log}).stalled.empty());

    writer.commit([](WriteTxn& txn) { GRAPH_OK(txn.set_property(kN, "body", Bytes{pattern(256U * 1024U)})); });
    const Result<ExportReport> upgraded = writer.graph().export_log(writer.outbox());
    REQUIRE(upgraded.has_value());
    CHECK_FALSE(upgraded->set_aside.has_value());
    CHECK(header_version(log) == 2);

    const MergeReport report = reader.ingest({log});
    CHECK(report.bad_frames.empty());
    CHECK(report.stalled.empty());
    CHECK(reader.dump() == writer.dump());
}

TEST_CASE("a newer log version or record kind stops that log as log_too_new and nothing is misread") {
    SUBCASE("newer header version") {
        Device writer;
        writer.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kN)); });
        const fs::path log = writer.exported();
        std::string bytes = read_file(log);
        bytes[4] = static_cast<char>(kLogVersion + 1);
        write_file(log, bytes);
        Device reader;
        const MergeReport report = reader.ingest({log});
        CHECK(reports_reason(report, "log_too_new"));
        CHECK_FALSE(has(reader.graph(), kN));
    }
    SUBCASE("unknown record kind after a valid part") {
        const TempDir dir;
        const fs::path log = dir.path() / log_file_name(kD1);
        write_file(log, parts_header(kD1) + part(1, 0, true, encode_frame(frame_of(1, {NodePut{kN}}))) +
                            part(2, 0, true, encode_frame(frame_of(2, {NodePut{kM}})), 7));
        Device reader;
        const MergeReport report = reader.ingest({log});
        CHECK(has(reader.graph(), kN));
        CHECK_FALSE(has(reader.graph(), kM));
        REQUIRE(report.stalled.size() == 1);
        CHECK(report.stalled[0].reason == "log_too_new");
    }
}

TEST_CASE("a hostile endless group is bounded: unfinished is invisible, oversized stops only its log") {
    Device honest;
    honest.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kM)); });
    const fs::path honest_log = honest.exported();
    const TempDir dir;
    const fs::path log = dir.path() / log_file_name(kD1);
    const std::string frame = encode_frame(frame_of(1, {NodePut{kN}}));
    GraphLimits limits;
    limits.group_bytes = 4096;
    Device reader(limits);

    SUBCASE("an unfinished group is a partial tail and holds nothing back") {
        std::string bytes = parts_header(kD1);
        for (std::uint32_t i = 0; i < 50; ++i)
            bytes += part(1, i, false, std::string(64, 'x'));
        write_file(log, bytes);
        const MergeReport report = reader.ingest({log, honest_log});
        CHECK(report.partial_tails.size() == 1);
        CHECK(report.stalled.empty());
        CHECK(has(reader.graph(), kM));
    }
    SUBCASE("a group over group_bytes stops only that log") {
        std::string bytes = parts_header(kD1);
        for (std::uint32_t i = 0; i < 100; ++i)
            bytes += part(1, i, false, std::string(64, 'x'));
        bytes += part(1, 100, true, "y");
        write_file(log, bytes);
        const MergeReport report = reader.ingest({log, honest_log});
        CHECK(reports_reason(report, "group_too_large"));
        REQUIRE(report.stalled.size() == 1);
        CHECK(report.stalled[0].reason == "bad_frame");
        CHECK(has(reader.graph(), kM));
    }
    SUBCASE("a gap in the index or a changed seq inside a group is broken_group") {
        for (const bool gap : {true, false}) {
            CAPTURE(gap);
            write_file(log, parts_header(kD1) + part(1, 0, false, frame.substr(0, 10)) +
                                part(gap ? 1 : 2, gap ? 2 : 1, true, frame.substr(10)));
            const MergeReport report = reader.ingest({log, honest_log});
            CHECK(reports_reason(report, "broken_group"));
            CHECK_FALSE(has(reader.graph(), kN));
            CHECK(has(reader.graph(), kM));
        }
    }
}

// Kayıt ya da grup SQLite'ın tek değer sınırını (SQLITE_LIMIT_LENGTH) aşarsa ops'a yazılamaz: frame_bytes
// sınırsız olsa da o günlük bad_frame ile durur, ingest hata vermez, diğer günlükler sürer.
TEST_CASE("a remote frame longer than SQLite's value limit stops only its log, also with frame_bytes unlimited") {
    Device big;
    big.commit([](WriteTxn& txn) {
        GRAPH_OK(txn.put_node(kN));
        GRAPH_OK(txn.set_property(kN, "body", Bytes{std::string(8192, 'x')}));
    });
    Device honest;
    honest.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kM)); });
    const std::vector<fs::path> logs{big.exported(), honest.exported()};

    const TempDir dir;
    GraphLimits unlimited;
    unlimited.frame_bytes = SIZE_MAX;
    Result<std::unique_ptr<Store>> store = Store::open(dir.path() / "g.db", unlimited);
    REQUIRE(store.has_value());
    sqlite3_limit((*store)->connection_for_test(), SQLITE_LIMIT_LENGTH, 4096);
    IngestLimits limits;
    limits.frame = FrameLimits{SIZE_MAX, SIZE_MAX, kDefaultDeps};
    const Result<MergeReport> report = ingest_logs(**store, logs, limits);
    REQUIRE_MESSAGE(report.has_value(), (report ? "" : report.error().message_key));
    REQUIRE(report->stalled.size() == 1);
    CHECK(report->stalled[0].device == big.graph().device());
    CHECK(report->stalled[0].reason == "bad_frame");
    CHECK(*(*store)->applied_seq(honest.graph().device()) == 1);
}

TEST_CASE("a local value or transaction beyond SQLite's value limit is LimitExceeded, never a silent NULL") {
    const TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "g.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    sqlite3_limit(store.connection_for_test(), SQLITE_LIMIT_LENGTH, 4096);
    GRAPH_OK(store.begin_write());
    GRAPH_OK(store.record(NodePut{kN}));
    SUBCASE("one value") {
        const Result<void> set = store.record(PropSet{kN, "body", Bytes{std::string(8192, 'x')}});
        REQUIRE_FALSE(set.has_value());
        CHECK(set.error().code == Error::Code::LimitExceeded);
    }
    SUBCASE("the transaction") {
        for (int i = 0; i < 3; ++i)
            GRAPH_OK(store.record(PropSet{kN, "k" + std::to_string(i), Bytes{std::string(2000, 'x')}}));
        const Result<void> committed = store.commit_write();
        REQUIRE_FALSE(committed.has_value());
        CHECK(committed.error().code == Error::Code::LimitExceeded);
    }
    store.end_txn();
}

// Toplu yazanların işlem başına bayt payı bundan türer (SPEC-graph "Toplu yazanlar").
TEST_CASE("the effective frame maximum is capped at SQLite's value limit when frame_bytes is unlimited") {
    const TempDir dir;
    GraphLimits unlimited;
    unlimited.frame_bytes = SIZE_MAX;
    Graph graph = test::open_graph(dir.path() / "g.db", unlimited);
    CHECK(graph.max_frame_bytes() <= static_cast<std::size_t>(INT_MAX));
    Graph small = test::open_graph(dir.path() / "s.db", small_limits());
    CHECK(small.max_frame_bytes() == kSmallFrameBytes);
}

namespace {

// Mantıksal çerçeveyi `parts` eşit v2 kaydına böler (düşmanca gönderen istediği kadar küçük parça seçebilir).
std::string split_into_parts(const DeviceId& device, std::uint64_t seq, const std::string& payload,
                             std::size_t parts) {
    std::string bytes = parts_header(device);
    std::size_t offset = 0;
    for (std::size_t index = 0; index < parts; ++index) {
        const std::size_t size = payload.size() / parts + (index < payload.size() % parts ? 1 : 0);
        bytes += part(seq, static_cast<std::uint32_t>(index), index + 1 == parts, payload.substr(offset, size));
        offset += size;
    }
    return bytes;
}

std::vector<Op> many_nodes(std::size_t count) {
    std::vector<Op> ops;
    for (std::size_t i = 0; i < count; ++i)
        ops.emplace_back(NodePut{node_named("o" + std::to_string(i))});
    return ops;
}

}  // namespace

// L4b2 bulgu 1: işlem sınırı parça x frame_ops'tu, parça sayısını gönderen seçer; 1 GB'lık 17 baytlık işlemler
// ~6 GB çözülürdü. Sınır en çok group_ops ve merge_owners'tır; aşan grup çözülmeden bekler, diğerleri sürer.
TEST_CASE("a group with more operations than group_ops or merge_owners stalls before decoding") {
    Device honest;
    honest.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kM)); });
    const fs::path honest_log = honest.exported();
    const TempDir dir;
    const fs::path log = dir.path() / log_file_name(kD1);
    write_file(log, split_into_parts(kD1, 1, encode_frame(frame_of(1, many_nodes(1000))), 200));
    GraphLimits limits;
    limits.frame_ops = 16;  // 200 parça x 16 = 3200 >= 1000: eski sınır geçirirdi
    SUBCASE("merge_owners") {
        limits.merge_owners = 100;
    }
    SUBCASE("group_ops") {
        limits.group_ops = 500;
    }
    Device reader(limits);
    const MergeReport report = reader.ingest({log, honest_log});
    CHECK(reports_reason(report, "too_many_ops"));
    REQUIRE(report.stalled.size() == 1);
    CHECK(report.stalled[0].device == kD1);
    CHECK(report.stalled[0].reason == "bad_frame");
    CHECK_FALSE(has(reader.graph(), node_named("o0")));
    CHECK(has(reader.graph(), kM));
}

// L4b2 bulgu 2: okuyucuya sığan ama ops satırına sığmayan uzak çerçeve (SQLITE_LIMIT_LENGTH satırı da sınırlar)
// ingest'i hata ile düşürmez; o günlük bad_frame ile durur, diğerleri sürer.
TEST_CASE("a remote frame that fits the reader but not the ops row stalls only its log") {
    constexpr std::size_t kLength = 4096;
    const auto frame_with = [](std::size_t size) {
        return encode_frame(frame_of(1, {NodePut{kN}, PropSet{kN, "body", Bytes{std::string(size, 'x')}}}));
    };
    const std::size_t base = frame_with(0).size();
    const std::string payload = frame_with(kLength - 10 - base);
    REQUIRE(payload.size() == kLength - 10);
    const TempDir dir;
    const fs::path log = dir.path() / log_file_name(kD1);
    write_file(log, encode_log_header(kD1) + encode_log_frame(payload));
    Device honest;
    honest.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kM)); });
    const std::vector<fs::path> logs{log, honest.exported()};

    GraphLimits unlimited;
    unlimited.frame_bytes = SIZE_MAX;
    Result<std::unique_ptr<Store>> store = Store::open(dir.path() / "g.db", unlimited);
    REQUIRE(store.has_value());
    sqlite3_limit((*store)->connection_for_test(), SQLITE_LIMIT_LENGTH, static_cast<int>(kLength));
    IngestLimits limits;
    limits.frame = FrameLimits{SIZE_MAX, SIZE_MAX, kDefaultDeps};
    const Result<MergeReport> report = ingest_logs(**store, logs, limits);
    REQUIRE_MESSAGE(report.has_value(), (report ? "" : report.error().message_key));
    REQUIRE(report->stalled.size() == 1);
    CHECK(report->stalled[0].device == kD1);
    CHECK(report->stalled[0].reason == "bad_frame");
    CHECK(*(*store)->applied_seq(kD1) == 0);
    CHECK(*(*store)->applied_seq(honest.graph().device()) == 1);
}

// L4b2 bulgu 3: bitmemiş grup her senkronda yeniden okunup özetleniyordu, ingest bütçesine sayılmadan. Okunan
// yarım grup bütçeden düşer; dosya değişmediyse sonraki ingest onu yeniden okumaz.
TEST_CASE("an unfinished group is charged to the ingest budget and not re-read while its log is unchanged") {
    const TempDir dir;
    const fs::path log = dir.path() / log_file_name(kD1);
    std::string bytes = parts_header(kD1);
    for (std::uint32_t i = 0; i < 50; ++i)
        bytes += part(1, i, false, std::string(64, 'x'));
    write_file(log, bytes);
    Device honest;  // cihaz kimliği UUIDv7'dir: kD1'den sonra sıralanır
    honest.commit([](WriteTxn& txn) { GRAPH_OK(txn.put_node(kM)); });
    const std::vector<fs::path> logs{log, honest.exported()};

    Result<std::unique_ptr<Store>> store = Store::open(dir.path() / "g.db");
    REQUIRE(store.has_value());
    IngestLimits limits;
    limits.max_bytes = 1000;
    const Result<MergeReport> first = ingest_logs(**store, logs, limits);
    REQUIRE(first.has_value());
    CHECK(first->partial_tails.size() == 1);
    REQUIRE(first->stalled.size() == 1);
    CHECK(first->stalled[0].reason == "ingest_budget");  // yarım grup bütçeyi yedi
    CHECK(*(*store)->applied_seq(honest.graph().device()) == 0);

    const Result<MergeReport> second = ingest_logs(**store, logs, limits);
    REQUIRE(second.has_value());
    CHECK(second->partial_tails.size() == 1);
    CHECK(second->stalled.empty());  // değişmeyen yarım grup yeniden okunmadı
    CHECK(*(*store)->applied_seq(honest.graph().device()) == 1);

    // Dosya değişince (grup tamamlanınca) yeniden okunur ve uygulanır.
    write_file(log, parts_header(kD1) + part(1, 0, true, encode_frame(frame_of(1, {NodePut{kN}}))));
    const Result<MergeReport> third = ingest_logs(**store, logs, IngestLimits{});
    REQUIRE(third.has_value());
    CHECK(third->partial_tails.empty());
    CHECK(*(*store)->applied_seq(kD1) == 1);
}

// L4d bulgu 3: hiç uygulanmayan (TOOBIG ile geri alınan) çerçeve yeniden kurma bırakmaz; yoksa her ingest
// boşuna O(ops) kurma yapardı.
TEST_CASE("a remote frame rolled back as too big leaves no pending rebuild behind") {
    constexpr std::size_t kLength = 4096;
    const DeviceId kD2{std::byte{2}};
    const std::string first = encode_frame(frame_of(1, {NodePut{kN}}));
    // D2, D1'in yazmasını görmeden siler: yerinde uygulama kanıtlanamaz, normalde yeniden kurma ister.
    const auto second_with = [](std::size_t size) {
        return encode_frame(frame_of(1, {NodeDel{kN}, PropSet{kM, "body", Bytes{std::string(size, 'x')}}}));
    };
    const std::string second = second_with(kLength - 10 - second_with(0).size());
    const TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "g.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    sqlite3_limit(store.connection_for_test(), SQLITE_LIMIT_LENGTH, static_cast<int>(kLength));
    GRAPH_OK(store.begin_write());
    Result<Merger> merger = Merger::open(store);
    REQUIRE(merger.has_value());
    GRAPH_OK(merger->apply_frame(kD1, *decode_frame(test::as_bytes(first)), test::as_bytes(first)));
    const Result<void> big = merger->apply_frame(kD2, *decode_frame(test::as_bytes(second)), test::as_bytes(second));
    REQUIRE_FALSE(big.has_value());
    CHECK(big.error().code == Error::Code::LimitExceeded);
    CHECK_FALSE(merger->rebuild_needed());
    store.end_txn();
}

// L4d bulgu 3: bitmemiş grubun kaydı, günlük artık yoksa ya da grup bittiyse silinir; meta büyümez.
TEST_CASE("a remembered unfinished group is forgotten once its log is gone") {
    const TempDir dir;
    const fs::path log = dir.path() / log_file_name(kD1);
    std::string bytes = parts_header(kD1);
    for (std::uint32_t i = 0; i < 3; ++i)
        bytes += part(1, i, false, "x");
    write_file(log, bytes);
    Result<std::unique_ptr<Store>> store = Store::open(dir.path() / "g.db");
    REQUIRE(store.has_value());
    const auto remembered = [&] {
        test::RawDb raw(dir.path() / "g.db");
        return raw.scalar("SELECT count(*) FROM meta WHERE k LIKE 'partial_tail:%'");
    };
    const std::vector<fs::path> logs{log};
    REQUIRE(ingest_logs(**store, logs, IngestLimits{}).has_value());
    CHECK(remembered() == 1);
    REQUIRE(ingest_logs(**store, std::span<const fs::path>{}, IngestLimits{}).has_value());
    CHECK(remembered() == 0);
}
