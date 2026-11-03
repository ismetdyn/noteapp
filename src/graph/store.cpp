#include "graph/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace noteapp::graph::detail {

namespace {

// SQL metni yalnız derleme anında sabit olabilir: consteval, çalışma anında üretilmiş metnin bu türe
// girmesini engeller.
class Sql {
public:
    consteval Sql(const char* sql) : text(sql) {}
    const char* text;
};

struct StatementEntry {
    StatementId id{};
    Sql sql;
};

// Her ifade tek tabloda nokta veya aralık okur. INDEXED BY plan kararı değil gereksinimdir: dizin
// kullanılamıyorsa hazırlama düşer (lang_indexedby.html), sqlite_stat1 planı değiştiremez.
constexpr StatementEntry kStatements[] = {
    {StatementId::MetaGet, "SELECT v FROM meta WHERE k=?1"},
    {StatementId::MetaPut, "INSERT OR REPLACE INTO meta(k, v) VALUES (?1, ?2)"},
    {StatementId::MetaKeysInRange, "SELECT k FROM meta WHERE k>=?1 AND k<?2 ORDER BY k"},
    {StatementId::MetaDelete, "DELETE FROM meta WHERE k=?1"},
    {StatementId::NodeGet, "SELECT dots FROM nodes WHERE id=?1"},
    {StatementId::NodePut, "INSERT OR REPLACE INTO nodes(id, dots) VALUES (?1, ?2)"},
    {StatementId::NodeDelete, "DELETE FROM nodes WHERE id=?1"},
    {StatementId::NodesAll, "SELECT id FROM nodes ORDER BY id"},
    {StatementId::LabelGet, "SELECT dots FROM labels WHERE node=?1 AND label=?2"},
    {StatementId::LabelPut, "INSERT OR REPLACE INTO labels(node, label, dots) VALUES (?1, ?2, ?3)"},
    {StatementId::LabelDelete, "DELETE FROM labels WHERE node=?1 AND label=?2"},
    {StatementId::LabelsOfNode, "SELECT label FROM labels WHERE node=?1 ORDER BY label"},
    {StatementId::LabelsDeleteOfNode, "DELETE FROM labels WHERE node=?1"},
    {StatementId::LabelScan, "SELECT node FROM labels INDEXED BY labels_scan WHERE label=?1 ORDER BY node"},
    {StatementId::EdgeGet, "SELECT src, type, dst, dots FROM edges WHERE id=?1"},
    {StatementId::EdgePut,
     "INSERT OR REPLACE INTO edges(id, src, type, dst, dots) VALUES (?1, ?2, ?3, ?4, ?5)"},
    {StatementId::EdgeDelete, "DELETE FROM edges WHERE id=?1"},
    {StatementId::EdgesOut,
     "SELECT id, type, dst FROM edges INDEXED BY edges_out WHERE src=?1 ORDER BY type, dst, id"},
    {StatementId::EdgesOutOfType,
     "SELECT id, type, dst FROM edges INDEXED BY edges_out WHERE src=?1 AND type=?2 ORDER BY dst, id"},
    {StatementId::EdgesIn,
     "SELECT id, type, src FROM edges INDEXED BY edges_in WHERE dst=?1 ORDER BY type, src, id"},
    {StatementId::EdgesInOfType,
     "SELECT id, type, src FROM edges INDEXED BY edges_in WHERE dst=?1 AND type=?2 ORDER BY src, id"},
    {StatementId::PropVersions,
     "SELECT dev, seq, hlc, value FROM props WHERE owner=?1 AND key=?2 ORDER BY dev, seq"},
    {StatementId::PropPut,
     "INSERT OR REPLACE INTO props(owner, key, dev, seq, hlc, value) VALUES (?1, ?2, ?3, ?4, ?5, ?6)"},
    {StatementId::PropDeleteVersion, "DELETE FROM props WHERE owner=?1 AND key=?2 AND dev=?3 AND seq=?4"},
    {StatementId::PropValuesOfOwner, "SELECT value FROM props WHERE owner=?1"},
    {StatementId::PropsOfOwner,
     "SELECT key, dev, seq, hlc, value FROM props WHERE owner=?1 ORDER BY key, dev, seq"},
    {StatementId::PropsDeleteOfOwner, "DELETE FROM props WHERE owner=?1"},
    {StatementId::LargeGet, "SELECT data FROM large_values WHERE id=?1"},
    {StatementId::LargePut, "INSERT INTO large_values(data) VALUES (?1)"},
    {StatementId::LargeDelete, "DELETE FROM large_values WHERE id=?1"},
    {StatementId::OpPut, "INSERT INTO ops(dev, seq, frame) VALUES (?1, ?2, ?3)"},
    {StatementId::PeersOfDevice, "SELECT other, seq FROM peers WHERE dev=?1 ORDER BY other"},
    {StatementId::OpById, "SELECT dev, seq, frame FROM ops WHERE id=?1"},
    {StatementId::OpLastId, "SELECT max(id) FROM ops"},
    {StatementId::OpGet, "SELECT frame FROM ops INDEXED BY sqlite_autoindex_ops_1 WHERE dev=?1 AND seq=?2"},
    {StatementId::OpsOfDevice,
     "SELECT seq, frame FROM ops INDEXED BY sqlite_autoindex_ops_1 WHERE dev=?1 AND seq>?2 ORDER BY seq"},
    {StatementId::PeerGet, "SELECT seq FROM peers WHERE dev=?1 AND other=?2"},
    {StatementId::PeerPut, "INSERT OR REPLACE INTO peers(dev, other, seq) VALUES (?1, ?2, ?3)"},
    {StatementId::PeerCount, "SELECT count(*) FROM peers WHERE dev=?1"},
};

consteval bool statements_match_ids() {
    std::size_t index = 0;
    for (const StatementEntry& entry : kStatements) {
        if (static_cast<std::size_t>(entry.id) != index)
            return false;
        ++index;
    }
    return index == static_cast<std::size_t>(StatementId::Count);
}
static_assert(statements_match_ids(), "kStatements must list every StatementId in order");

// Şema sürümü 1. Tablo kümesi kapalıdır: etiket, tip veya anahtar eklemek burada değişiklik gerektirmez (ilke
// 4).
constexpr Sql kSchemaV1[] = {
    "CREATE TABLE meta(k TEXT PRIMARY KEY, v BLOB) WITHOUT ROWID",
    "CREATE TABLE nodes(id BLOB PRIMARY KEY, dots BLOB NOT NULL) WITHOUT ROWID",
    "CREATE TABLE labels(node BLOB, label TEXT, dots BLOB NOT NULL, PRIMARY KEY(node, label)) WITHOUT ROWID",
    "CREATE INDEX labels_scan ON labels(label, node)",
    // NOLINTNEXTLINE(bugprone-suspicious-missing-comma) -- tek SQL deyimi iki satıra bölünmüştür
    "CREATE TABLE edges(id BLOB PRIMARY KEY, src BLOB NOT NULL, type TEXT NOT NULL, dst BLOB NOT NULL,"
    " dots BLOB NOT NULL) WITHOUT ROWID",
    "CREATE INDEX edges_out ON edges(src, type, dst)",
    "CREATE INDEX edges_in ON edges(dst, type, src)",
    "CREATE TABLE props(owner BLOB, key TEXT, dev BLOB, seq INT, hlc INT, value BLOB NOT NULL,"
    " PRIMARY KEY(owner, key, dev, seq)) WITHOUT ROWID",
    "CREATE TABLE large_values(id INTEGER PRIMARY KEY, data BLOB NOT NULL)",
    "CREATE TABLE ops(id INTEGER PRIMARY KEY, dev BLOB NOT NULL, seq INT NOT NULL, frame BLOB NOT NULL, "
    "UNIQUE(dev, seq))",
    "CREATE TABLE peers(dev BLOB, other BLOB, seq INT, PRIMARY KEY(dev, other)) WITHOUT ROWID",
    "PRAGMA application_id=1313293378",  // "NGDB"; kApplicationId ile aynı
    "PRAGMA user_version=1",
};

constexpr std::int64_t kApplicationId = 0x4E474442;
constexpr std::int64_t kSchemaVersion = 1;
constexpr int kBusyTimeoutMs = 5000;
constexpr std::chrono::milliseconds kOpenRetryPause{10};
// withoutrowid.html: WITHOUT ROWID satırı 4 KiB sayfada ~200 baytı aşmamalı; büyük değer de satır okunurken
// yüklenmesin.
constexpr std::size_t kInlineValueMax = 200;
constexpr std::uint8_t kLargeReferenceBit = 0x80;
constexpr std::size_t kLargeReferenceBytes = 1 + sizeof(std::uint64_t) * 2;

constexpr std::string_view kMetaDevice = "device_id";
constexpr std::string_view kMetaSeq = "seq";
constexpr std::string_view kMetaHlc = "hlc";
constexpr std::string_view kMetaRebuildPending = "rebuild_pending";
constexpr unsigned kHlcCounterBits = 16;
constexpr std::uint64_t kLowestHlc = 1;  // 0 "head yok" karşılaştırmasıyla karışmasın

Error corrupt(std::string reason) {
    return Error{Error::Code::Corrupt, "graph.corrupt", {{"reason", std::move(reason)}}};
}

// Tek hazırlama noktası: yalnız `Sql` kabul eder (tests/graph/source_scan_test.cpp bunu tarar).
int prepare_sql(sqlite3* db, Sql sql, unsigned flags, sqlite3_stmt** out) {
    return sqlite3_prepare_v3(db, sql.text, -1, flags, out, nullptr);
}

using OwnedStatement = std::unique_ptr<sqlite3_stmt, decltype([](sqlite3_stmt* s) { sqlite3_finalize(s); })>;

Result<OwnedStatement> prepare_once(sqlite3* db, Sql sql) {
    sqlite3_stmt* raw = nullptr;
    const int code = prepare_sql(db, sql, 0, &raw);
    if (code != SQLITE_OK)
        return std::unexpected(storage_error(code, "prepare"));
    return OwnedStatement(raw);
}

Result<void> exec(sqlite3* db, Sql sql) {
    Result<OwnedStatement> statement = prepare_once(db, sql);
    if (!statement)
        return std::unexpected(statement.error());
    int code = SQLITE_ROW;
    while (code == SQLITE_ROW)
        code = sqlite3_step(statement->get());
    if (code != SQLITE_DONE)
        return std::unexpected(storage_error(code, "exec"));
    return {};
}

Result<std::int64_t> query_int(sqlite3* db, Sql sql) {
    Result<OwnedStatement> statement = prepare_once(db, sql);
    if (!statement)
        return std::unexpected(statement.error());
    const int code = sqlite3_step(statement->get());
    if (code != SQLITE_ROW)
        return std::unexpected(storage_error(code, "query"));
    return sqlite3_column_int64(statement->get(), 0);
}

Result<std::string> query_text(sqlite3* db, Sql sql) {
    Result<OwnedStatement> statement = prepare_once(db, sql);
    if (!statement)
        return std::unexpected(statement.error());
    const int code = sqlite3_step(statement->get());
    if (code != SQLITE_ROW)
        return std::unexpected(storage_error(code, "query"));
    const unsigned char* text = sqlite3_column_text(statement->get(), 0);
    return text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(text));
}

