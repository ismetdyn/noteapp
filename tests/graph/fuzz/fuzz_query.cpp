// libFuzzer: Graph::query uçtan uca. İlk bayt kip ('W' yazma, diğerleri salt-okunur), kalanı Cypher metni.
// Kabul 5: crash, 10 sn'yi aşan sorgu ve sızıntı hatadır; sonucun kendisi önemli değil.
#include "graph/graph.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace noteapp::graph;
namespace fs = std::filesystem;

namespace {

constexpr int kWritesPerSeed = 200;  // yazmalar birikince şema başa döner, bellek ve dosya büyümesin
constexpr std::string_view kSchema =
    "CREATE (a:A {x: 1, s: 'yazı'})-[:T {w: 1}]->(b:B {x: 2.5}), (b)-[:T {w: 2}]->(a), (a)-[:U]->(a), "
    "(b)-[:U]->(c:C {flag: true}), (c)-[:T]->(d:A:B {x: 1}), (:E)";

QueryLimits fuzz_limits() {
    QueryLimits limits;
    limits.max_rows = 1000;
    limits.max_expansions = 20'000;
    limits.max_duration = std::chrono::milliseconds(1000);
    limits.max_memory_bytes = 32U * 1024U * 1024U;
    limits.max_memo_bytes = 1024U * 1024U;
    return limits;
}

const Params& fuzz_params() {
    static const Params params = {
        {"v", std::int64_t{1}},
        {"s", "yazı"},
        {"b", Bytes{std::string("\xFF\x00", 2)}},
        {"l", QueryList{{QueryValue(std::int64_t{1}), QueryValue("x"), QueryValue{}}}},
    };
    return params;
}

class Fixture {
public:
    Fixture() : dir_(fs::temp_directory_path() / ("noteapp-fuzz-query-" + std::to_string(getpid()))) {
        reseed();
    }

    Graph& graph() { return *graph_; }

    void after_write() {
        if (++writes_ >= kWritesPerSeed)
            reseed();
    }

private:
    [[noreturn]] static void die(const std::string& what) {
        std::fprintf(stderr, "fuzz fixture: %s\n", what.c_str());
        std::abort();
    }

    void reseed() {
        graph_.reset();
        writes_ = 0;
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
        fs::create_directories(dir_);
        Result<Graph> opened = Graph::open(dir_ / "graph.db");
        if (!opened)
            die(opened.error().message_key);
        graph_ = std::make_unique<Graph>(std::move(*opened));
        const Result<QueryResult> seeded = graph_->query(kSchema, {}, QueryMode::ReadWrite);
        if (!seeded)
            die(seeded.error().message_key);
    }

    fs::path dir_;
    std::unique_ptr<Graph> graph_;
    int writes_ = 0;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static Fixture fixture;
    if (size == 0)
        return 0;
    const bool write = data[0] == 'W';
    const std::string_view text(reinterpret_cast<const char*>(data) + 1, size - 1);
    const QueryMode mode = write ? QueryMode::ReadWrite : QueryMode::ReadOnly;
    (void)fixture.graph().query(text, fuzz_params(), mode, fuzz_limits());
    // Sorgu ne dönerse dönsün işlem açık kalmamalı: açık kalsa sonraki her sorgu sessizce kırılırdı.
    if (!fixture.graph().read().has_value())
        std::abort();
    if (write)
        fixture.after_write();
    return 0;
}
