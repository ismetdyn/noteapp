#include "graph/merge.hpp"
#include "graph/op.hpp"
#include "graph/store.hpp"
#include "graph_test_support.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;
using noteapp::test::TempDir;
using test::as_bytes;
using test::edge_named;
using test::node_named;

namespace {

DeviceId device(int number) {
    DeviceId id{};
    id[0] = static_cast<std::byte>(number);
    return id;
}

const DeviceId kA = device(1);
const DeviceId kB = device(2);
const DeviceId kC = device(3);
const DeviceId kD = device(4);
const NodeId kN = node_named("n");
const NodeId kM = node_named("m");
const EdgeId kE = edge_named("e");

struct Authored {
    DeviceId dev;
    Frame frame;
};

Authored authored(const DeviceId& dev, std::uint64_t seq, std::vector<Dot> deps, std::vector<Op> ops) {
    Frame frame;
    frame.seq = seq;
    frame.hlc = (seq << 8U) + static_cast<std::uint64_t>(dev[0]);
    frame.deps = std::move(deps);
    frame.ops = std::move(ops);
    return {dev, std::move(frame)};
}

Dot dot_of(const Authored& a) {
    return Dot{a.dev, a.frame.seq};
}

std::string printable(std::span<const std::byte> bytes) {
    std::string out;
    for (const std::byte b : bytes)
        out += std::to_string(std::to_integer<int>(b)) + ".";
    return out;
}

void dump_props(Store& store, std::span<const std::byte> owner, std::string& out) {
    for (const std::string_view key : {"x", "y"}) {
        Result<Statement> versions = store.statement(StatementId::PropVersions);
        REQUIRE(versions.has_value());
        versions->bind_blob(1, owner).bind_text(2, key);
        while (*versions->step()) {
            const Result<Value> value = store.load_value(versions->column_blob(3));
            REQUIRE(value.has_value());
            out += "  prop " + std::string(key) + " " + printable(versions->column_blob(0)) + " " +
                   std::to_string(versions->column_int(1)) + " " + encode_value(*value) + "\n";
        }
    }
}

void dump_edges_of(Store& store, const NodeId& node, std::string& out) {
    std::vector<EdgeId> edges;
    {
        Result<Statement> outgoing = store.statement(StatementId::EdgesOut);
        REQUIRE(outgoing.has_value());
        outgoing->bind_blob(1, node.bytes);
        while (*outgoing->step()) {
            EdgeId edge;
            std::ranges::copy(outgoing->column_blob(0), edge.bytes.begin());
            edges.push_back(edge);
            out += "  edge " + printable(edge.bytes) + " " + std::string(outgoing->column_text(1)) + " " +
                   printable(outgoing->column_blob(2)) + "\n";
        }
    }
    for (const EdgeId& edge : edges)
        dump_props(store, edge.bytes, out);
}

// Satırların tamamı (dots dahil): dots farkı bugün görünmese de sonraki silmenin kararını değiştirir.
std::string canonical_dump(Store& store) {
    std::string out;
    std::vector<NodeId> nodes;
    {
        Result<Statement> all = store.statement(StatementId::NodesAll);
        REQUIRE(all.has_value());
        while (*all->step()) {
            NodeId id;
            std::ranges::copy(all->column_blob(0), id.bytes.begin());
            nodes.push_back(id);
        }
    }
    for (const NodeId& id : nodes) {
        {
            Result<Statement> row = store.statement(StatementId::NodeGet);
            REQUIRE(row.has_value());
            row->bind_blob(1, id.bytes);
            REQUIRE(*row->step());
            out += "node " + printable(id.bytes) + " dots " + printable(row->column_blob(0)) + "\n";
        }
        Result<Statement> labels = store.statement(StatementId::LabelsOfNode);
        REQUIRE(labels.has_value());
        labels->bind_blob(1, id.bytes);
        while (*labels->step())
            out += "  label " + std::string(labels->column_text(0)) + "\n";
        dump_props(store, id.bytes, out);
        dump_edges_of(store, id, out);
    }
    return out;
}

void apply_batch(Store& store, const std::vector<const Authored*>& frames) {
    GRAPH_OK(store.begin_write());
    Result<Merger> merger = Merger::open(store);
    REQUIRE(merger.has_value());
    for (const Authored* a : frames) {
        const std::string payload = encode_frame(a->frame);
        GRAPH_OK(merger->apply_frame(a->dev, a->frame, as_bytes(payload)));
    }
    GRAPH_OK(merger->finish());
    GRAPH_OK(store.commit_write());
}

enum class Delivery { OneSession, SessionPerFrame };

std::string dump_after(const std::vector<Authored>& frames, const std::vector<std::size_t>& order,
                       Delivery delivery) {
    TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "graph.db");
    REQUIRE(opened.has_value());
    std::vector<const Authored*> sequence;
    for (const std::size_t index : order)
        sequence.push_back(&frames[index]);
    if (delivery == Delivery::OneSession) {
        apply_batch(**opened, sequence);
    } else {
        for (const Authored* a : sequence)
            apply_batch(**opened, {a});
    }
    GRAPH_OK((*opened)->begin_read());
    std::string dump = canonical_dump(**opened);
    (*opened)->end_txn();
    return dump;
}