template <class Bind> Result<void> run_statement(Store& store, StatementId id, Bind&& bind) {
    Result<Statement> statement = store.statement(id);
    if (!statement)
        return std::unexpected(statement.error());
    bind(*statement);
    return statement->run();
}

using IdBytes = std::span<const std::byte, 16>;

// Dots kuralı (nodes, labels, edges): bağlamın gördükleri düşer, kendisi eklenir. Kalan her dot görülmemiş
// eşzamanlı yazmadır.
bool is_seen(const Dot& dot, const ApplyContext& context) {
    return dot == context.dot || context.seen.covers(dot);
}

std::vector<Dot> surviving(std::vector<Dot> dots, const ApplyContext& context) {
    std::erase_if(dots, [&](const Dot& dot) { return is_seen(dot, context); });
    return dots;
}

std::string advanced(std::vector<Dot> dots, const ApplyContext& context) {
    dots = surviving(std::move(dots), context);
    dots.push_back(context.dot);
    std::ranges::sort(dots);
    return encode_dots(dots);
}

std::span<const std::byte> blob_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::span<const std::byte> blob_of(const DeviceId& device) {
    return device;
}

// Satır yoksa nullopt: düğüm, etiket ya da kenar ölü.
template <class Bind>
Result<std::optional<std::vector<Dot>>> read_dots(Store& store, StatementId id, int column, Bind&& bind) {
    Result<Statement> statement = store.statement(id);
    if (!statement)
        return std::unexpected(statement.error());
    bind(*statement);
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return std::optional<std::vector<Dot>>{};
    Result<std::vector<Dot>> dots = decode_dots(statement->column_blob(column));
    if (!dots)
        return std::unexpected(dots.error());
    return std::optional<std::vector<Dot>>{std::move(*dots)};
}

Result<std::optional<std::vector<Dot>>> read_node_dots(Store& store, const NodeId& id) {
    return read_dots(store, StatementId::NodeGet, 0, [&](Statement& s) { s.bind_blob(1, id.bytes); });
}

Result<std::optional<std::vector<Dot>>> read_edge_dots(Store& store, const EdgeId& id) {
    return read_dots(store, StatementId::EdgeGet, 3, [&](Statement& s) { s.bind_blob(1, id.bytes); });
}

Result<std::optional<std::vector<Dot>>> read_label_dots(Store& store, const NodeId& node,
                                                        std::string_view label) {
    return read_dots(store, StatementId::LabelGet, 0,
                     [&](Statement& s) { s.bind_blob(1, node.bytes).bind_text(2, label); });
}

Result<void> put_node_dots(Store& store, const NodeId& id, const std::string& dots) {
    return run_statement(store, StatementId::NodePut,
                         [&](Statement& s) { s.bind_blob(1, id.bytes).bind_blob(2, blob_of(dots)); });
}

Result<void> put_label_dots(Store& store, const NodeId& node, std::string_view label,
                            const std::string& dots) {
    return run_statement(store, StatementId::LabelPut, [&](Statement& s) {
        s.bind_blob(1, node.bytes).bind_text(2, label).bind_blob(3, blob_of(dots));
    });
}

