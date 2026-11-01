#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace noteapp::graph {

// Bayt bayt saklanır: NUL, CRLF, BOM ve geçersiz UTF-8 korunur (SPEC-graph kabul 3).
struct Bytes {
    std::string data;
    bool operator==(const Bytes&) const = default;
};

struct Timestamp {
    std::int64_t unix_ms = 0;
    bool operator==(const Timestamp&) const = default;
};

// Yazarken NaN, sonsuz ve geçersiz UTF-8 std::string reddedilir: NaN kendine eşit olmadığından yakınsama
// karşılaştırmasını, geçersiz metin de sorgu semantiğini bozardı. monostate null'dır; null yazmak özelliği
// kaldırır.
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, Timestamp>;

}  // namespace noteapp::graph