bool sees(const Authored& reader, const Authored& written) {
    if (reader.dev == written.dev)
        return written.frame.seq <= reader.frame.seq;
    return std::ranges::any_of(reader.frame.deps, [&](const Dot& dep) {
        return dep.dev == written.dev && written.frame.seq <= dep.seq;
    });
}

bool is_causal(const std::vector<Authored>& frames, const std::vector<std::size_t>& order) {
    for (std::size_t early = 0; early < order.size(); ++early)
        for (std::size_t late = early + 1; late < order.size(); ++late)
            if (sees(frames[order[early]], frames[order[late]]))
                return false;
    return true;
}

// Tüm nedensel sıralar, iki teslim biçimiyle. Dönen dump her sırada aynıdır.
std::string converged_dump(const std::vector<Authored>& frames) {
    std::vector<std::size_t> order(frames.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::string expected;
    int schedules = 0;
    do {
        if (!is_causal(frames, order))
            continue;
        for (const Delivery delivery : {Delivery::OneSession, Delivery::SessionPerFrame}) {
            const std::string dump = dump_after(frames, order, delivery);
            if (schedules++ == 0)
                expected = dump;
            std::string shown;
            for (const std::size_t index : order)
                shown += std::to_string(index) + " ";
            INFO("order " << shown << " per_frame=" << (delivery == Delivery::SessionPerFrame));
            REQUIRE(dump == expected);
        }
    } while (std::ranges::next_permutation(order).found);
    REQUIRE(schedules >= 4);  // en az iki sıra: yoksa eşitlik hiçbir şey kanıtlamaz
    return expected;
}

bool has(const std::string& dump, std::string_view needle) {
    return dump.find(needle) != std::string::npos;
}

// A düğümü kurar; B onu görüp siler; A silmeyi görmeden `concurrent` yazar.
std::vector<Authored> delete_against(Op concurrent) {
    Authored create =
        authored(kA, 1, {}, {NodePut{kN}, LabelAdd{kN, "Keep"}, PropSet{kN, "y", std::int64_t{7}}});
    Authored remove = authored(kB, 1, {dot_of(create)}, {NodeDel{kN}});
    Authored write = authored(kA, 2, {}, {std::move(concurrent)});
    return {std::move(create), std::move(remove), std::move(write)};
}

}  // namespace

TEST_CASE("delete_vs_prop_set_converges_and_the_edit_wins") {
    const std::string dump = converged_dump(delete_against(PropSet{kN, "x", std::int64_t{1}}));
    CHECK(has(dump, "prop x"));
    CHECK(has(dump, "label Keep"));  // silme etkisizse önceki durum da kalır
    CHECK(has(dump, "prop y"));
}

