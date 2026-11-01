// Gerçek süreçlerle dayanıklılık ve eşzamanlılık (SPEC-graph kabul 2). fork + SIGKILL POSIX'tir; Windows'ta
// kilit davranışı için ayrı doğrulama gerekir ve bu testler orada derlenmez.
#ifndef _WIN32

#include "graph/op.hpp"
#include "graph_test_support.hpp"
#include "support/test_support.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <doctest/doctest.h>
#include <random>
#include <set>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace noteapp::graph;
using noteapp::test::TempDir;
using test::as_bytes;
using test::begin_read;
using test::node_named;
using test::RawDb;

namespace {

constexpr int kPropsPerTxn = 8;

std::string prop_key(int index) {
    return "p" + std::to_string(index);
}

// Bir işlem: düğüm + etiket + özellikler + büyük değer + önceki düğüme kenar. Kısmi kalan her parça test
// edilir.
Result<void> write_commit(Graph& graph, int index) {
    Result<WriteTxn> txn = graph.write();
    if (!txn)
        return std::unexpected(txn.error());
    const NodeId id = node_named("n" + std::to_string(index));
    Result<void> step = txn->put_node(id);
    if (step)
        step = txn->add_label(id, "Crash");
    for (int i = 0; step && i < kPropsPerTxn; ++i)
        step = txn->set_property(id, prop_key(i), std::int64_t{index});
    if (step)
        step = txn->set_property(id, "big", Bytes{std::string(4096, static_cast<char>('a' + index % 26))});
    if (step && index > 0)
        step = txn->put_edge(test::edge_named("e" + std::to_string(index)),
                             node_named("n" + std::to_string(index - 1)), "PREV", id);
    if (!step)
        return step;
    return txn->commit();
}

// Çocuk: commit eder, sonra pipe'a bildirir. Sonsuza dek döner; ebeveyn SIGKILL gönderir.
[[noreturn]] void crash_writer(const std::filesystem::path& path, int first_index, int ack_fd) {
    Result<Graph> graph = Graph::open(path);
    if (!graph)
        _exit(3);
    for (int index = first_index;; ++index) {
        if (!write_commit(*graph, index))
            _exit(4);
        const std::array<char, sizeof(int)> ack = std::bit_cast<std::array<char, sizeof(int)>>(index);
        if (write(ack_fd, ack.data(), ack.size()) != static_cast<ssize_t>(ack.size()))
            _exit(5);
    }
}

// Bir tur: yazıcıyı başlat, rastgele sürede öldür, son onaylanan indeksi döndür.
int run_crash_round(const std::filesystem::path& path, int first_index, std::chrono::microseconds delay) {
    std::array<int, 2> ack_pipe{};
    REQUIRE(pipe(ack_pipe.data()) == 0);
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        close(ack_pipe[0]);
        crash_writer(path, first_index, ack_pipe[1]);
    }
    close(ack_pipe[1]);
    int acked = first_index - 1;
    // İlk onayı bekle: en az bir commit olmadan tur bir şey kanıtlamaz.
    std::array<char, sizeof(int)> ack{};
    REQUIRE(read(ack_pipe[0], ack.data(), ack.size()) == static_cast<ssize_t>(ack.size()));
    acked = std::bit_cast<int>(ack);
    std::this_thread::sleep_for(delay);
    REQUIRE(kill(child, SIGKILL) == 0);
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status));
    while (read(ack_pipe[0], ack.data(), ack.size()) == static_cast<ssize_t>(ack.size()))
        acked = std::bit_cast<int>(ack);
    close(ack_pipe[0]);
    return acked;
}

void check_consistent_after_crash(const std::filesystem::path& path, int acked) {
    // Önce motor açar: öldürülen yazıcının WAL'ı burada kurtarılır.
    Result<Graph> reopened = Graph::open(path);
    REQUIRE_MESSAGE(reopened.has_value(), (reopened.has_value() ? "" : reopened.error().message_key));
    ReadTxn txn = begin_read(*reopened);

    RawDb raw(path);
    const std::int64_t committed = raw.scalar("SELECT count(*) FROM nodes");
    // Onaylanan her işlem görünür. Onaylanmamış en çok biri de commit olmuş olabilir (commit ile bildirim
    // arasında öldü).
    CHECK(committed >= acked + 1);
    CHECK(committed <= acked + 2);

    // Veri ile günlük birlikte: düğüm başına tam bir çerçeve, delik yok, sayaç aynı.
    CHECK(raw.scalar("SELECT count(*) FROM ops") == committed);
    CHECK(raw.scalar("SELECT max(seq) FROM ops") == committed);
    CHECK(raw.scalar("SELECT v FROM meta WHERE k='seq'") == committed);
    // Yarım işlem parçası yok: her düğümün tüm parçaları var.
    CHECK(raw.scalar("SELECT count(*) FROM labels") == committed);
    CHECK(raw.scalar("SELECT count(*) FROM props") == committed * (kPropsPerTxn + 1));
    CHECK(raw.scalar("SELECT count(*) FROM large_values") == committed);
    CHECK(raw.scalar("SELECT count(*) FROM edges") == committed - 1);
    CHECK(raw.rows("PRAGMA integrity_check") == std::vector<std::string>{"ok"});

    for (int index = 0; index < committed; ++index) {
        const NodeId id = node_named("n" + std::to_string(index));
        const Result<bool> exists = txn.has_node(id);
        REQUIRE(exists.has_value());
        REQUIRE(*exists);
        const Result<Value> first = txn.property(id, prop_key(0));
        REQUIRE(first.has_value());
        CHECK(*first == Value{std::int64_t{index}});
    }
    const std::vector<std::string> frames = raw.rows("SELECT frame FROM ops ORDER BY seq DESC LIMIT 1");
    REQUIRE(frames.size() == 1);
    const Result<detail::Frame> frame = detail::decode_frame(as_bytes(frames[0]));
    REQUIRE(frame.has_value());
    CHECK(frame->seq == static_cast<std::uint64_t>(committed));
}

}  // namespace

