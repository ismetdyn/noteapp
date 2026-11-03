#include "graph/op.hpp"
#include "graph_test_support.hpp"
#include "support/test_support.hpp"
#include "sync_test_support.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <vector>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;
using test::as_bytes;
using test::edge_named;
using test::node_named;

namespace {

// 0: kapalı. n: n'inci ayırma bad_alloc fırlatır (yalnız tek ayırma: hata nesnesi kurulabilsin).
std::size_t allocation_countdown = 0;
bool allocation_failure_fired = false;

}  // namespace

namespace noteapp::graph::test {
// cypher_security_test.cpp de kullanır.
void fail_allocation_after(std::size_t count) {
    allocation_countdown = count;
}
}  // namespace noteapp::graph::test

// noinline: aşağıdaki delete ile aynı gerekçe (-Wmismatched-new-delete).
[[gnu::noinline]] void* operator new(std::size_t size) {
    if (allocation_countdown > 0 && --allocation_countdown == 0) {
        allocation_failure_fired = true;
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}
// std::stable_sort'un geçici tamponu bu biçimi kullanır: değiştirilmezse sanitizer'ın new'u bizim delete'e
// düşer.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}
// noinline: gövde çağırana açılırsa GCC free ile new eşleşmesini sanır (-Wmismatched-new-delete); burada
// eştirler.
[[gnu::noinline]] void operator delete(void* memory) noexcept {
    std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

Frame sample_frame() {
    Frame frame;
    frame.seq = 7;
    frame.hlc = 0x0123456789ABCDEFULL;
    frame.deps.push_back(Dot{DeviceId{std::byte{1}}, 3});
    frame.ops.emplace_back(NodePut{node_named("a")});
    frame.ops.emplace_back(LabelAdd{node_named("a"), "Thing"});
    frame.ops.emplace_back(LabelDel{node_named("a"), "Old"});
    frame.ops.emplace_back(EdgePut{edge_named("e"), node_named("a"), "REL", node_named("b")});
    frame.ops.emplace_back(PropSet{node_named("a"), "n", std::int64_t{-5}});
    frame.ops.emplace_back(PropSet{edge_named("e"), "w", 1.5});
    frame.ops.emplace_back(PropSet{node_named("a"), "gone", std::monostate{}});
    frame.ops.emplace_back(PropSet{node_named("a"), "raw", Bytes{std::string("\0\xFF\r\n", 4)}});
    frame.ops.emplace_back(PropSet{node_named("a"), "at", Timestamp{-1}});
    frame.ops.emplace_back(PropSet{node_named("a"), "yes", true});
    frame.ops.emplace_back(PropSet{node_named("a"), "s", std::string("metin")});
    frame.ops.emplace_back(EdgeDel{edge_named("e")});
    frame.ops.emplace_back(NodeDel{node_named("a")});
    return frame;
}

}  // namespace

TEST_CASE("frame_roundtrip_keeps_every_op_kind") {
    const Frame frame = sample_frame();
    const std::string encoded = encode_frame(frame);
    const Result<Frame> decoded = decode_frame(as_bytes(encoded));
    REQUIRE(decoded.has_value());
    CHECK(decoded->seq == frame.seq);
    CHECK(decoded->hlc == frame.hlc);
    CHECK(decoded->deps == frame.deps);
    CHECK(decoded->ops == frame.ops);
}

TEST_CASE("decode_frame_rejects_every_truncation_without_crash") {
    const std::string encoded = encode_frame(sample_frame());
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        const Result<Frame> decoded = decode_frame(as_bytes(std::string_view(encoded).substr(0, length)));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::BadFrame);
    }
}

TEST_CASE("decode_frame_rejects_trailing_bytes_and_every_single_byte_corruption_is_safe") {
    std::string encoded = encode_frame(sample_frame());
    const Result<Frame> trailing = decode_frame(as_bytes(encoded + 'x'));
    REQUIRE_FALSE(trailing.has_value());
    CHECK(trailing.error().code == Error::Code::BadFrame);

    // İçerik değişimi çoğu zaman geçerli başka çerçevedir; zarfın özeti yakalar. Sonuç iki sınıftan biridir:
    // BadFrame ya da aynı baytlara geri kodlanan tutarlı çerçeve. Tutarsız değer üretilemez.
    int rejected = 0;
    int accepted = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char saved = encoded[i];
        encoded[i] = static_cast<char>(saved ^ 0xFF);
        const Result<Frame> decoded = decode_frame(as_bytes(encoded));
        INFO("flipped byte " << i);
        if (decoded) {
            ++accepted;
            CHECK(encode_frame(*decoded) == encoded);
        } else {
            ++rejected;
            CHECK(decoded.error().code == Error::Code::BadFrame);
        }
        encoded[i] = saved;
    }
    // İki sınıf da gerçekten görülmeli: biri boşsa yukarıdaki dallar hiçbir şey kanıtlamaz.
    CHECK(rejected > 0);
    CHECK(accepted > 0);
}

