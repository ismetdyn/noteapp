#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <span>
#include <string_view>

namespace noteapp::graph {

// Kimlikler cihazlar arası benzersiz 128 bittir (SPEC-graph ilke 6); depolamanın iç sayaçları dışarı çıkmaz.
struct NodeId {
    std::array<std::byte, 16> bytes{};
    auto operator<=>(const NodeId&) const = default;
};

struct EdgeId {
    std::array<std::byte, 16> bytes{};
    auto operator<=>(const EdgeId&) const = default;
};

// UUIDv7: zaman sıralı olduğundan dizin sayfalarına yakın yazılır. İşletim sisteminin rastgelesi alınamazsa
// süreç durur: tahmin edilebilir veya tekrarlanan kimlik sessiz veri birleşmesine yol açardı.
NodeId new_node_id();
EdgeId new_edge_id();

// UUIDv8: (ns, canonical) SHA-256'sının ilk 16 baytı. Aynı doğal anahtarı iki cihazda kuran işlem aynı
// kimliğe varır. Düğüm ve kenar uzayları ayrıdır: ikisi aynı özellik tablosunda sahip olabilir,
// çakışmamalıdır.
NodeId derive_node_id(std::string_view ns, std::span<const std::byte> canonical);
EdgeId derive_edge_id(std::string_view ns, std::span<const std::byte> canonical);

std::array<std::byte, 32> sha256(std::span<const std::byte> data);

}  // namespace noteapp::graph