// Düğüme yazma: dots ilerler, böylece onu görmemiş silme etkisiz kalır. Satır yoksa kurulur: yazma düğümü
// var eder. Sıra bağımsızlığı bunu ister: görmediği düğüme yazan uzak çerçeve, put'tan önce de sonra da aynı
// sonucu verir. Yerel API olmayan düğüme yazmayı zaten reddeder.
Result<void> touch_node(Store& store, const NodeId& id, const ApplyContext& context) {
    Result<std::optional<std::vector<Dot>>> dots = read_node_dots(store, id);
    if (!dots)
        return std::unexpected(dots.error());
    return put_node_dots(store, id, advanced(dots->value_or(std::vector<Dot>{}), context));
}

std::optional<std::int64_t> large_reference_id(std::span<const std::byte> stored) {
    if (stored.size() != kLargeReferenceBytes ||
        (std::to_integer<std::uint8_t>(stored[0]) & kLargeReferenceBit) == 0)
        return std::nullopt;
    std::uint64_t id = 0;
    for (std::size_t i = 0; i < sizeof id; ++i)
        id |= std::to_integer<std::uint64_t>(stored[1 + i]) << (i * 8U);
    return static_cast<std::int64_t>(id);
}

bool is_large_reference(std::span<const std::byte> stored) {
    return !stored.empty() && (std::to_integer<std::uint8_t>(stored[0]) & kLargeReferenceBit) != 0;
}

Result<void> delete_large(Store& store, std::int64_t id) {
    return run_statement(store, StatementId::LargeDelete, [&](Statement& s) { s.bind_int(1, id); });
}

Result<void> delete_owner_props(Store& store, std::span<const std::byte> owner) {
    std::vector<std::int64_t> large_ids;
    {
        Result<Statement> values = store.statement(StatementId::PropValuesOfOwner);
        if (!values)
            return std::unexpected(values.error());
        values->bind_blob(1, owner);
        while (true) {
            const Result<bool> row = values->step();
            if (!row)
                return std::unexpected(row.error());
            if (!*row)
                break;
            if (const auto id = large_reference_id(values->column_blob(0)))
                large_ids.push_back(*id);
        }
    }
    for (const std::int64_t id : large_ids)
        if (const Result<void> deleted = delete_large(store, id); !deleted)
            return deleted;
    return run_statement(store, StatementId::PropsDeleteOfOwner,
                         [&](Statement& s) { s.bind_blob(1, owner); });
}

Result<void> delete_edge_state(Store& store, const EdgeId& id) {
    if (const Result<void> props = delete_owner_props(store, id.bytes); !props)
        return props;
    return run_statement(store, StatementId::EdgeDelete, [&](Statement& s) { s.bind_blob(1, id.bytes); });
}

Result<void> collect_edge_ids(Store& store, StatementId id, const NodeId& node, std::vector<EdgeId>& out) {
    Result<Statement> statement = store.statement(id);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_blob(1, node.bytes);
    while (true) {
        const Result<bool> row = statement->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return {};
        const std::span<const std::byte> blob = statement->column_blob(0);
        if (blob.size() != sizeof(EdgeId::bytes))
            return std::unexpected(corrupt("bad_edge_id"));
        EdgeId edge;
        std::ranges::copy(blob, edge.bytes.begin());
        out.push_back(edge);
    }
}

Result<void> delete_node_state(Store& store, const NodeId& id) {
    std::vector<EdgeId> edges;
    if (const Result<void> out = collect_edge_ids(store, StatementId::EdgesOut, id, edges); !out)
        return out;
    if (const Result<void> in = collect_edge_ids(store, StatementId::EdgesIn, id, edges); !in)
        return in;
    std::ranges::sort(edges);
    const auto duplicates = std::ranges::unique(edges);  // öz döngü iki yönde de görünür
    edges.erase(duplicates.begin(), duplicates.end());
    for (const EdgeId& edge : edges)
        if (const Result<void> deleted = delete_edge_state(store, edge); !deleted)
            return deleted;
    if (const Result<void> labels = run_statement(store, StatementId::LabelsDeleteOfNode,
                                                  [&](Statement& s) { s.bind_blob(1, id.bytes); });
        !labels)
        return labels;
    if (const Result<void> props = delete_owner_props(store, id.bytes); !props)
        return props;
    return run_statement(store, StatementId::NodeDelete, [&](Statement& s) { s.bind_blob(1, id.bytes); });
}

Result<void> apply_one(Store& store, const ApplyContext& context, const NodePut& op) {
    Result<std::optional<std::vector<Dot>>> dots = read_node_dots(store, op.id);
    if (!dots)
        return std::unexpected(dots.error());
    return put_node_dots(store, op.id, advanced(dots->value_or(std::vector<Dot>{}), context));
}

// Düğümün tüm yazmalarını görmemiş silme boştur: düzenleme kazanır (SPEC-graph "Silme ve düzenleme
// çakışması").
Result<void> apply_one(Store& store, const ApplyContext& context, const NodeDel& op) {
    Result<std::optional<std::vector<Dot>>> dots = read_node_dots(store, op.id);
    if (!dots)
        return std::unexpected(dots.error());
    if (!dots->has_value() || !surviving(std::move(**dots), context).empty())
        return {};
    return delete_node_state(store, op.id);
}

Result<void> apply_one(Store& store, const ApplyContext& context, const LabelAdd& op) {
    if (const Result<void> touched = touch_node(store, op.node, context); !touched)
        return touched;
    Result<std::optional<std::vector<Dot>>> dots = read_label_dots(store, op.node, op.label);
    if (!dots)
        return std::unexpected(dots.error());
    return put_label_dots(store, op.node, op.label, advanced(dots->value_or(std::vector<Dot>{}), context));
}

// Add-wins: yalnız görülen eklemeler düşer; görülmeyen dot kalırsa etiket yaşar.
Result<void> apply_one(Store& store, const ApplyContext& context, const LabelDel& op) {
    if (const Result<void> touched = touch_node(store, op.node, context); !touched)
        return touched;
    Result<std::optional<std::vector<Dot>>> dots = read_label_dots(store, op.node, op.label);
    if (!dots)
        return std::unexpected(dots.error());
    if (!dots->has_value())
        return {};
    const std::vector<Dot> left = surviving(std::move(**dots), context);
    if (!left.empty())
        return put_label_dots(store, op.node, op.label, encode_dots(left));
    return run_statement(store, StatementId::LabelDelete,
                         [&](Statement& s) { s.bind_blob(1, op.node.bytes).bind_text(2, op.label); });
}

Result<void> put_edge_row(Store& store, const EdgePut& op, const std::string& dots) {
    return run_statement(store, StatementId::EdgePut, [&](Statement& s) {
        s.bind_blob(1, op.id.bytes)
            .bind_blob(2, op.src.bytes)
            .bind_text(3, op.type)
            .bind_blob(4, op.dst.bytes)
            .bind_blob(5, blob_of(dots));
    });
}

