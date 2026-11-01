// rand_s bildirimi bu tanım stdlib.h'tan önce geldiğinde görünür (MinGW stdlib.h).
#define _CRT_RAND_S

#include "graph/id.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

namespace noteapp::graph {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::size_t kBlockBytes = 64;
constexpr std::size_t kLengthBytes = 8;
constexpr unsigned kVersionByte = 6;
constexpr unsigned kVariantByte = 8;
constexpr std::string_view kNodeSpace = "node";
constexpr std::string_view kEdgeSpace = "edge";

using HashState = std::array<std::uint32_t, 8>;
using Id = std::array<std::byte, 16>;

void compress(HashState& state, std::span<const std::byte, kBlockBytes> block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
        w[i] = (std::to_integer<std::uint32_t>(block[i * 4]) << 24U) |
               (std::to_integer<std::uint32_t>(block[i * 4 + 1]) << 16U) |
               (std::to_integer<std::uint32_t>(block[i * 4 + 2]) << 8U) |
               std::to_integer<std::uint32_t>(block[i * 4 + 3]);
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t big1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + big1 + choose + kRoundConstants[i] + w[i];
        const std::uint32_t big0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + big0 + majority;
    }
    const HashState mixed = {a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < state.size(); ++i)
        state[i] += mixed[i];
}

[[noreturn]] void random_source_failed() {
    std::fputs("noteapp_graph: operating system random source failed\n", stderr);
    std::abort();
}

void fill_random(std::span<std::byte> out) {
#ifdef _WIN32
    // rand_s: msvcrt zaten bağlı, yeni DLL gerekmez; işletim sisteminin kriptografik üretecini kullanır.
    for (std::size_t i = 0; i < out.size(); i += sizeof(unsigned int)) {
        unsigned int word = 0;
        if (rand_s(&word) != 0)
            random_source_failed();
        for (std::size_t j = 0; j < sizeof word && i + j < out.size(); ++j)
            out[i + j] = static_cast<std::byte>(word >> (j * 8U));
    }
#else
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t got = getrandom(out.data() + done, out.size() - done, 0);
        if (got > 0)
            done += static_cast<std::size_t>(got);
        else if (got < 0 && errno != EINTR)
            random_source_failed();
    }
#endif
}

void stamp_version(Id& id, unsigned version) {
    id[kVersionByte] =
        static_cast<std::byte>((std::to_integer<unsigned>(id[kVersionByte]) & 0x0FU) | (version << 4U));
    id[kVariantByte] = static_cast<std::byte>((std::to_integer<unsigned>(id[kVariantByte]) & 0x3FU) | 0x80U);
}

// RFC 9562: 48 bit unix ms, ardından rastgele bitler.
Id new_v7() {
    Id id{};
    fill_random(id);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto unix_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    for (std::size_t i = 0; i < 6; ++i)
        id[i] = static_cast<std::byte>(unix_ms >> ((5U - i) * 8U));
    stamp_version(id, 7);
    return id;
}

// Uzunluk öneki alanları ayırır: ("ab","c") ile ("a","bc") aynı kimliğe düşmez.
Id derive_v8(std::string_view space, std::string_view ns, std::span<const std::byte> canonical) {
    std::string input;
    input.reserve(space.size() + ns.size() + canonical.size() + kLengthBytes);
    input.append(space);
    const std::uint64_t ns_size = ns.size();
    for (std::size_t i = 0; i < kLengthBytes; ++i)
        input.push_back(static_cast<char>(ns_size >> (i * 8U)));
    input.append(ns);
    input.append(reinterpret_cast<const char*>(canonical.data()), canonical.size());

    const std::array<std::byte, 32> digest = sha256(std::as_bytes(std::span(input.data(), input.size())));
    Id id{};
    std::copy_n(digest.begin(), id.size(), id.begin());
    stamp_version(id, 8);
    return id;
}

}  // namespace

std::array<std::byte, 32> sha256(std::span<const std::byte> data) {
    HashState state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::size_t offset = 0;
    for (; data.size() - offset >= kBlockBytes; offset += kBlockBytes)
        compress(state, data.subspan(offset).first<kBlockBytes>());

    // Dolgu: 0x80, sıfırlar, sonda big-endian bit uzunluğu; gerekirse iki blok.
    std::array<std::byte, kBlockBytes * 2> tail{};
    const std::size_t rest = data.size() - offset;
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), rest, tail.begin());
    tail[rest] = std::byte{0x80};
    const std::size_t tail_size = rest + 1 + kLengthBytes <= kBlockBytes ? kBlockBytes : kBlockBytes * 2;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t i = 0; i < kLengthBytes; ++i)
        tail[tail_size - 1 - i] = static_cast<std::byte>(bit_length >> (i * 8U));
    for (std::size_t at = 0; at < tail_size; at += kBlockBytes)
        compress(state, std::span(tail).subspan(at).first<kBlockBytes>());

    std::array<std::byte, 32> digest{};
    for (std::size_t i = 0; i < state.size(); ++i)
        for (std::size_t j = 0; j < 4; ++j)
            digest[i * 4 + j] = static_cast<std::byte>(state[i] >> ((3U - j) * 8U));
    return digest;
}

NodeId new_node_id() {
    return NodeId{new_v7()};
}

EdgeId new_edge_id() {
    return EdgeId{new_v7()};
}

NodeId derive_node_id(std::string_view ns, std::span<const std::byte> canonical) {
    return NodeId{derive_v8(kNodeSpace, ns, canonical)};
}

EdgeId derive_edge_id(std::string_view ns, std::span<const std::byte> canonical) {
    return EdgeId{derive_v8(kEdgeSpace, ns, canonical)};
}

}  // namespace noteapp::graph
