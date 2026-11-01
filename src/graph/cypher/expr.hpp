#pragma once

// İfade değerlendirme ve değer kuralları (SPEC-graph "Sorgu dili"): üç değerli null, tam sayısal
// karşılaştırma, taşma denetimli aritmetik. Metin karşılaştırması baytlıktır; Türkçe katlama arama
// katmanındadır.
#include "graph/cypher/ast.hpp"
#include "graph/cypher/runtime.hpp"
#include "graph/error.hpp"
#include "graph/query.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace noteapp::graph::detail::cypher {

inline constexpr int kMaxValueDepth = 64;

Result<QueryValue> eval(const Expr& expr, const Row& row, ExecContext& context);
// WHERE kuralı: yalnız `true` geçer; null ve tip uyuşmazlığı geçmez.
Result<bool> eval_predicate(const Expr& expr, const Row& row, ExecContext& context);
// SKIP ve LIMIT sayısı: sabit ifadedir, negatif olmayan tam sayı olmalıdır.
Result<std::size_t> eval_count(const Expr& expr, ExecContext& context);

// ORDER BY'ın toplam sırası (openCypher): artan yönde null en sonda, karışık tipler sabit sırada.
int compare_for_order(const QueryValue& left, const QueryValue& right);
// `=` semantiği: null belirsizdir; farklı tipler eşit değildir.
std::optional<bool> values_equal(const QueryValue& left, const QueryValue& right);

// DISTINCT, gruplama ve MERGE kimliğinin kanonik baytları. `=` ile eşit değerler (1 ve 1.0) aynı baytları
// verir; null null'a eşittir.
void append_canonical(std::string& out, const QueryValue& value);

QueryValue from_stored(Value stored);
Result<Value> to_stored(const QueryValue& value, Position pos);

std::size_t value_bytes(const QueryValue& value);
std::size_t row_bytes(const Row& row);
int value_depth(const QueryValue& value);

std::string id_to_hex(std::span<const std::byte> id);
std::optional<std::array<std::byte, 16>> id_from_hex(std::string_view text);

// Kenar kimliğinden tip; kenar yoksa boş. Gezinme API'sinde karşılığı olmayan tek okuma.
Result<std::optional<std::string>> read_edge_type(ExecContext& context, const EdgeId& id);

}  // namespace noteapp::graph::detail::cypher