// Kimlik kenarın kendisidir: uzak çerçeve var olan kenarın ucunu veya tipini değiştiremez.
Result<void> require_same_edge(Store& store, const EdgePut& op) {
    Result<Statement> row = store.statement(StatementId::EdgeGet);
    if (!row)
        return std::unexpected(row.error());
    row->bind_blob(1, op.id.bytes);
    const Result<bool> found = row->step();
    if (!found)
        return std::unexpected(found.error());
    if (*found && (!std::ranges::equal(row->column_blob(0), std::span<const std::byte>(op.src.bytes)) ||
                   row->column_text(1) != op.type ||
                   !std::ranges::equal(row->column_blob(2), std::span<const std::byte>(op.dst.bytes))))
        return std::unexpected(
            Error{Error::Code::BadFrame, "graph.bad_frame", {{"reason", "edge_identity_changed"}}});
    return {};
}

// Kenar düğümü canlandırmaz: ucu ölüyse durum yazılmaz, işlem günlükte kalır ve sarkan kenar oluşmaz.
Result<void> apply_one(Store& store, const ApplyContext& context, const EdgePut& op) {
    if (const Result<void> same = require_same_edge(store, op); !same)
        return same;
    for (const NodeId& end : {op.src, op.dst}) {
        const Result<std::optional<std::vector<Dot>>> dots = read_node_dots(store, end);
        if (!dots)
            return std::unexpected(dots.error());
        if (!dots->has_value())
            return {};
    }
    Result<std::optional<std::vector<Dot>>> dots = read_edge_dots(store, op.id);
    if (!dots)
        return std::unexpected(dots.error());
    return put_edge_row(store, op, advanced(dots->value_or(std::vector<Dot>{}), context));
}

// Ucu ve tipi aynı satırdan okunur, yalnız dots değişir.
Result<void> update_edge_dots(Store& store, const EdgeId& id, const std::string& dots) {
    Result<Statement> row = store.statement(StatementId::EdgeGet);
    if (!row)
        return std::unexpected(row.error());
    row->bind_blob(1, id.bytes);
    const Result<bool> found = row->step();
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return std::unexpected(corrupt("edge_vanished"));
    return run_statement(store, StatementId::EdgePut, [&](Statement& s) {
        s.bind_blob(1, id.bytes)
            .bind_blob(2, row->column_blob(0))
            .bind_text(3, row->column_text(1))
            .bind_blob(4, row->column_blob(2))
            .bind_blob(5, blob_of(dots));
    });
}

// Düğüm silmeyle aynı kural: kenarın tüm yazmalarını (put ve özellik) görmemiş silme boştur. Add-wins.
Result<void> apply_one(Store& store, const ApplyContext& context, const EdgeDel& op) {
    Result<std::optional<std::vector<Dot>>> dots = read_edge_dots(store, op.id);
    if (!dots)
        return std::unexpected(dots.error());
    if (!dots->has_value() || !surviving(std::move(**dots), context).empty())
        return {};
    return delete_edge_state(store, op.id);
}

// false: kenar yok, yazma düşer. Kenar uçlarını bilmeden kurulamaz. Var olan kenarda dots ilerler.
Result<bool> touch_owner(Store& store, const OwnerId& owner, const ApplyContext& context) {
    if (const NodeId* node = std::get_if<NodeId>(&owner)) {
        const Result<void> touched = touch_node(store, *node, context);
        if (!touched)
            return std::unexpected(touched.error());
        return true;
    }
    const EdgeId& edge = std::get<EdgeId>(owner);
    Result<std::optional<std::vector<Dot>>> dots = read_edge_dots(store, edge);
    if (!dots)
        return std::unexpected(dots.error());
    if (!dots->has_value())
        return false;
    const Result<void> updated = update_edge_dots(store, edge, advanced(std::move(**dots), context));
    if (!updated)
        return std::unexpected(updated.error());
    return true;
}

struct SeenVersion {
    DeviceId dev;
    std::int64_t seq{};
    std::optional<std::int64_t> large_id;
};

Result<std::vector<SeenVersion>> seen_versions(Store& store, std::span<const std::byte> owner,
                                               std::string_view key, const ApplyContext& context) {
    Result<Statement> versions = store.statement(StatementId::PropVersions);
    if (!versions)
        return std::unexpected(versions.error());
    versions->bind_blob(1, owner).bind_text(2, key);
    std::vector<SeenVersion> seen;
    while (true) {
        const Result<bool> row = versions->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return seen;
        const std::span<const std::byte> dev = versions->column_blob(0);
        if (dev.size() != std::tuple_size_v<DeviceId>)
            return std::unexpected(corrupt("bad_device"));
        Dot dot;
        std::ranges::copy(dev, dot.dev.begin());
        dot.seq = static_cast<std::uint64_t>(versions->column_int(1));
        if (is_seen(dot, context))
            seen.push_back(
                SeenVersion{dot.dev, versions->column_int(1), large_reference_id(versions->column_blob(3))});
    }
}

Result<void> delete_versions(Store& store, std::span<const std::byte> owner, std::string_view key,
                             const std::vector<SeenVersion>& versions) {
    for (const SeenVersion& version : versions) {
        if (version.large_id)
            if (const Result<void> deleted = delete_large(store, *version.large_id); !deleted)
                return deleted;
        const Result<void> deleted = run_statement(store, StatementId::PropDeleteVersion, [&](Statement& s) {
            s.bind_blob(1, owner).bind_text(2, key).bind_blob(3, version.dev).bind_int(4, version.seq);
        });
        if (!deleted)
            return deleted;
    }
    return {};
}

// Büyük değer kendi tablosuna gider, satırda [tag|0x80][id][len] kalır: satır okuması onu yüklemez.
Result<std::string> stored_form(Store& store, const Value& value) {
    std::string encoded = encode_value(value);
    if (encoded.size() <= kInlineValueMax)
        return encoded;
    const Result<void> inserted =
        run_statement(store, StatementId::LargePut, [&](Statement& s) { s.bind_blob(1, blob_of(encoded)); });
    if (!inserted)
        return std::unexpected(inserted.error());
    const auto id = static_cast<std::uint64_t>(store.last_insert_id());
    const std::uint64_t size = encoded.size();
    std::string reference;
    reference.reserve(kLargeReferenceBytes);
    reference.push_back(static_cast<char>(static_cast<std::uint8_t>(encoded[0]) | kLargeReferenceBit));
    for (std::size_t i = 0; i < sizeof id; ++i)
        reference.push_back(static_cast<char>(id >> (i * 8U)));
    for (std::size_t i = 0; i < sizeof size; ++i)
        reference.push_back(static_cast<char>(size >> (i * 8U)));
    return reference;
}

std::span<const std::byte> owner_bytes(const OwnerId& owner) {
    if (const NodeId* node = std::get_if<NodeId>(&owner))
        return node->bytes;
    return std::get<EdgeId>(owner).bytes;
}

