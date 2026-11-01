#pragma once

#include "graph/cypher/ast.hpp"
#include "graph/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::graph::detail::cypher {

enum class TokenKind {
    End,
    Word,        // anahtar sözcük veya ad; `text` özgün yazım
    QuotedName,  // `ters tırnaklı ad`
    Integer,     // `text` rakamlar
    Float,
    String,  // `text` çözülmüş değer
    Param,   // `text` $ olmadan
    Symbol
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    Position pos;
    std::size_t begin = 0;  // kaynak metindeki bayt aralığı: sütun adı buradan alınır
    std::size_t end = 0;
};

// Dosya sonu belirteciyle biter. UTF-8'i geçersiz ad veya dizge Syntax'tir.
Result<std::vector<Token>> tokenize(std::string_view source);

}  // namespace noteapp::graph::detail::cypher
