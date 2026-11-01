#pragma once

#include "graph/cypher/ast.hpp"
#include "graph/error.hpp"

#include <cstddef>

namespace noteapp::graph::detail::cypher {

// Ham tampon alır: fuzz hedefi bu girişi kullanır, NUL'a güvenmez. Sınırlar: <= 64 KiB metin, <= 64 derinlik,
// <= 256 desen öğesi, <= 64 cümle. G/Ç yok, işlem yok.
Result<Query> parse_query(const char* data, std::size_t size);

}  // namespace noteapp::graph::detail::cypher
