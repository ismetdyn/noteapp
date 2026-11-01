#pragma once

#include "graph/query.hpp"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace noteapp::graph::detail::cypher {

struct Position {
    int line = 1;
    int col = 1;
};

enum class BinaryOp {
    And,
    Or,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    In,
    Contains,
    StartsWith,
    EndsWith,
    Add,
    Sub,
    Mul,
    Div,
    Mod
};

enum class Function { Labels, Type, Id };
enum class AggregateFn { CountStar, Count, Sum, Min, Max, Avg, Collect };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// Tek düğüm tipi, tür başına küçük alan kümesi: değerlendirme bu ağaçta yürür, ayrı bir derleme aşaması
// yoktur.
struct Expr {
    enum class Kind {
        Literal,
        Param,     // planlayıcı Literal'e çevirir
        Var,       // `slot` planlayıcıdan gelir
        Property,  // args[0].name, `slot` çözülmüş değişken, `key_id` önbellek anahtarı
        List,
        Binary,
        Not,
        Negate,
        IsNull,
        IsNotNull,
        Call,       // labels, type, id
        Aggregate,  // planlayıcı Var'a çevirir
        HasLabels   // planlayıcı üretir: `labels` kümesinin tamamı var mı
    };

    Kind kind = Kind::Literal;
    Position pos;
    int height = 1;  // ağaç yüksekliği: değerlendirme özyinelemesinin sınırı
    QueryValue literal;
    std::string name;  // Var, Param, Property anahtarı
    int slot = -1;
    int key_id = -1;
    BinaryOp op = BinaryOp::And;
    Function fn = Function::Labels;
    AggregateFn aggregate = AggregateFn::CountStar;
    bool distinct = false;
    std::vector<std::string> labels;
    std::vector<ExprPtr> args;
};

using PropertyMap = std::vector<std::pair<std::string, ExprPtr>>;

struct NodePattern {
    Position pos;
    int slot = -1;  // planlayıcı doldurur; adsız düğüm de gizli yuva alır
    std::string var;
    std::vector<std::string> labels;
    PropertyMap props;
};

enum class RelDirection { Right, Left, Both };

struct RelPattern {
    Position pos;
    int slot = -1;
    std::string var;
    std::vector<std::string> types;
    PropertyMap props;
    RelDirection direction = RelDirection::Both;
    bool var_length = false;
    int min_hops = 1;
    int max_hops = 1;
};

// rels[i], nodes[i] ile nodes[i + 1] arasındadır.
struct PathPattern {
    std::vector<NodePattern> nodes;
    std::vector<RelPattern> rels;
};

struct MatchClause {
    Position pos;
    bool optional = false;
    std::vector<PathPattern> paths;
    ExprPtr where;
};

struct CreateClause {
    Position pos;
    std::vector<PathPattern> paths;
};

struct MergeClause {
    Position pos;
    PathPattern path;
};

struct SetItem {
    Position pos;
    std::string var;
    std::string key;  // boşsa etiket işlemi
    ExprPtr value;    // REMOVE'da yok
    std::vector<std::string> labels;
};

struct SetClause {
    Position pos;
    bool remove = false;
    std::vector<SetItem> items;
};

struct DeleteClause {
    Position pos;
    bool detach = false;
    std::vector<ExprPtr> targets;
};

struct ProjectionItem {
    ExprPtr expr;
    std::string alias;
    bool explicit_alias = false;
};

struct OrderItem {
    ExprPtr expr;
    bool descending = false;
};

// WITH ve RETURN aynı şekildedir.
struct ProjectionClause {
    Position pos;
    bool is_return = false;
    bool distinct = false;
    bool star = false;
    std::vector<ProjectionItem> items;
    std::vector<OrderItem> order;
    ExprPtr skip;
    ExprPtr limit;
    ExprPtr where;
};

using Clause =
    std::variant<MatchClause, CreateClause, MergeClause, SetClause, DeleteClause, ProjectionClause>;

struct Query {
    std::vector<Clause> clauses;
};

}  // namespace noteapp::graph::detail::cypher
