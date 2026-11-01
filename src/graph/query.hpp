#pragma once

#include "graph/id.hpp"
#include "graph/value.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace noteapp::graph {

struct QueryValue;

// Liste yalnız sorgu sonucunda vardır (collect, labels, liste sabiti); saklanmaz.
struct QueryList {
    std::vector<QueryValue> items;
    friend bool operator==(const QueryList& left, const QueryList& right);
};

// Saklanan değerler (`Value`) ve yalnız sorguda olanlar: düğüm, kenar, liste.
struct QueryValue {
    using Variant = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, Timestamp,
                                 NodeId, EdgeId, QueryList>;

    QueryValue() = default;
    template <class T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, QueryValue> && std::is_constructible_v<Variant, T>)
    QueryValue(T&& value) : v(std::forward<T>(value)) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(v); }
    bool operator==(const QueryValue&) const = default;

    Variant v;
};

inline bool operator==(const QueryList& left, const QueryList& right) {
    return left.items == right.items;
}

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<QueryValue>> rows;
};

using Params = std::map<std::string, QueryValue, std::less<>>;

// ReadOnly'de yazma işleci plan aşamasında reddedilir, işlem açılmaz.
enum class QueryMode { ReadOnly, ReadWrite };

// Aşım kesilmiş sonuç değil LimitExceeded'dır (SPEC-graph "Sorgu dili").
struct QueryLimits {
    std::size_t max_rows = 100'000;                       // sonuç satırı
    std::size_t max_expansions = 1'000'000;               // gezilen kenar
    std::chrono::milliseconds max_duration{5000};         // sorgu başına süre
    std::size_t max_memory_bytes = 256U * 1024U * 1024U;  // sıralama, gruplama, sonuç gibi tutulan satırlar
    // Sorgu içi komşuluk ve özellik önbelleği. Dolunca yalnız yeni giriş eklenmez: sonuç aynı, sorgu
    // yavaşlar.
    std::size_t max_memo_bytes = 64U * 1024U * 1024U;
};

}  // namespace noteapp::graph