TEST_CASE("delete_vs_label_add_converges_and_the_edit_wins") {
    const std::string dump = converged_dump(delete_against(LabelAdd{kN, "New"}));
    CHECK(has(dump, "label New"));
    CHECK(has(dump, "label Keep"));
    CHECK(has(dump, "prop y"));
}

TEST_CASE("delete_vs_label_del_converges_and_the_edit_wins") {
    const std::string dump = converged_dump(delete_against(LabelDel{kN, "Keep"}));
    CHECK(has(dump, "node "));
    CHECK_FALSE(has(dump, "label Keep"));
    CHECK(has(dump, "prop y"));
}

TEST_CASE("delete_vs_node_put_converges_and_the_put_wins") {
    const std::string dump = converged_dump(delete_against(NodePut{kN}));
    CHECK(has(dump, "node "));
    CHECK(has(dump, "label Keep"));
}

TEST_CASE("delete_that_saw_every_write_removes_the_node_in_any_order") {
    const Authored create = authored(kA, 1, {}, {NodePut{kN}, LabelAdd{kN, "L"}});
    const Authored edit = authored(kA, 2, {}, {PropSet{kN, "x", std::int64_t{1}}});
    const Authored remove = authored(kB, 1, {dot_of(edit)}, {NodeDel{kN}});
    const Authored other = authored(kC, 1, {}, {NodePut{kM}});
    const std::string dump = converged_dump({create, edit, remove, other});
    CHECK_FALSE(has(dump, "label L"));
    CHECK_FALSE(has(dump, "prop x"));
    CHECK(has(dump, "node "));  // kM kaldı: dump gerçekten dolu
}

TEST_CASE("two_partial_deletes_leave_the_node_alive_in_any_order") {
    const Authored a = authored(kA, 1, {}, {NodePut{kN}, PropSet{kN, "x", std::int64_t{1}}});
    const Authored b = authored(kB, 1, {}, {NodePut{kN}, PropSet{kN, "y", std::int64_t{2}}});
    const Authored only_a = authored(kC, 1, {dot_of(a)}, {NodeDel{kN}});
    const Authored only_b = authored(kD, 1, {dot_of(b)}, {NodeDel{kN}});
    const std::string dump = converged_dump({a, b, only_a, only_b});
    CHECK(has(dump, "prop x"));
    CHECK(has(dump, "prop y"));
}

// Satır varken de eski durum geri gelmelidir: yeniden kurma "satır yok" koşuluna bağlanamaz.
TEST_CASE("concurrent_write_after_a_recreate_restores_state_the_delete_had_removed") {
    const Authored create = authored(kA, 1, {}, {NodePut{kN}, LabelAdd{kN, "Old"}});
    const Authored remove = authored(kB, 1, {dot_of(create)}, {NodeDel{kN}});
    const Authored recreate = authored(kB, 2, {dot_of(create)}, {NodePut{kN}, LabelAdd{kN, "Fresh"}});
    const Authored concurrent = authored(kA, 2, {}, {PropSet{kN, "x", std::int64_t{1}}});
    const std::string dump = converged_dump({create, remove, recreate, concurrent});
    CHECK(has(dump, "label Old"));
    CHECK(has(dump, "label Fresh"));
    CHECK(has(dump, "prop x"));
}

TEST_CASE("edge_with_a_dead_endpoint_stays_out_and_returns_with_the_endpoint") {
    const Authored nodes = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}});
    const Authored remove = authored(kB, 1, {dot_of(nodes)}, {NodeDel{kM}});
    const Authored edge =
        authored(kA, 2, {}, {EdgePut{kE, kN, "REL", kM}, PropSet{kE, "x", std::int64_t{1}}});
    {
        const std::string dump = converged_dump({nodes, remove, edge});
        CHECK_FALSE(has(dump, "edge "));
        CHECK_FALSE(has(dump, "prop x"));
    }
    const Authored revive = authored(kC, 1, {dot_of(nodes)}, {PropSet{kM, "y", std::int64_t{2}}});
    const std::string dump = converged_dump({nodes, remove, edge, revive});
    CHECK(has(dump, "edge "));
    CHECK(has(dump, "prop x"));
    CHECK(has(dump, "prop y"));
}