TEST_CASE("frame_op_count_is_capped_on_decode") {
    Frame frame;
    frame.seq = 1;
    frame.ops.assign(kDefaultFrameOps, NodePut{node_named("a")});
    CHECK(decode_frame(as_bytes(encode_frame(frame))).has_value());

    frame.ops.emplace_back(NodePut{node_named("a")});
    const Result<Frame> over = decode_frame(as_bytes(encode_frame(frame)));
    REQUIRE_FALSE(over.has_value());
    CHECK(over.error().code == Error::Code::BadFrame);
}

// L4b: çerçeve başına işlem sınırı yerel commit'i sınırlamaz; export çerçeveyi kayıtlara böler, okuyan kayıt
// başına aynı sınırı uygular (multipart_test).
TEST_CASE("local_commit_over_the_op_cap_commits") {
    noteapp::test::TempDir dir;
    Graph graph = test::open_graph(dir.path() / "graph.db");
    const NodeId a = node_named("a");
    {
        WriteTxn txn = test::begin_write(graph);
        for (std::size_t i = 0; i <= kDefaultFrameOps; ++i)
            GRAPH_OK(txn.put_node(a));
        GRAPH_OK(txn.commit());
    }
    ReadTxn read = test::begin_read(graph);
    CHECK(*read.has_node(a));
}

// Bellek biterse çözücü istisna sızdırmaz: API istisnasızdır ve girdi uzaktan gelir.
TEST_CASE("decoding_survives_allocation_failure_at_every_point") {
    const std::string encoded = encode_frame(sample_frame());
    int failures = 0;
    for (std::size_t nth = 1;; ++nth) {
        allocation_failure_fired = false;
        allocation_countdown = nth;
        const Result<Frame> decoded = decode_frame(as_bytes(encoded));
        allocation_countdown = 0;
        if (!allocation_failure_fired) {
            CHECK(decoded.has_value());
            break;
        }
        ++failures;
        INFO("failing allocation " << nth);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == Error::Code::LimitExceeded);
    }
    // deps ve ops ayırmaları; ops sayısı doğrulandıktan sonra tek seferde ayrılır (L4b2). Yoksa sayaç hiçbir
    // ayırmaya denk gelmemiştir.
    CHECK(failures >= 2);
}

// Bulgu 5: ayırma her noktada düşebilir; API sınırı istisna sızdırmaz, süreç ölmez, sonraki ingest tamamlar.
TEST_CASE("ingest_and_export_survive_allocation_failure_at_every_point") {
    test::Device source;
    source.commit([](WriteTxn& txn) {
        GRAPH_OK(txn.put_node(node_named("a")));
        GRAPH_OK(txn.add_label(node_named("a"), "L"));
        GRAPH_OK(txn.set_property(node_named("a"), "x", std::string("value")));
    });
    const auto log = source.exported();
    const std::vector<std::filesystem::path> logs{log};
    test::Device target;

    const auto sweep = [](const auto& call) {
        int failures = 0;
        for (std::size_t nth = 1; nth < 20000; ++nth) {
            allocation_failure_fired = false;
            allocation_countdown = nth;
            const auto result = call();
            allocation_countdown = 0;
            if (!allocation_failure_fired)
                break;
            ++failures;
            INFO("failing allocation " << nth);
            if (!result)
                CHECK(result.error().code == Error::Code::LimitExceeded);
        }
        return failures;
    };
    CHECK(sweep([&] { return target.graph().ingest_logs(logs); }) > 5);
    CHECK(target.ingest(logs).stalled.empty());
    CHECK(target.dump() == source.dump());

    source.commit([](WriteTxn& txn) { GRAPH_OK(txn.add_label(node_named("a"), "M")); });
    CHECK(sweep([&] { return source.graph().export_log(source.outbox()); }) > 5);
    test::Device fresh;
    GRAPH_OK(source.graph().export_log(source.outbox()));
    CHECK(fresh.ingest(logs).bad_frames.empty());
    CHECK(fresh.dump() == source.dump());
}