// Register kuralı: yazma yalnız gördüğü sürümleri siler, kendini ekler; kalanlar head'dir. Null da sürümdür:
// eşzamanlı yazmayla birlikte head olarak görünür ve sonuç uygulama sırasından bağımsız kalır.
Result<void> apply_one(Store& store, const ApplyContext& context, const PropSet& op) {
    const Result<bool> alive = touch_owner(store, op.owner, context);
    if (!alive)
        return std::unexpected(alive.error());
    if (!*alive)
        return {};
    const std::span<const std::byte> owner = owner_bytes(op.owner);
    const Result<std::vector<SeenVersion>> seen = seen_versions(store, owner, op.key, context);
    if (!seen)
        return std::unexpected(seen.error());
    if (const Result<void> deleted = delete_versions(store, owner, op.key, *seen); !deleted)
        return deleted;
    const Result<std::string> stored = stored_form(store, op.value);
    if (!stored)
        return std::unexpected(stored.error());
    return run_statement(store, StatementId::PropPut, [&](Statement& s) {
        s.bind_blob(1, owner)
            .bind_text(2, op.key)
            .bind_blob(3, context.dot.dev)
            .bind_int(4, static_cast<std::int64_t>(context.dot.seq))
            .bind_int(5, static_cast<std::int64_t>(context.hlc))
            .bind_blob(6, blob_of(*stored));
    });
}

Result<std::optional<std::int64_t>> read_meta_int(Store& store, std::string_view key) {
    Result<Statement> statement = store.statement(StatementId::MetaGet);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_text(1, key);
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return std::optional<std::int64_t>{};
    return std::optional<std::int64_t>{statement->column_int(0)};
}

Result<void> write_meta_int(Store& store, std::string_view key, std::int64_t value) {
    return run_statement(store, StatementId::MetaPut,
                         [&](Statement& s) { s.bind_text(1, key).bind_int(2, value); });
}

std::uint64_t now_hlc() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return static_cast<std::uint64_t>(std::max<std::int64_t>(unix_ms, 0)) << kHlcCounterBits;
}

// Tasarım: now + sapma ötesi HLC kendi saatine alınmaz, sıralama için yine kabul edilir. Sapma config'tedir
// (limits.graph.hlc_skew); taşacak kadar büyük ya da sınırsız sapmada her HLC alınır.
bool beyond_skew(std::uint64_t hlc, std::uint64_t now, std::chrono::milliseconds skew) {
    const auto skew_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(skew.count(), 0));
    if (skew_ms > (UINT64_MAX - now) >> kHlcCounterBits)
        return false;
    return hlc > now + (skew_ms << kHlcCounterBits);
}

}  // namespace

std::vector<std::string_view> statement_texts() {
    std::vector<std::string_view> texts;
    for (const StatementEntry& entry : kStatements)
        texts.emplace_back(entry.sql.text);
    return texts;
}

Error storage_error(int code, std::string_view where) {
    Error::Code mapped = Error::Code::Io;
    const int primary = code & 0xFF;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)
        mapped = Error::Code::Busy;
    else if (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB)
        mapped = Error::Code::Corrupt;
    else if (primary == SQLITE_TOOBIG)  // değer ya da işlem depolamanın tek değer sınırını aşıyor
        mapped = Error::Code::LimitExceeded;
    return Error{
        mapped, "graph.storage_error", {{"where", std::string(where)}, {"code", sqlite3_errstr(code)}}};
}

Statement::Statement(Store& store, StatementId id, sqlite3_stmt* handle)
    : store_(&store), id_(id), handle_(handle) {}

Statement::Statement(Statement&& other) noexcept
    : store_(other.store_), id_(other.id_), handle_(std::exchange(other.handle_, nullptr)),
      bind_error_(other.bind_error_) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        release();
        store_ = other.store_;
        id_ = other.id_;
        handle_ = std::exchange(other.handle_, nullptr);
        bind_error_ = other.bind_error_;
    }
    return *this;
}

Statement::~Statement() {
    release();
}

void Statement::release() noexcept {
    if (handle_ != nullptr)
        store_->give_back(id_, std::exchange(handle_, nullptr));
}

// Bağlama hatası (SQLITE_TOOBIG: değer SQLITE_LIMIT_LENGTH'i aşar) yutulursa parametre NULL kalır ve
// sessizce yanlış yazılırdı: ilk hata saklanır, step onu döner. int'e sığmayan boy bağlanmadan reddedilir.
static_assert(SQLITE_OK == 0, "Statement::bind_error_ başlangıcı SQLITE_OK'tur");

void Statement::note_bind(int code) {
    if (bind_error_ == SQLITE_OK)
        bind_error_ = code;
}

// Boş span'ın data()'sı null olabilir; null işaretçi SQL NULL bağlar, boş BLOB değil.
Statement& Statement::bind_blob(int index, std::span<const std::byte> blob) {
    static constexpr char kEmpty = 0;
    if (blob.size() > static_cast<std::size_t>(INT_MAX)) {
        note_bind(SQLITE_TOOBIG);
        return *this;
    }
    const void* data = blob.empty() ? static_cast<const void*>(&kEmpty) : blob.data();
    note_bind(sqlite3_bind_blob(handle_, index, data, static_cast<int>(blob.size()), SQLITE_STATIC));
    return *this;
}

Statement& Statement::bind_text(int index, std::string_view text) {
    static constexpr char kEmpty = 0;
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        note_bind(SQLITE_TOOBIG);
        return *this;
    }
    const char* data = text.empty() ? &kEmpty : text.data();
    note_bind(sqlite3_bind_text(handle_, index, data, static_cast<int>(text.size()), SQLITE_STATIC));
    return *this;
}

Statement& Statement::bind_int(int index, std::int64_t value) {
    note_bind(sqlite3_bind_int64(handle_, index, value));
    return *this;
}

Result<bool> Statement::step() {
    if (bind_error_ != SQLITE_OK)
        return std::unexpected(storage_error(bind_error_, "bind"));
    const int code = sqlite3_step(handle_);
    if (code == SQLITE_ROW)
        return true;
    if (code == SQLITE_DONE)
        return false;
    return std::unexpected(storage_error(code, "step"));
}

Result<void> Statement::run() {
    const Result<bool> first = step();
    if (!first)
        return std::unexpected(first.error());
    sqlite3_reset(handle_);
    return {};
}

std::span<const std::byte> Statement::column_blob(int index) const {
    const void* data = sqlite3_column_blob(handle_, index);
    const int size = sqlite3_column_bytes(handle_, index);
    return {static_cast<const std::byte*>(data), static_cast<std::size_t>(size)};
}

std::string_view Statement::column_text(int index) const {
    const unsigned char* data = sqlite3_column_text(handle_, index);
    const int size = sqlite3_column_bytes(handle_, index);
    return {reinterpret_cast<const char*>(data), static_cast<std::size_t>(size)};
}

std::int64_t Statement::column_int(int index) const {
    return sqlite3_column_int64(handle_, index);
}

CausalContext::CausalContext(std::vector<Dot> seen) : seen_(std::move(seen)) {}

bool CausalContext::covers(const Dot& dot) const {
    return std::ranges::any_of(
        seen_, [&](const Dot& known) { return known.dev == dot.dev && dot.seq <= known.seq; });
}

std::size_t Store::max_blob_bytes() const {
    return static_cast<std::size_t>(std::max(sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, -1), 0));
}

FrameLimits Store::frame_limits() const {
    FrameLimits limits = frame_limits_of(limits_);
    limits.bytes = std::min(limits.bytes, max_blob_bytes());
    return limits;
}

Store::Store(sqlite3* db, const GraphLimits& limits)
    : db_(db), pool_(static_cast<std::size_t>(StatementId::Count)),
      created_(static_cast<std::size_t>(StatementId::Count)), limits_(limits) {}