TEST_CASE("edge_concurrent_with_an_effective_delete_stays_out_after_a_recreate") {
    const Authored nodes = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}});
    const Authored remove = authored(kB, 1, {dot_of(nodes)}, {NodeDel{kM}});
    const Authored recreate = authored(kB, 2, {dot_of(nodes)}, {NodePut{kM}});
    const Authored edge = authored(kC, 1, {dot_of(nodes)}, {EdgePut{kE, kN, "REL", kM}});
    CHECK_FALSE(has(converged_dump({nodes, remove, recreate, edge}), "edge "));
}

TEST_CASE("edge_that_saw_the_delete_survives_it") {
    const Authored nodes = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}});
    const Authored remove = authored(kB, 1, {dot_of(nodes)}, {NodeDel{kM}});
    const Authored recreate = authored(kB, 2, {dot_of(nodes)}, {NodePut{kM}});
    const Authored edge = authored(kB, 3, {dot_of(nodes)}, {EdgePut{kE, kN, "REL", kM}});
    const Authored unrelated = authored(kC, 1, {}, {NodePut{node_named("z")}});
    CHECK(has(converged_dump({nodes, remove, recreate, edge, unrelated}), "edge "));
}

TEST_CASE("three_devices_mixed_ops_converge_in_every_causal_order") {
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "REL", kM}});
    const Authored del_n = authored(kB, 1, {dot_of(base)}, {EdgeDel{kE}, NodeDel{kN}});
    const Authored edit_n = authored(kA, 2, {}, {PropSet{kN, "x", std::int64_t{1}}, LabelAdd{kN, "L"}});
    const Authored edit_edge = authored(kC, 1, {dot_of(base)}, {PropSet{kE, "y", std::int64_t{2}}});
    const Authored del_m = authored(kC, 2, {dot_of(base)}, {NodeDel{kM}});
    const std::string dump = converged_dump({base, del_n, edit_n, edit_edge, del_m});
    CHECK(has(dump, "prop x"));
}

// Silme, yazmanın bağımlılığını listelemiyor (geçişli olmayan deps): satırdaki dots'u görür ama eski yazmayı
// görmez. Yerinde karar da yeniden kurma da aynı olmalı. M üzerindeki silme ve düzenleme, sıraya göre yeniden
// kurmayı kimi çizelgede tetikler, kimisinde tetiklemez; iki yol ayrışırsa dump'lar eşit çıkmaz.
TEST_CASE("delete_with_non_transitive_deps_decides_the_same_on_both_paths") {
    const Authored first = authored(kA, 1, {}, {NodePut{kN}});
    const Authored second = authored(kB, 1, {dot_of(first)}, {PropSet{kN, "x", std::int64_t{1}}});
    const Authored blind = authored(kC, 1, {dot_of(second)}, {NodeDel{kN}});
    const Authored put_m = authored(kA, 2, {}, {NodePut{kM}});
    const Authored del_m = authored(kD, 1, {dot_of(put_m)}, {NodeDel{kM}});
    const Authored edit_m = authored(kA, 3, {}, {PropSet{kM, "y", std::int64_t{2}}});
    const std::string dump = converged_dump({first, second, blind, put_m, del_m, edit_m});
    CHECK(has(dump, "prop x"));  // silme ilk yazmayı görmedi: etkisizdir
    CHECK(has(dump, "prop y"));
}