TEST_CASE("decode_frame_refuses_hostile_counts_and_names") {
    Frame frame;
    frame.seq = 1;
    frame.ops.emplace_back(LabelAdd{node_named("a"), "Label"});
    const std::string good = encode_frame(frame);
    REQUIRE(decode_frame(as_bytes(good)).has_value());

    SUBCASE("zero_seq") {
        Frame zero = frame;
        zero.seq = 0;
        CHECK_FALSE(decode_frame(as_bytes(encode_frame(zero))).has_value());
    }
    SUBCASE("no_ops") {
        Frame empty = frame;
        empty.ops.clear();
        CHECK_FALSE(decode_frame(as_bytes(encode_frame(empty))).has_value());
    }
    SUBCASE("too_many_deps") {
        Frame many = frame;
        many.deps.assign(kDefaultDeps + 1, Dot{});
        CHECK_FALSE(decode_frame(as_bytes(encode_frame(many))).has_value());
    }
    SUBCASE("invalid_utf8_label") {
        Frame bad = frame;
        bad.ops[0] = LabelAdd{node_named("a"), "\xC0\xAF"};
        CHECK_FALSE(decode_frame(as_bytes(encode_frame(bad))).has_value());
    }
    SUBCASE("oversized_name") {
        Frame bad = frame;
        bad.ops[0] = LabelAdd{node_named("a"), std::string(kMaxNameBytes + 1, 'x')};
        CHECK_FALSE(decode_frame(as_bytes(encode_frame(bad))).has_value());
    }
    SUBCASE("nan_value") {
        Frame bad = frame;
        bad.ops[0] = PropSet{node_named("a"), "k", std::numeric_limits<double>::quiet_NaN()};
        CHECK_FALSE(decode_frame(as_bytes(encode_frame(bad))).has_value());
    }
    SUBCASE("invalid_utf8_string_value") {
        Frame bad = frame;
        bad.ops[0] = PropSet{node_named("a"), "k", std::string("\xFF")};
        CHECK_FALSE(decode_frame(as_bytes(encode_frame(bad))).has_value());
    }
}

TEST_CASE("value_codec_keeps_exact_bits_and_rejects_unknown_or_large_reference_tags") {
    const double negative_zero = -0.0;
    const Result<Value> decoded = decode_value(as_bytes(encode_value(negative_zero)));
    REQUIRE(decoded.has_value());
    CHECK(std::signbit(std::get<double>(*decoded)));

    const char unknown[] = {static_cast<char>(0x7F)};
    CHECK_FALSE(decode_value(as_bytes(std::string_view(unknown, 1))).has_value());
    const char large_reference[] = {static_cast<char>(0x85), 0, 0};
    CHECK_FALSE(decode_value(as_bytes(std::string_view(large_reference, 3))).has_value());
    CHECK_FALSE(decode_value({}).has_value());
    const char short_int[] = {2, 1};
    CHECK_FALSE(decode_value(as_bytes(std::string_view(short_int, 2))).has_value());
    const char long_bool[] = {1, 1, 1};
    CHECK_FALSE(decode_value(as_bytes(std::string_view(long_bool, 3))).has_value());
    const char bad_bool[] = {1, 2};
    CHECK_FALSE(decode_value(as_bytes(std::string_view(bad_bool, 2))).has_value());
}

TEST_CASE("utf8_validation_rejects_overlong_surrogate_and_out_of_range") {
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("ascii"));
    CHECK(is_valid_utf8("İşğü€\xF0\x9F\x98\x80"));
    CHECK(is_valid_utf8("\xF4\x8F\xBF\xBF"));
    CHECK_FALSE(is_valid_utf8("\xC0\xAF"));          // aşırı uzun '/'
    CHECK_FALSE(is_valid_utf8("\xE0\x80\xAF"));      // aşırı uzun
    CHECK_FALSE(is_valid_utf8("\xF0\x80\x80\xAF"));  // aşırı uzun
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));      // vekil
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // U+10FFFF üstü
    CHECK_FALSE(is_valid_utf8("\xE2\x82"));          // yarım dizi
    CHECK_FALSE(is_valid_utf8("\x80"));              // başsız devam
    CHECK_FALSE(is_valid_utf8("\xFF"));
}

TEST_CASE("names_must_be_nonempty_bounded_utf8_without_nul") {
    CHECK(validate_name("Tür").has_value());
    CHECK(validate_name(std::string(kMaxNameBytes, 'x')).has_value());
    CHECK_FALSE(validate_name("").has_value());
    CHECK_FALSE(validate_name(std::string(kMaxNameBytes + 1, 'x')).has_value());
    CHECK_FALSE(validate_name("\xFF").has_value());
    CHECK_FALSE(validate_name(std::string_view("a\0b", 3)).has_value());
}

TEST_CASE("dots_roundtrip_and_reject_bad_length") {
    const std::vector<Dot> dots = {Dot{DeviceId{std::byte{1}}, 2}, Dot{DeviceId{std::byte{3}}, 4}};
    const std::string encoded = encode_dots(dots);
    const Result<std::vector<Dot>> decoded = decode_dots(as_bytes(encoded));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == dots);
    CHECK_FALSE(decode_dots(as_bytes(std::string_view(encoded).substr(0, encoded.size() - 1))).has_value());
}