Store::~Store() {
    end_txn();
    for (std::vector<sqlite3_stmt*>& statements : pool_)
        for (sqlite3_stmt* statement : statements)
            sqlite3_finalize(statement);
    // Tek BUSY nedeni bırakılmamış ifadedir: bir cursor Graph'tan uzun yaşamış demektir (ömür sözleşmesi
    // ihlali).
    if (sqlite3_close(db_) != SQLITE_OK) {
        std::fputs("noteapp_graph: a cursor outlived its graph\n", stderr);
        std::abort();
    }
}

namespace {

struct CloseDatabase {
    void operator()(sqlite3* db) const noexcept { sqlite3_close(db); }
};

// SQLite umask'la oluşturur, WAL ve SHM ana dosyanın modunu kopyalar: dosya SQLite açmadan önce 0600 olmalı.
// Eski sürümün bıraktığı dosyalar da burada daralır. Windows: dosyalar kullanıcı profilinin ACL'sini miras
// alır, ek işlem yok.
Result<void> make_database_private([[maybe_unused]] const std::filesystem::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return std::unexpected(Error{Error::Code::Io, "graph.io", {{"reason", "open_private"}}});
    const int changed = ::fchmod(fd, 0600);
    ::close(fd);
    if (changed != 0)
        return std::unexpected(Error{Error::Code::Io, "graph.io", {{"reason", "chmod_private"}}});
    for (const char* suffix : {"-wal", "-shm"}) {
        std::error_code ignored;  // yoksa sorun değil; SQLite ana dosyanın modunu kopyalar
        std::filesystem::path extra = path;
        extra += suffix;
        std::filesystem::permissions(extra,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ignored);
    }
#endif
    return {};
}

}  // namespace

Result<std::unique_ptr<Store>> Store::open(const std::filesystem::path& path, const GraphLimits& limits) {
    if (const Result<void> made = make_database_private(path); !made)
        return std::unexpected(made.error());
    std::unique_ptr<sqlite3, CloseDatabase> db;
    const std::u8string name = path.u8string();
    const int code = sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), std::out_ptr(db),
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    // Başarısız açılış da kapatılması gereken tanıtıcı döndürür; kurucu ayırma hatası verirse db kapatır,
    // kurulunca sahiplik Store'a geçer.
    std::unique_ptr<Store> store(new Store(db.get(), limits));
    (void)db.release();
    if (code != SQLITE_OK)
        return std::unexpected(storage_error(code, "open"));

    if (const Result<void> configured = store->configure(); !configured)
        return std::unexpected(configured.error());

    // wal.html §9: son bağlantının kapanışı ve kurtarma sırasında busy_timeout'a rağmen SQLITE_BUSY
    // dönebilir; yeni dosyada WAL'a geçiş yarışı da aynıdır. Adımlar idempotent olduğundan süre içinde baştan
    // denenir.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBusyTimeoutMs);
    while (true) {
        const Result<void> ready = store->initialize();
        if (ready)
            return store;
        if (ready.error().code != Error::Code::Busy || std::chrono::steady_clock::now() >= deadline)
            return std::unexpected(ready.error());
        std::this_thread::sleep_for(kOpenRetryPause);
    }
}

// Kimlik denetimi WAL'dan önce: reddedilecek dosyanın başlığına dokunulmaz.
Result<void> Store::initialize() {
    Result<void> step = check_identity();
    if (step)
        step = enable_wal();
    if (step)
        step = migrate();
    if (step)
        step = load_device();
    return step;
}

Result<void> Store::configure() {
    int code = sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    if (code == SQLITE_OK)
        code = sqlite3_db_config(db_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    if (code == SQLITE_OK)
        code = sqlite3_db_config(db_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    // QPSG kapalıyken planlayıcı bağlı değere bakar, her bağlamada ifadeyi expire eder: kiralanan her ifade
    // step'te yeniden derlenir (ölçüm: okuma başına ~3 µs, sorgu süresinin %70'i). Plan zaten INDEXED BY ile
    // sabittir.
    if (code == SQLITE_OK)
        code = sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_QPSG, 1, nullptr);
    if (code != SQLITE_OK)
        return std::unexpected(storage_error(code, "configure"));
    return {};
}

Result<void> Store::check_identity() {
    // Tek anlık görüntü: ayrı okumalar başka sürecin kurulum commit'ine denk gelirse yarım kimlik görüp
    // dosyayı yabancı sanar.
    if (const Result<void> begun = exec(db_, "BEGIN"); !begun)
        return begun;
    const Result<void> checked = check_identity_in_snapshot();
    (void)exec(db_, "ROLLBACK");  // yalnız okuma yapıldı; sonuç yukarıdaki kontrolün sonucudur
    return checked;
}

Result<void> Store::check_identity_in_snapshot() {
    const Result<std::int64_t> application_id = query_int(db_, "PRAGMA application_id");
    if (!application_id)
        return std::unexpected(application_id.error());
    const Result<std::int64_t> version = query_int(db_, "PRAGMA user_version");
    if (!version)
        return std::unexpected(version.error());
    if (*application_id == 0 && *version == 0) {
        const Result<std::int64_t> objects = query_int(db_, "SELECT count(*) FROM sqlite_schema");
        if (!objects)
            return std::unexpected(objects.error());
        if (*objects == 0)
            return {};  // yeni dosya
        return std::unexpected(corrupt("foreign_file"));
    }
    if (*application_id != kApplicationId)
        return std::unexpected(corrupt("foreign_application_id"));
    if (*version > kSchemaVersion)
        return std::unexpected(
            Error{Error::Code::TooNew,
                  "graph.schema_too_new",
                  {{"found", std::to_string(*version)}, {"supported", std::to_string(kSchemaVersion)}}});
    return {};
}

Result<void> Store::enable_wal() {
    // WAL kalıcıdır; zaten açıksa yeniden geçiş kilit istemesin.
    Result<std::string> mode = query_text(db_, "PRAGMA journal_mode");
    if (mode && *mode != "wal")
        mode = query_text(db_, "PRAGMA journal_mode=WAL");
    if (!mode)
        return std::unexpected(mode.error());
    // wal.html: geçiş başarısızsa pragma hata değil eski kipi döner.
    if (*mode != "wal")
        return std::unexpected(Error{Error::Code::Io, "graph.wal_unavailable", {{"mode", *mode}}});
    Result<void> setting = exec(db_, "PRAGMA synchronous=FULL");
    if (setting)
        setting = exec(db_, "PRAGMA cache_size=-8000");
    return setting;
}

Result<void> Store::migrate() {
    const Result<std::int64_t> quick = query_int(db_, "PRAGMA user_version");
    if (!quick)
        return std::unexpected(quick.error());
    if (*quick == kSchemaVersion)
        return {};

    if (const Result<void> begun = exec(db_, "BEGIN IMMEDIATE"); !begun)
        return begun;
    const auto run_steps = [&]() -> Result<void> {
        // Kilidi beklerken başka süreç kurmuş olabilir: karar kilit altında yeniden verilir.
        const Result<std::int64_t> version = query_int(db_, "PRAGMA user_version");
        if (!version)
            return std::unexpected(version.error());
        if (*version != 0)
            return {};
        for (const Sql& sql : kSchemaV1)
            if (const Result<void> created = exec(db_, sql); !created)
                return created;
        const NodeId device = new_node_id();
        return run_statement(*this, StatementId::MetaPut,
                             [&](Statement& s) { s.bind_text(1, kMetaDevice).bind_blob(2, device.bytes); });
    };
    Result<void> result = run_steps();
    if (result)
        result = exec(db_, "COMMIT");
    if (!result)
        (void)exec(db_, "ROLLBACK");  // asıl hata döner; geri alma hatası onu maskelemesin
    return result;
}

Result<void> Store::load_device() {
    Result<Statement> statement = this->statement(StatementId::MetaGet);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_text(1, kMetaDevice);
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row || statement->column_blob(0).size() != device_.size())
        return std::unexpected(corrupt("bad_device_id"));
    std::ranges::copy(statement->column_blob(0), device_.begin());
    return {};
}