TEST_CASE("edge_delete_with_non_transitive_deps_decides_the_same_on_both_paths") {
    const Authored first = authored(kA, 1, {}, {NodePut{kN}, EdgePut{kE, kN, "REL", kN}});
    const Authored second = authored(kB, 1, {dot_of(first)}, {PropSet{kE, "x", std::int64_t{1}}});
    const Authored blind = authored(kC, 1, {dot_of(second)}, {EdgeDel{kE}});
    const Authored put_m = authored(kA, 2, {}, {NodePut{kM}});
    const Authored del_m = authored(kD, 1, {dot_of(put_m)}, {NodeDel{kM}});
    const Authored edit_m = authored(kA, 3, {}, {PropSet{kM, "y", std::int64_t{2}}});
    const std::string dump = converged_dump({first, second, blind, put_m, del_m, edit_m});
    CHECK(has(dump, "edge "));  // silme put'u görmedi: etkisizdir
    CHECK(has(dump, "prop x"));
}

// Kenar put'u ve özelliği: silme tanındığında yerinde karar güvenilmez, iki yol da aynı sonuca varmalı.
TEST_CASE("edge_writes_concurrent_with_an_edge_delete_converge") {
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "REL", kM}});
    const Authored del = authored(kB, 1, {dot_of(base)}, {EdgeDel{kE}});
    const Authored reput = authored(kC, 1, {dot_of(base)}, {EdgePut{kE, kN, "REL", kM}});
    const Authored weight = authored(kD, 1, {dot_of(base)}, {PropSet{kE, "x", std::int64_t{1}}});
    const std::string dump = converged_dump({base, del, reput, weight});
    CHECK(has(dump, "edge "));  // add-wins
    CHECK(has(dump, "prop x"));
}

// F1 bulgusu: yeniden kurmada çerçevenin bir kenara ilk özellik yazması dışındakiler düşüyordu (adlı ilişkide
// `name` kalıp `directed` gidiyordu); yerinde uygulama hepsini tutar, sonuç teslim sırasına bağlıydı.
TEST_CASE("edge_props_written_in_one_frame_all_survive_a_concurrent_delete") {
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "REL", kM}});
    const Authored del = authored(kB, 1, {dot_of(base)}, {EdgeDel{kE}});
    const Authored edit = authored(kC, 1, {dot_of(base)},
                                   {PropSet{kE, "x", std::int64_t{1}}, PropSet{kE, "y", std::int64_t{2}}});
    const std::string dump = converged_dump({base, del, edit});
    CHECK(has(dump, "edge "));
    CHECK(has(dump, "prop x"));
    CHECK(has(dump, "prop y"));
}

// İnceleme bulgusu: aynı çerçevede put'tan önceki ve sonraki özellik yazmalarının kararı farklıdır. Yalnız
// ilk özellik kaydedilirse, put'tan önceki (düşen) yazma sonrakini de düşürür; yerinde uygulama tutar.
TEST_CASE("edge_props_before_and_after_a_put_in_one_frame_decide_alike_on_both_paths") {
    const NodeId k = node_named("k");
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, NodePut{k}});
    const Authored edit = authored(
        kC, 1, {dot_of(base)},
        {PropSet{kE, "y", std::int64_t{1}}, EdgePut{kE, kN, "REL", kM}, PropSet{kE, "x", std::int64_t{2}}});
    // İlgisiz eşzamanlı silme ve yazma: hangi sırayla gelirse gelsin yeniden kurma koşar.
    const Authored remove_k = authored(kB, 1, {dot_of(base)}, {NodeDel{k}});
    const Authored label_k = authored(kD, 1, {dot_of(base)}, {LabelAdd{k, "L"}});
    const std::string dump = converged_dump({base, edit, remove_k, label_k});
    CHECK(has(dump, "edge "));
    CHECK(has(dump, "prop x"));
    CHECK_FALSE(has(dump, "prop y"));  // kenar yokken yazıldı: iki yolda da düşer
}

// Çerçeve [özellik, put, özellik] ve eşzamanlı silme: ilk özellik ayaktaki put'u görmüştür, hepsi kalır.
TEST_CASE("edge_prop_put_prop_frame_concurrent_with_a_delete_converges_with_both_props") {
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "REL", kM}});
    const Authored del = authored(kB, 1, {dot_of(base)}, {EdgeDel{kE}});
    const Authored edit = authored(
        kC, 1, {dot_of(base)},
        {PropSet{kE, "x", std::int64_t{1}}, EdgePut{kE, kN, "REL", kM}, PropSet{kE, "y", std::int64_t{2}}});
    const std::string dump = converged_dump({base, del, edit});
    CHECK(has(dump, "edge "));
    CHECK(has(dump, "prop x"));
    CHECK(has(dump, "prop y"));
}

