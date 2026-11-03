#pragma once

#include "graph/cypher/ast.hpp"
#include "graph/error.hpp"

#include <cstddef>

namespace noteapp::graph::detail::cypher {

// Ham tampon alır: fuzz hedefi bu girişi kullanır, NUL'a güvenmez. Sınırlar `limits`'tendir (metin, iç
// içelik, desen öğesi, cümle, `*1..N`); varsayılanı fuzz ve testler içindir. G/Ç yok, işlem yok.
Result<Query> parse_query(const char* data, std::size_t size, const QueryLimits& limits = {});

}  // namespace noteapp::graph::detail::cypher