Result<sqlite3_stmt*> Store::prepare(StatementId id) {
    std::vector<sqlite3_stmt*>& free_list = pool_[static_cast<std::size_t>(id)];
    if (!free_list.empty()) {
        sqlite3_stmt* handle = free_list.back();
        free_list.pop_back();
        return handle;
    }
    // give_back noexcept'tir: geri dönecek yer ifade kurulmadan ayrılır, yoksa bad_alloc süreci
    // sonlandırırdı.
    created_[static_cast<std::size_t>(id)] += 1;
    free_list.reserve(created_[static_cast<std::size_t>(id)]);
    sqlite3_stmt* handle = nullptr;
    const int code =
        prepare_sql(db_, kStatements[static_cast<std::size_t>(id)].sql, SQLITE_PREPARE_PERSISTENT, &handle);
    if (code != SQLITE_OK)
        return std::unexpected(storage_error(code, "prepare"));
    return handle;
}

void Store::give_back(StatementId id, sqlite3_stmt* handle) noexcept {
    sqlite3_reset(handle);  // dönüş kodu son step'in hatasıdır, step zaten bildirdi
    sqlite3_clear_bindings(handle);
    pool_[static_cast<std::size_t>(id)].push_back(handle);
}

Result<Statement> Store::statement(StatementId id) {
    const Result<sqlite3_stmt*> handle = prepare(id);
    if (!handle)
        return std::unexpected(handle.error());
    return Statement(*this, id, *handle);
}

std::int64_t Store::last_insert_id() const {
    return sqlite3_last_insert_rowid(db_);
}

Result<void> Store::set_query_only(bool enabled) {
    return enabled ? exec(db_, "PRAGMA query_only=ON") : exec(db_, "PRAGMA query_only=OFF");
}

Result<void> Store::begin_read() {
    if (const Result<void> begun = exec(db_, "BEGIN"); !begun)
        return begun;
    txn_ = TxnKind::Read;
    return {};
}

Result<void> Store::begin_write() {
    if (const Result<void> begun = exec(db_, "BEGIN IMMEDIATE"); !begun)
        return begun;
    txn_ = TxnKind::Write;
    poisoned_ = false;
    low_priority_ = false;
    const Result<void> started = start_frame();
    if (!started)
        end_txn();
    return started;
}

// seq ve HLC yazma kilidi altında okunur: süreçler arası tek zincir.
Result<void> Store::start_frame() {
    const Result<std::optional<std::int64_t>> seq = read_meta_int(*this, kMetaSeq);
    if (!seq)
        return std::unexpected(seq.error());
    const Result<std::optional<std::int64_t>> hlc = read_meta_int(*this, kMetaHlc);
    if (!hlc)
        return std::unexpected(hlc.error());

    std::vector<Dot> deps;
    Result<Statement> peers = statement(StatementId::PeersOfDevice);
    if (!peers)
        return std::unexpected(peers.error());
    peers->bind_blob(1, blob_of(device_));
    while (true) {
        const Result<bool> row = peers->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            break;
        const std::span<const std::byte> other = peers->column_blob(0);
        if (other.size() != device_.size())
            return std::unexpected(corrupt("bad_peer"));
        Dot dep;
        std::ranges::copy(other, dep.dev.begin());
        dep.seq = static_cast<std::uint64_t>(peers->column_int(1));
        deps.push_back(dep);
    }

    const auto last_seq = static_cast<std::uint64_t>(seq->value_or(0));
    const auto last_hlc = static_cast<std::uint64_t>(hlc->value_or(0));
    frame_ = Frame{};
    frame_.seq = last_seq + 1;
    // Saat geri gitse de HLC artar; sayaç taşması milisaniyeye yürür.
    frame_.hlc = std::max(now_hlc(), last_hlc + 1);
    frame_.deps = deps;
    std::vector<Dot> seen = std::move(deps);
    seen.push_back(Dot{device_, last_seq});
    apply_context_ = ApplyContext{Dot{device_, frame_.seq}, frame_.hlc, CausalContext(std::move(seen))};
    return {};
}

// Çerçeve (kayıt) sınırı burada uygulanmaz: export işlemi kayıtlara böler (L4b). İşlemin tamamı ops satırına,
// yani depolamanın tek değer sınırına sığmalıdır; aşarsa OpPut LimitExceeded döner.
Result<void> Store::store_frame() {
    if (frame_.deps.size() > frame_limits().deps)
        return std::unexpected(
            Error{Error::Code::LimitExceeded, "graph.limit_exceeded", {{"limit", "deps"}}});
    const std::string encoded = encode_frame(frame_);
    const Result<void> stored = run_statement(*this, StatementId::OpPut, [&](Statement& s) {
        s.bind_blob(1, device_)
            .bind_int(2, static_cast<std::int64_t>(frame_.seq))
            .bind_blob(3, blob_of(encoded));
    });
    if (!stored)
        return stored;
    if (const Result<void> seq = write_meta_int(*this, kMetaSeq, static_cast<std::int64_t>(frame_.seq)); !seq)
        return seq;
    if (low_priority_)  // cihazın saati geri gitmez: sonraki yazmalar yine gerçek saatle
        return {};
    return write_meta_int(*this, kMetaHlc, static_cast<std::int64_t>(frame_.hlc));
}

Result<void> Store::set_lowest_priority() {
    if (!frame_.ops.empty())
        return std::unexpected(
            Error{Error::Code::InvalidArgument, "graph.invalid_argument", {{"reason", "txn_has_ops"}}});
    low_priority_ = true;
    frame_.hlc = kLowestHlc;
    apply_context_.hlc = kLowestHlc;
    return {};
}

Result<void> Store::commit_write() {
    Result<void> result;
    if (poisoned_)
        result = std::unexpected(Error{Error::Code::Io, "graph.txn_poisoned", {}});
    else if (!frame_.ops.empty())
        result = store_frame();
    if (result)
        result = exec(db_, "COMMIT");
    if (result)
        txn_ = TxnKind::None;
    else
        end_txn();  // lang_transaction.html: başarısız COMMIT işlemi açık bırakabilir
    frame_ = Frame{};
    return result;
}