TEST_CASE("kill_9_keeps_every_acked_commit_with_its_op_and_no_partial_transaction") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    std::mt19937 random(20261101);  // tekrarlanabilir zamanlama seçimi
    std::uniform_int_distribution<int> delay_us(0, 30000);
    int committed = 0;
    constexpr int kRounds = 15;
    for (int round = 0; round < kRounds; ++round) {
        INFO("round " << round);
        const int acked = run_crash_round(path, committed, std::chrono::microseconds(delay_us(random)));
        check_consistent_after_crash(path, acked);
        RawDb raw(path);
        committed = static_cast<int>(raw.scalar("SELECT count(*) FROM nodes"));
    }
    CHECK(committed >= kRounds);
}

TEST_CASE("concurrent_processes_lose_no_writes_while_creating_the_same_file") {
    TempDir dir;
    const auto path = dir.path() / "graph.db";
    constexpr int kProcesses = 6;
    constexpr int kCommitsPerProcess = 40;
    const NodeId shared = node_named("shared");

    std::array<int, 2> gate{};
    REQUIRE(pipe(gate.data()) == 0);
    std::vector<pid_t> children;
    for (int process = 0; process < kProcesses; ++process) {
        const pid_t child = fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            // Hepsi aynı anda başlasın: yeni dosyayı kurma ve WAL'a geçme yarışı da sınanır.
            close(gate[1]);
            char byte = 0;
            (void)!read(gate[0], &byte, 1);
            Result<Graph> graph = Graph::open(path);
            if (!graph) {
                // Çocukta doctest yok: sebep testin çıktısında görünsün.
                std::fprintf(stderr, "open failed: %s %s\n", graph.error().message_key.c_str(),
                             graph.error().args.empty() ? "" : graph.error().args.back().second.c_str());
                _exit(10);
            }
            for (int i = 0; i < kCommitsPerProcess; ++i) {
                // Oku-değiştir-yaz: yazma kilidi anlık görüntüden önce alınmazsa sayaç kaybolur.
                Result<WriteTxn> txn = graph->write();
                if (!txn)
                    _exit(11);
                if (!txn->put_node(shared))
                    _exit(12);
                const Result<Value> current = txn->property(shared, "count");
                if (!current)
                    _exit(13);
                const std::int64_t count =
                    std::holds_alternative<std::int64_t>(*current) ? std::get<std::int64_t>(*current) : 0;
                if (!txn->set_property(shared, "count", count + 1))
                    _exit(14);
                if (!txn->put_node(node_named(std::to_string(process) + "/" + std::to_string(i))))
                    _exit(15);
                if (!txn->commit())
                    _exit(16);
            }
            _exit(0);
        }
        children.push_back(child);
    }
    close(gate[0]);
    close(gate[1]);  // kapanış tüm çocukları aynı anda serbest bırakır

    for (const pid_t child : children) {
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }

    Result<Graph> graph = Graph::open(path);
    REQUIRE(graph.has_value());
    ReadTxn txn = begin_read(*graph);
    const std::int64_t total = kProcesses * kCommitsPerProcess;
    CHECK(*txn.property(shared, "count") == Value{total});

    RawDb raw(path);
    CHECK(raw.scalar("SELECT count(*) FROM nodes") == total + 1);
    CHECK(raw.scalar("SELECT count(*) FROM ops") == total);
    // Aynı dosya aynı cihaz: seq süreçler arası tektir ve deliksizdir.
    CHECK(raw.scalar("SELECT count(DISTINCT dev) FROM ops") == 1);
    CHECK(raw.scalar("SELECT min(seq) FROM ops") == 1);
    CHECK(raw.scalar("SELECT max(seq) FROM ops") == total);
    CHECK(raw.scalar("SELECT count(*) FROM props") == 1);
    CHECK(raw.rows("PRAGMA integrity_check") == std::vector<std::string>{"ok"});
}

#endif  // _WIN32
