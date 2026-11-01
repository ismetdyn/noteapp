#pragma once

// Testlerin dosyaya motoru atlayarak bakması için ince SQLite sarmalayıcısı: motor kendi yazdığını kendi
// okuyarak kanıtlayamaz. Yalnız bu klasörde kullanılır.
#include "graph/graph.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <memory>
#include <span>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::graph::test {

class RawDb {
public:
    explicit RawDb(const std::filesystem::path& path) {
        const std::u8string name = path.u8string();
        if (sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &db_) != SQLITE_OK)
            throw std::runtime_error("open failed");
    }
    ~RawDb() { sqlite3_close(db_); }
    RawDb(const RawDb&) = delete;
    RawDb& operator=(const RawDb&) = delete;

    void exec(const std::string& sql) {
        char* message = nullptr;
        const int code = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
        const std::string detail = message == nullptr ? "" : message;
        sqlite3_free(message);
        if (code != SQLITE_OK)
            throw std::runtime_error(sql + ": " + detail);
    }

    // Sütunları '|' ile birleştirilmiş satırlar. BLOB ham baytlarıyla gelir.
    std::vector<std::string> rows(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error(sql + ": " + sqlite3_errmsg(db_));
        std::vector<std::string> out;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            std::string row;
            for (int i = 0; i < sqlite3_column_count(statement); ++i) {
                const void* data = sqlite3_column_blob(statement, i);
                if (i > 0)
                    row += '|';
                if (data != nullptr)
                    row.append(static_cast<const char*>(data),
                               static_cast<std::size_t>(sqlite3_column_bytes(statement, i)));
            }
            out.push_back(std::move(row));
        }
        sqlite3_finalize(statement);
        return out;
    }

    std::int64_t scalar(const std::string& sql) {
        const std::vector<std::string> result = rows(sql);
        if (result.size() != 1)
            throw std::runtime_error(sql + ": expected one row");
        return std::stoll(result[0]);
    }

    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

inline Graph open_graph(const std::filesystem::path& path) {
    Result<Graph> graph = Graph::open(path);
    REQUIRE_MESSAGE(graph.has_value(), (graph.has_value() ? "" : graph.error().message_key));
    return std::move(*graph);
}

inline WriteTxn begin_write(Graph& graph) {
    Result<WriteTxn> txn = graph.write();
    REQUIRE_MESSAGE(txn.has_value(), (txn.has_value() ? "" : txn.error().message_key));
    return std::move(*txn);
}

inline ReadTxn begin_read(Graph& graph) {
    Result<ReadTxn> txn = graph.read();
    REQUIRE_MESSAGE(txn.has_value(), (txn.has_value() ? "" : txn.error().message_key));
    return std::move(*txn);
}

// Hata iletisi başarısız beklenti satırında görünsün diye makro.
#define GRAPH_OK(expression)                                                                                 \
    do {                                                                                                     \
        const auto graph_ok_result = (expression);                                                           \
        REQUIRE_MESSAGE(graph_ok_result.has_value(),                                                         \
                        (graph_ok_result.has_value() ? "" : graph_ok_result.error().message_key));           \
    } while (false)

inline std::span<const std::byte> as_bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

inline NodeId node_named(std::string_view name) {
    return derive_node_id("test", as_bytes(name));
}

inline EdgeId edge_named(std::string_view name) {
    return derive_edge_id("test", as_bytes(name));
}

}  // namespace noteapp::graph::test