// Tek çerçevede silip yeniden kurma ve özellik (NoteStore böyle çerçeve üretmez: bir çağrı tek işlemdir ve
// yeni ilişki çiftteki kimlikleri atlar; motor yine de tutarlı karar vermeli). Eşzamanlı özellik yazması
// silmeyi etkisiz kılar.
TEST_CASE("edge_delete_put_prop_in_one_frame_keeps_the_new_prop_on_both_paths") {
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "REL", kM}});
    const Authored redo = authored(
        kC, 1, {dot_of(base)}, {EdgeDel{kE}, EdgePut{kE, kN, "REL", kM}, PropSet{kE, "x", std::int64_t{1}}});
    const Authored other = authored(kB, 1, {dot_of(base)}, {PropSet{kE, "y", std::int64_t{2}}});
    const std::string dump = converged_dump({base, redo, other});
    CHECK(has(dump, "edge "));
    CHECK(has(dump, "prop x"));
    CHECK(has(dump, "prop y"));
}

// İnceleme bulgusu: aynı çerçevede kur-sil-kur. Yeniden kurma ikinci put'u ilkinin tekrarı sayıp atıyordu
// (kenar ölü), yerinde uygulama tutuyordu (kenar ayakta): kalıcı ayrışma.
TEST_CASE("edge_put_delete_put_in_one_frame_is_alive_on_both_paths") {
    const NodeId k = node_named("k");
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, NodePut{k}});
    const Authored redo = authored(kC, 1, {dot_of(base)},
                                   {EdgePut{kE, kN, "REL", kM}, EdgeDel{kE}, EdgePut{kE, kN, "REL", kM},
                                    PropSet{kE, "x", std::int64_t{1}}});
    // İlgisiz eşzamanlı silme ve yazma: hangi sırayla gelirse gelsin yeniden kurma koşar.
    const Authored remove_k = authored(kB, 1, {dot_of(base)}, {NodeDel{k}});
    const Authored label_k = authored(kD, 1, {dot_of(base)}, {LabelAdd{k, "L"}});
    const std::string dump = converged_dump({base, redo, remove_k, label_k});
    CHECK(has(dump, "edge "));
    CHECK(has(dump, "prop x"));
}

TEST_CASE("edge_reput_concurrent_with_a_delete_keeps_the_state_the_delete_removed") {
    const Authored base = authored(
        kA, 1, {}, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "REL", kM}, PropSet{kE, "y", std::int64_t{7}}});
    const Authored del = authored(kB, 1, {dot_of(base)}, {EdgeDel{kE}});
    const Authored reput = authored(kC, 1, {dot_of(base)}, {EdgePut{kE, kN, "REL", kM}});
    const std::string dump = converged_dump({base, del, reput});
    CHECK(has(dump, "edge "));
    CHECK(has(dump, "prop y"));
}

TEST_CASE("edge_delete_without_seeing_every_write_has_no_effect") {
    TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "graph.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    GRAPH_OK(store.begin_write());
    GRAPH_OK(store.record(NodePut{kN}));
    GRAPH_OK(store.record(EdgePut{kE, kN, "REL", kN}));
    const auto edge_exists = [&] {
        Result<Statement> row = store.statement(StatementId::EdgeGet);
        REQUIRE(row.has_value());
        row->bind_blob(1, kE.bytes);
        return *row->step();
    };
    GRAPH_OK(store.apply(ApplyContext{Dot{kB, 1}, 1, CausalContext{}}, EdgeDel{kE}));
    CHECK(edge_exists());
    const Dot local{store.device(), 1};
    GRAPH_OK(store.apply(ApplyContext{Dot{kB, 2}, 2, CausalContext{{local}}}, EdgeDel{kE}));
    CHECK_FALSE(edge_exists());
    store.end_txn();
}

