#include "graph/id.hpp"
#include "graph_test_support.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <set>
#include <string>

using namespace noteapp::graph;
using test::as_bytes;

namespace {

std::string hex(std::span<const std::byte> data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : data) {
        out += kDigits[std::to_integer<unsigned>(b) >> 4U];
        out += kDigits[std::to_integer<unsigned>(b) & 0xFU];
    }
    return out;
}

std::string sha(std::string_view text) {
    return hex(sha256(as_bytes(text)));
}

unsigned version_of(const std::array<std::byte, 16>& id) {
    return std::to_integer<unsigned>(id[6]) >> 4U;
}

unsigned variant_of(const std::array<std::byte, 16>& id) {
    return std::to_integer<unsigned>(id[8]) >> 6U;
}

}  // namespace

// FIPS 180-4 / NIST örnek vektörleri; blok sınırı (55/56/64 bayt) dolgu hatasını yakalar.
TEST_CASE("sha256_matches_nist_vectors") {
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(sha(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(sha(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("new_ids_are_uuid_v7_with_current_time_and_unique") {
    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    std::set<NodeId> seen;
    for (int i = 0; i < 10000; ++i) {
        const NodeId id = new_node_id();
        CHECK(version_of(id.bytes) == 7);
        CHECK(variant_of(id.bytes) == 2);
        seen.insert(id);
    }
    CHECK(seen.size() == 10000);

    const NodeId id = new_node_id();
    std::int64_t unix_ms = 0;
    for (int i = 0; i < 6; ++i)
        unix_ms = (unix_ms << 8) | std::to_integer<std::int64_t>(id.bytes[static_cast<std::size_t>(i)]);
    CHECK(unix_ms >= before);
    CHECK(unix_ms <= before + 60000);

    const EdgeId edge = new_edge_id();
    CHECK(version_of(edge.bytes) == 7);
}

TEST_CASE("derived_ids_are_uuid_v8_deterministic_and_separated_by_namespace_and_kind") {
    const NodeId a = derive_node_id("conflict", as_bytes("x"));
    CHECK(version_of(a.bytes) == 8);
    CHECK(variant_of(a.bytes) == 2);
    CHECK(a == derive_node_id("conflict", as_bytes("x")));
    CHECK_FALSE(a == derive_node_id("conflict", as_bytes("y")));
    CHECK_FALSE(a == derive_node_id("conflic", as_bytes("tx")));

    const EdgeId edge = derive_edge_id("conflict", as_bytes("x"));
    CHECK(version_of(edge.bytes) == 8);
    CHECK_FALSE(edge.bytes == a.bytes);
}
