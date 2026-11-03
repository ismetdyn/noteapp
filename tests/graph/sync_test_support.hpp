#pragma once

// Cihazlar arası testlerin ortak düzeneği: her Device'ın kendi graph.db'si ve çıkış dizini var.
#include "graph/oplog.hpp"
#include "graph_test_support.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::graph::test {

inline void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(out.good());
}

// Satırların tamamı; büyük değer içeriğine açılır, çünkü large_values kimliği cihaza özgüdür.
inline std::string dump_of(const std::filesystem::path& db) {
    RawDb raw(db);
    std::string out;
    for (const std::string& row : raw.rows("SELECT id, dots FROM nodes ORDER BY id"))
        out += "node " + row + "\n";
    for (const std::string& row : raw.rows("SELECT node, label, dots FROM labels ORDER BY node, label"))
        out += "label " + row + "\n";
    for (const std::string& row : raw.rows("SELECT id, src, type, dst, dots FROM edges ORDER BY id"))
        out += "edge " + row + "\n";
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(
                raw.get(), "SELECT owner, key, dev, seq, hlc, value FROM props ORDER BY owner, key, dev, seq",
                -1, &statement, nullptr) == SQLITE_OK);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        for (int column = 0; column < 5; ++column) {
            const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, column));
            out.append(data, static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
            out += '|';
        }
        const auto* value = static_cast<const char*>(sqlite3_column_blob(statement, 5));
        const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement, 5));
        if ((static_cast<unsigned char>(value[0]) & 0x80U) != 0) {
            std::uint64_t id = 0;
            for (std::size_t i = 0; i < sizeof id; ++i)
                id |= static_cast<std::uint64_t>(static_cast<unsigned char>(value[1 + i])) << (i * 8U);
            out += raw.rows("SELECT data FROM large_values WHERE id=" + std::to_string(id)).at(0);
        } else {
            out.append(value, size);
        }
        out += '\n';
    }
    sqlite3_finalize(statement);
    return out;
}

class Device {
public:
    Device() : Device(GraphLimits{}) {}
    // Config'ten gelen motor sınırlarıyla açılan cihaz (limits.graph.*).
    explicit Device(const GraphLimits& limits)
        : db_(dir_.path() / "graph.db"), outbox_(dir_.path() / "out"), limits_(limits),
          graph_(open_graph(db_, limits)) {
        std::filesystem::create_directories(outbox_);
    }

    Graph& graph() { return *graph_; }
    const std::filesystem::path& db() const { return db_; }
    const std::filesystem::path& outbox() const { return outbox_; }
    const std::filesystem::path& dir() const { return dir_.path(); }

    void close() { graph_.reset(); }
    void reopen() { graph_ = open_graph(db_, limits_); }

    std::filesystem::path exported() {
        GRAPH_OK(graph_->export_log(outbox_));
        return log_path();
    }

    // Bu cihazın kendi günlüğü; henüz yazılmadıysa boş yol.
    std::filesystem::path log_path() const {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(outbox_))
            if (entry.path().extension() == ".oplog")
                return entry.path();
        return {};
    }

    MergeReport ingest(const std::vector<std::filesystem::path>& logs) {
        Result<MergeReport> report = graph_->ingest_logs(logs);
        REQUIRE_MESSAGE(report.has_value(), (report.has_value() ? "" : report.error().message_key));
        return std::move(*report);
    }

    std::string dump() const { return dump_of(db_); }

    void commit(const auto& change) {
        WriteTxn txn = begin_write(*graph_);
        change(txn);
        GRAPH_OK(txn.commit());
    }

private:
    noteapp::test::TempDir dir_;
    std::filesystem::path db_;
    std::filesystem::path outbox_;
    GraphLimits limits_;
    std::optional<Graph> graph_;
};

// Elle kurulmuş günlük: doğrulamadan geçer, içeriği kötü niyetli olabilir.
inline std::filesystem::path write_log(const std::filesystem::path& directory, const detail::DeviceId& device,
                                       std::span<const std::string> frames) {
    std::filesystem::create_directories(directory);
    std::string bytes = detail::encode_log_header(device);
    for (const std::string& frame : frames)
        bytes += detail::encode_log_frame(frame);
    const std::filesystem::path path = directory / detail::log_file_name(device);
    write_file(path, bytes);
    return path;
}

}  // namespace noteapp::graph::test