TEST_CASE("edge_put_that_did_not_see_its_endpoint_stays_out_on_both_paths") {
    const Authored node = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}});
    const Authored blind_edge = authored(kB, 1, {}, {EdgePut{kE, kN, "REL", kM}});
    const std::string dump = converged_dump({node, blind_edge});
    CHECK_FALSE(has(dump, "edge "));
}

// Çatışan işlem her cihazda aynı kuralla etkisizdir; çerçeveyi reddetmek cihazları ayrıştırırdı.
TEST_CASE("remote_edge_put_cannot_change_an_edge_it_has_seen") {
    TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "graph.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    const Authored nodes = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}, EdgePut{kE, kN, "REL", kM}});
    apply_batch(store, {&nodes});
    GRAPH_OK(store.begin_read());
    const std::string before = canonical_dump(store);
    store.end_txn();

    std::uint64_t seq = 0;
    for (const Op& hostile :
         {Op{EdgePut{kE, kM, "REL", kM}}, Op{EdgePut{kE, kN, "OTHER", kM}}, Op{EdgePut{kE, kN, "REL", kN}}}) {
        const Authored bad = authored(kB, ++seq, {dot_of(nodes)}, {hostile});
        const std::string payload = encode_frame(bad.frame);
        GRAPH_OK(store.begin_write());
        Result<Merger> merger = Merger::open(store);
        REQUIRE(merger.has_value());
        GRAPH_OK(merger->apply_frame(bad.dev, bad.frame, as_bytes(payload)));
        CHECK(merger->last_frame_had_identity_conflict());
        GRAPH_OK(merger->finish());
        GRAPH_OK(store.commit_write());
        GRAPH_OK(store.begin_read());
        CHECK(canonical_dump(store) == before);
        store.end_txn();
    }
}

TEST_CASE("store_apply_alone_still_refuses_a_changed_edge_identity") {
    TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "graph.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    GRAPH_OK(store.begin_write());
    GRAPH_OK(store.record(NodePut{kN}));
    GRAPH_OK(store.record(NodePut{kM}));
    GRAPH_OK(store.record(EdgePut{kE, kN, "REL", kM}));
    const ApplyContext context{Dot{kB, 1}, 1, CausalContext{}};
    const Result<void> applied = store.apply(context, EdgePut{kE, kM, "REL", kN});
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().code == Error::Code::BadFrame);
    store.end_txn();
}

// Yeniden kurma O(ops): dürüst ve sık görülen eşzamanlılık onu tetiklememeli.
TEST_CASE("honest_concurrency_without_deletes_never_rebuilds") {
    TempDir dir;
    Result<std::unique_ptr<Store>> opened = Store::open(dir.path() / "graph.db");
    REQUIRE(opened.has_value());
    Store& store = **opened;
    const Authored base = authored(kA, 1, {}, {NodePut{kN}, NodePut{kM}});
    const Authored edit = authored(kA, 2, {}, {PropSet{kN, "x", std::int64_t{1}}});
    const Authored other_edit = authored(kB, 1, {dot_of(base)}, {PropSet{kN, "x", std::int64_t{2}}});
    const Authored link = authored(kC, 1, {dot_of(base)}, {EdgePut{kE, kN, "REL", kM}});
    const Authored weight = authored(kC, 2, {dot_of(base)}, {PropSet{kE, "y", std::int64_t{3}}});
    GRAPH_OK(store.begin_write());
    Result<Merger> merger = Merger::open(store);
    REQUIRE(merger.has_value());
    for (const Authored* a : {&base, &edit, &other_edit, &link, &weight}) {
        const std::string payload = encode_frame(a->frame);
        GRAPH_OK(merger->apply_frame(a->dev, a->frame, as_bytes(payload)));
    }
    GRAPH_OK(merger->finish());
    GRAPH_OK(store.commit_write());
    CHECK(merger->rebuild_count() == 0);
}