void Store::end_txn() noexcept {
    if (txn_ == TxnKind::None)
        return;
    txn_ = TxnKind::None;
    frame_ = Frame{};
    const Result<void> rolled_back = exec(db_, "ROLLBACK");
    // SQLite bazı hatalarda (ör. SQLITE_FULL) işlemi kendi geri alır: ROLLBACK o zaman hata döner ama işlem
    // kapalıdır. İşlem hâlâ açıksa sonraki yazma bu işlemin parçası olurdu; devam etmek veri bozardı.
    if (!rolled_back && sqlite3_get_autocommit(db_) == 0) {
        std::fputs("noteapp_graph: rollback failed\n", stderr);
        std::abort();
    }
}

Result<void> Store::record(Op op) {
    if (poisoned_)
        return std::unexpected(Error{Error::Code::Io, "graph.txn_poisoned", {}});
    const Result<void> applied = apply(apply_context_, op);
    if (!applied) {
        poisoned_ = true;
        return applied;
    }
    frame_.ops.push_back(std::move(op));
    return {};
}

Result<void> Store::apply(const ApplyContext& context, const Op& op) {
    return std::visit([&](const auto& alternative) { return apply_one(*this, context, alternative); }, op);
}

Result<std::uint64_t> Store::applied_seq(const DeviceId& device) {
    if (device == device_) {
        const Result<std::optional<std::int64_t>> seq = read_meta_int(*this, kMetaSeq);
        if (!seq)
            return std::unexpected(seq.error());
        return static_cast<std::uint64_t>(seq->value_or(0));
    }
    Result<Statement> peer = statement(StatementId::PeerGet);
    if (!peer)
        return std::unexpected(peer.error());
    peer->bind_blob(1, blob_of(device_)).bind_blob(2, blob_of(device));
    const Result<bool> row = peer->step();
    if (!row)
        return std::unexpected(row.error());
    return *row ? static_cast<std::uint64_t>(peer->column_int(0)) : std::uint64_t{0};
}

Result<void> Store::set_applied(const DeviceId& device, std::uint64_t seq) {
    if (device == device_)
        return write_meta_int(*this, kMetaSeq, static_cast<std::int64_t>(seq));
    return run_statement(*this, StatementId::PeerPut, [&](Statement& s) {
        s.bind_blob(1, blob_of(device_))
            .bind_blob(2, blob_of(device))
            .bind_int(3, static_cast<std::int64_t>(seq));
    });
}

Result<std::uint64_t> Store::peer_count() {
    Result<Statement> count = statement(StatementId::PeerCount);
    if (!count)
        return std::unexpected(count.error());
    count->bind_blob(1, blob_of(device_));
    const Result<bool> row = count->step();
    if (!row)
        return std::unexpected(row.error());
    return *row ? static_cast<std::uint64_t>(count->column_int(0)) : std::uint64_t{0};
}

Result<bool> Store::rebuild_pending() {
    const Result<std::optional<std::int64_t>> pending = read_meta_int(*this, kMetaRebuildPending);
    if (!pending)
        return std::unexpected(pending.error());
    return pending->value_or(0) != 0;
}

Result<void> Store::set_rebuild_pending(bool pending) {
    return write_meta_int(*this, kMetaRebuildPending, pending ? 1 : 0);
}

Result<void> Store::observe_hlc(std::uint64_t hlc) {
    if (beyond_skew(hlc, now_hlc(), limits_.hlc_skew))
        return {};
    const Result<std::optional<std::int64_t>> known = read_meta_int(*this, kMetaHlc);
    if (!known)
        return std::unexpected(known.error());
    if (hlc <= static_cast<std::uint64_t>(known->value_or(0)))
        return {};
    return write_meta_int(*this, kMetaHlc, static_cast<std::int64_t>(hlc));
}

Result<void> Store::set_bulk_sync(bool enabled) {
    return enabled ? exec(db_, "PRAGMA synchronous=NORMAL") : exec(db_, "PRAGMA synchronous=FULL");
}

Result<void> Store::begin_savepoint() {
    return exec(db_, "SAVEPOINT remote_frame");
}

Result<void> Store::release_savepoint() {
    return exec(db_, "RELEASE remote_frame");
}

Result<void> Store::rollback_savepoint() {
    if (const Result<void> rolled_back = exec(db_, "ROLLBACK TO remote_frame"); !rolled_back)
        return rolled_back;
    return release_savepoint();
}

// Düğümler gidince kenarlar, etiketler ve özellikler de gider: sarkan kenar olmadığından artık kalmaz.
Result<void> Store::clear_graph() {
    std::vector<NodeId> nodes;
    {
        Result<Statement> all = statement(StatementId::NodesAll);
        if (!all)
            return std::unexpected(all.error());
        while (true) {
            const Result<bool> row = all->step();
            if (!row)
                return std::unexpected(row.error());
            if (!*row)
                break;
            const std::span<const std::byte> blob = all->column_blob(0);
            if (blob.size() != sizeof(NodeId::bytes))
                return std::unexpected(corrupt("bad_node_id"));
            NodeId id;
            std::ranges::copy(blob, id.bytes.begin());
            nodes.push_back(id);
        }
    }
    for (const NodeId& node : nodes)
        if (const Result<void> deleted = delete_node_state(*this, node); !deleted)
            return deleted;
    return {};
}

Result<Store::StoredValue> Store::peek_value(std::span<const std::byte> stored) {
    if (is_large_reference(stored)) {
        if (stored.size() != kLargeReferenceBytes)
            return std::unexpected(corrupt("bad_large_reference"));
        std::uint64_t encoded_size = 0;
        for (std::size_t i = 0; i < sizeof encoded_size; ++i)
            encoded_size |= std::to_integer<std::uint64_t>(stored[1 + sizeof(std::uint64_t) + i]) << (i * 8U);
        return StoredValue{Value{}, encoded_size == 0 ? 0 : encoded_size - 1,
                           true};  // etiket baytı içerik değil
    }
    Result<Value> value = load_value(stored);
    if (!value)
        return std::unexpected(value.error());
    std::uint64_t size = stored.size();
    if (const std::string* text = std::get_if<std::string>(&*value))
        size = text->size();
    else if (const Bytes* bytes = std::get_if<Bytes>(&*value))
        size = bytes->data.size();
    return StoredValue{std::move(*value), size, false};
}

Result<std::uint64_t> Store::data_version() {
    const Result<std::int64_t> version = query_int(db_, "PRAGMA data_version");
    if (!version)
        return std::unexpected(version.error());
    return static_cast<std::uint64_t>(*version);
}

Result<Value> Store::load_value(std::span<const std::byte> stored) {
    const auto as_corrupt = [](const Error& error) -> Error {
        return error.code == Error::Code::LimitExceeded ? error : corrupt("bad_stored_value");
    };
    if (!is_large_reference(stored))
        return decode_value(stored).transform_error(as_corrupt);
    const std::optional<std::int64_t> id = large_reference_id(stored);
    if (!id)
        return std::unexpected(corrupt("bad_large_reference"));
    Result<Statement> statement = this->statement(StatementId::LargeGet);
    if (!statement)
        return std::unexpected(statement.error());
    statement->bind_int(1, *id);
    const Result<bool> row = statement->step();
    if (!row)
        return std::unexpected(row.error());
    if (!*row)
        return std::unexpected(corrupt("missing_large_value"));
    return decode_value(statement->column_blob(0)).transform_error(as_corrupt);
}

}  // namespace noteapp::graph::detail
