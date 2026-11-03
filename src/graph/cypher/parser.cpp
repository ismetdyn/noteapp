#include "graph/cypher/parser.hpp"

#include "graph/cypher/lexer.hpp"
#include "graph/cypher/runtime.hpp"
#include "graph/op.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace noteapp::graph::detail::cypher {

namespace {

// Değişken adı olamayan sözcükler. Etiket, tip ve özellik anahtarı olabilirler: `n.count`, `:Order`.
constexpr std::array<std::string_view, 31> kReserved = {
    "MATCH",    "OPTIONAL", "WHERE",    "RETURN", "WITH", "CREATE", "MERGE", "SET",
    "REMOVE",   "DELETE",   "DETACH",   "ORDER",  "BY",   "SKIP",   "LIMIT", "AS",
    "DISTINCT", "AND",      "OR",       "XOR",    "NOT",  "IN",     "IS",    "NULL",
    "TRUE",     "FALSE",    "CONTAINS", "STARTS", "ENDS", "ON",     "CASE"};

// Cümle başında tanınır, konumlu reddedilir (SPEC-graph "Yok").
constexpr std::array<std::string_view, 9> kUnsupportedClauses = {
    "LOAD", "CALL", "UNWIND", "FOREACH", "UNION", "DROP", "START", "USE", "ALTER"};

bool same_word(std::string_view word, std::string_view upper) {
    return std::ranges::equal(word, upper, [](char a, char b) {
        return std::toupper(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
    });
}

template <std::size_t N> bool in_words(std::string_view word, const std::array<std::string_view, N>& words) {
    return std::ranges::any_of(words, [&](std::string_view candidate) { return same_word(word, candidate); });
}

// İlk hata saklanır ve okuma dosya sonuna atlar: tüm döngüler kendiliğinden biter, her çağrıda hata taşınmaz.
// Bu sayede ifade fonksiyonları null dönmez; hata varken kurulan ağaç atılır.
class Parser {
public:
    Parser(std::vector<Token> tokens, std::string_view source, const QueryLimits& limits)
        : tokens_(std::move(tokens)), source_(source), limits_(limits) {}

    Result<Query> run() {
        Query query = parse_clauses();
        check_query_shape(query);
        if (error_)
            return std::unexpected(std::move(*error_));
        return query;
    }

private:
    // --- Belirteç erişimi ---

    const Token& peek(std::size_t offset = 0) const {
        const std::size_t at = at_ + offset;
        return at < tokens_.size() ? tokens_[at] : tokens_.back();
    }
    bool at_end() const { return peek().kind == TokenKind::End; }
    bool at_symbol(std::string_view symbol, std::size_t offset = 0) const {
        return peek(offset).kind == TokenKind::Symbol && peek(offset).text == symbol;
    }
    bool at_word(std::string_view upper, std::size_t offset = 0) const {
        return peek(offset).kind == TokenKind::Word && same_word(peek(offset).text, upper);
    }
    bool accept_symbol(std::string_view symbol) {
        if (!at_symbol(symbol))
            return false;
        ++at_;
        return true;
    }
    bool accept_word(std::string_view upper) {
        if (!at_word(upper))
            return false;
        ++at_;
        return true;
    }

    void fail(Error::Code code, std::string key, Position pos,
              std::vector<std::pair<std::string, std::string>> args) {
        if (!error_)
            error_ = located_error(code, std::move(key), pos, std::move(args));
        at_ = tokens_.size() - 1;
    }
    void syntax(std::string what, Position pos) {
        fail(Error::Code::Syntax, "graph.cypher_syntax", pos, {{"what", std::move(what)}});
    }
    void syntax_here(std::string what) { syntax(std::move(what), peek().pos); }
    void unsupported(std::string what, Position pos) {
        fail(Error::Code::Unsupported, "graph.cypher_unsupported", pos, {{"what", std::move(what)}});
    }
    void limit(std::string what, Position pos) {
        fail(Error::Code::LimitExceeded, "graph.limit_exceeded", pos, {{"limit", std::move(what)}});
    }
    void expect_symbol(std::string_view symbol) {
        if (!accept_symbol(symbol))
            syntax_here("expected '" + std::string(symbol) + "'");
    }
    void expect_word(std::string_view upper) {
        if (!accept_word(upper))
            syntax_here("expected " + std::string(upper));
    }

    // --- Adlar ---

    bool at_name() const { return peek().kind == TokenKind::Word || peek().kind == TokenKind::QuotedName; }

    // Etiket, tip, özellik anahtarı: ayrılmış sözcük de olur, uzunluk depolamanın sınırındadır.
    std::string expect_label_name(std::string_view what) {
        if (!at_name()) {
            syntax_here("expected " + std::string(what));
            return "";
        }
        std::string name = peek().text;
        if (name.size() > kMaxNameBytes)
            limit("name_bytes", peek().pos);
        ++at_;
        return name;
    }

    bool at_variable() const {
        return peek().kind == TokenKind::QuotedName ||
               (peek().kind == TokenKind::Word && !in_words(peek().text, kReserved));
    }

    std::string expect_variable() {
        if (!at_variable()) {
            syntax_here("expected variable");
            return "";
        }
        std::string name = peek().text;
        if (name.size() > kMaxNameBytes)
            limit("name_bytes", peek().pos);
        ++at_;
        return name;
    }

    // --- Cümleler ---

    Query parse_clauses() {
        Query query;
        while (!at_end() && !at_symbol(";")) {
            if (query.clauses.size() >= limits_.max_clauses) {
                limit("clauses", peek().pos);
                break;
            }
            query.clauses.push_back(parse_clause());
        }
        accept_symbol(";");
        if (!at_end())
            syntax_here("expected end of query");
        return query;
    }

    Clause parse_clause() {
        const Position pos = peek().pos;
        if (peek().kind == TokenKind::Word && in_words(peek().text, kUnsupportedClauses)) {
            unsupported(peek().text, pos);
            return MatchClause{};
        }
        if (accept_word("MATCH"))
            return parse_match(pos, false);
        if (accept_word("OPTIONAL")) {
            expect_word("MATCH");
            return parse_match(pos, true);
        }
        return parse_writing_or_projection(pos);
    }

    Clause parse_writing_or_projection(Position pos) {
        if (accept_word("CREATE"))
            return parse_create(pos);
        if (accept_word("MERGE"))
            return parse_merge(pos);
        if (accept_word("SET"))
            return parse_set(pos);
        if (accept_word("REMOVE"))
            return parse_remove(pos);
        if (accept_word("DELETE"))
            return parse_delete(pos, false);
        if (accept_word("DETACH")) {
            expect_word("DELETE");
            return parse_delete(pos, true);
        }
        if (accept_word("WITH"))
            return parse_projection(pos, false);
        if (accept_word("RETURN"))
            return parse_projection(pos, true);
        syntax_here("expected clause");
        return MatchClause{};
    }

    // Son cümle sonuç veya yazma olmalı; RETURN yalnız sonda.
    void check_query_shape(const Query& query) {
        if (error_)
            return;
        if (query.clauses.empty()) {
            syntax_here("empty query");
            return;
        }
        for (std::size_t i = 0; i < query.clauses.size(); ++i) {
            const auto* projection = std::get_if<ProjectionClause>(&query.clauses[i]);
            if (projection != nullptr && projection->is_return && i + 1 != query.clauses.size())
                return syntax("RETURN must be the last clause", clause_position(query.clauses[i + 1]));
        }
        const Clause& last = query.clauses.back();
        const auto* projection = std::get_if<ProjectionClause>(&last);
        if (std::holds_alternative<MatchClause>(last) || (projection != nullptr && !projection->is_return))
            syntax("query cannot end here; expected RETURN or a writing clause", clause_position(last));
    }

    static Position clause_position(const Clause& clause) {
        return std::visit([](const auto& value) { return value.pos; }, clause);
    }

    Clause parse_match(Position pos, bool optional) {
        MatchClause clause;
        clause.pos = pos;
        clause.optional = optional;
        clause.paths = parse_patterns();
        if (at_word("USING"))
            unsupported("USING", peek().pos);
        if (accept_word("WHERE"))
            clause.where = parse_expression();
        return clause;
    }

    Clause parse_create(Position pos) {
        // CREATE INDEX, CONSTRAINT: kullanıcı tanımlı dizin yok (SPEC-graph "Yok").
        if (at_word("INDEX") || at_word("CONSTRAINT") || at_word("UNIQUE")) {
            unsupported(peek().text, peek().pos);
            return MatchClause{};
        }
        CreateClause clause;
        clause.pos = pos;
        clause.paths = parse_patterns();
        return clause;
    }

    Clause parse_merge(Position pos) {
        MergeClause clause;
        clause.pos = pos;
        clause.path = parse_path();
        if (at_word("ON"))
            unsupported("MERGE ON", peek().pos);
        return clause;
    }

    std::vector<std::string> parse_label_chain() {
        std::vector<std::string> labels;
        while (accept_symbol(":"))
            labels.push_back(expect_label_name("label"));
        return labels;
    }

    SetItem parse_set_item() {
        SetItem item;
        item.pos = peek().pos;
        item.var = expect_variable();
        if (at_symbol(":")) {
            item.labels = parse_label_chain();
            return item;
        }
        // n = {...} ve n += {...} desteklenmez: özellik kümesini toplu değiştirir.
        if (at_symbol("=") || at_symbol("+")) {
            unsupported("SET map", peek().pos);
            return item;
        }
        expect_symbol(".");
        item.key = expect_label_name("property key");
        expect_symbol("=");
        item.value = parse_expression();
        return item;
    }

    Clause parse_set(Position pos) {
        SetClause clause;
        clause.pos = pos;
        do {
            clause.items.push_back(parse_set_item());
        } while (accept_symbol(","));
        return clause;
    }

    SetItem parse_remove_item() {
        SetItem item;
        item.pos = peek().pos;
        item.var = expect_variable();
        if (at_symbol(":")) {
            item.labels = parse_label_chain();
            return item;
        }
        expect_symbol(".");
        item.key = expect_label_name("property key");
        return item;
    }

    Clause parse_remove(Position pos) {
        SetClause clause;
        clause.pos = pos;
        clause.remove = true;
        do {
            clause.items.push_back(parse_remove_item());
        } while (accept_symbol(","));
        return clause;
    }

    Clause parse_delete(Position pos, bool detach) {
        DeleteClause clause;
        clause.pos = pos;
        clause.detach = detach;
        do {
            clause.targets.push_back(parse_expression());
        } while (accept_symbol(","));
        return clause;
    }

    Clause parse_projection(Position pos, bool is_return) {
        ProjectionClause clause;
        clause.pos = pos;
        clause.is_return = is_return;
        clause.distinct = accept_word("DISTINCT");
        parse_projection_items(clause);
        parse_order_by(clause);
        if (accept_word("SKIP"))
            clause.skip = parse_expression();
        if (accept_word("LIMIT"))
            clause.limit = parse_expression();
        if (!is_return && accept_word("WHERE"))
            clause.where = parse_expression();
        return clause;
    }

    void parse_projection_items(ProjectionClause& clause) {
        if (accept_symbol("*")) {
            clause.star = true;
            if (!at_symbol(","))
                return;
            ++at_;
        }
        do {
            clause.items.push_back(parse_projection_item());
        } while (accept_symbol(","));
    }

    ProjectionItem parse_projection_item() {
        ProjectionItem item;
        const std::size_t begin = peek().begin;
        item.expr = parse_expression();
        // Hata sonrası at_ dosya sonunu geçmiş olabilir; ad o durumda kullanılmaz.
        const std::size_t last = std::min(at_, tokens_.size()) - (at_ == 0 ? 0 : 1);
        if (accept_word("AS")) {
            item.alias = expect_variable();
            item.explicit_alias = true;
        } else {
            const std::size_t end = tokens_[last].end;
            item.alias = std::string(source_.substr(begin, end > begin ? end - begin : 0));
        }
        return item;
    }

    void parse_order_by(ProjectionClause& clause) {
        if (!accept_word("ORDER"))
            return;
        expect_word("BY");
        do {
            OrderItem item;
            item.expr = parse_expression();
            if (accept_word("DESC") || accept_word("DESCENDING"))
                item.descending = true;
            else if (!accept_word("ASC"))
                accept_word("ASCENDING");
            clause.order.push_back(std::move(item));
        } while (accept_symbol(","));
    }

    // --- Desenler ---

    std::vector<PathPattern> parse_patterns() {
        std::vector<PathPattern> paths;
        do {
            paths.push_back(parse_path());
        } while (accept_symbol(","));
        return paths;
    }

    void count_element(Position pos) {
        if (++pattern_elements_ > limits_.max_pattern_elements)
            limit("pattern_elements", pos);
    }

    PathPattern parse_path() {
        PathPattern path;
        const Position pos = peek().pos;
        // p = ... ve shortestPath(...): adlandırılmış yol yok.
        if (at_name() && (at_symbol("=", 1) || at_symbol("(", 1))) {
            unsupported(peek().text, pos);
            return path;
        }
        path.nodes.push_back(parse_node());
        while (at_symbol("-") || at_symbol("<")) {
            path.rels.push_back(parse_rel());
            path.nodes.push_back(parse_node());
        }
        return path;
    }

    NodePattern parse_node() {
        NodePattern node;
        node.pos = peek().pos;
        count_element(node.pos);
        expect_symbol("(");
        if (at_variable())
            node.var = expect_variable();
        node.labels = parse_label_chain();
        if (at_symbol("{"))
            node.props = parse_map();
        else if (peek().kind == TokenKind::Param)
            unsupported("parameter map", peek().pos);
        expect_symbol(")");
        return node;
    }

    RelPattern parse_rel() {
        RelPattern rel;
        rel.pos = peek().pos;
        count_element(rel.pos);
        const bool left = accept_symbol("<");
        expect_symbol("-");
        if (accept_symbol("[")) {
            parse_rel_detail(rel);
            expect_symbol("]");
        }
        expect_symbol("-");
        const bool right = accept_symbol(">");
        rel.direction =
            left == right ? RelDirection::Both : (left ? RelDirection::Left : RelDirection::Right);
        return rel;
    }

    void parse_rel_detail(RelPattern& rel) {
        if (at_variable())
            rel.var = expect_variable();
        if (accept_symbol(":")) {
            rel.types.push_back(expect_label_name("relationship type"));
            while (accept_symbol("|")) {
                accept_symbol(":");
                rel.types.push_back(expect_label_name("relationship type"));
            }
        }
        if (accept_symbol("*"))
            parse_var_length(rel);
        if (at_symbol("{")) {
            if (rel.var_length)
                unsupported("properties on variable-length relationship", peek().pos);
            rel.props = parse_map();
        } else if (peek().kind == TokenKind::Param) {
            unsupported("parameter map", peek().pos);
        }
    }

    // Sınır derleme anında sabittir: planın maliyeti ve bellek üst sınırı parametreye bağlı olmasın.
    std::optional<int> parse_hop_bound() {
        const Token& token = peek();
        if (token.kind == TokenKind::Param) {
            unsupported("parameter in variable-length bound", token.pos);
            return std::nullopt;
        }
        if (token.kind != TokenKind::Integer)
            return std::nullopt;
        int value = 0;
        const auto [end, code] =
            std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
        (void)end;
        if (code != std::errc{} || static_cast<std::size_t>(value) > limits_.max_var_length_hops)
            limit("var_length_hops", token.pos);
        ++at_;
        return value;
    }

    void parse_var_length(RelPattern& rel) {
        const Position pos = peek().pos;
        rel.var_length = true;
        const std::optional<int> first = parse_hop_bound();
        if (error_)
            return;
        const bool range = accept_symbol("..");
        const std::optional<int> second = range ? parse_hop_bound() : first;
        if (error_)
            return;
        // SPEC: kesin *1..N. Sınırsız yol ve sıfır uzunluk yok.
        if (!second || (first && *first == 0)) {
            unsupported(second ? "zero-length path" : "unbounded variable-length path", pos);
            return;
        }
        rel.min_hops = first.value_or(1);
        rel.max_hops = *second;
        if (rel.min_hops > rel.max_hops)
            syntax("variable-length minimum is greater than maximum", pos);
        if (rel.max_hops < 1)
            unsupported("zero-length path", pos);
    }

    PropertyMap parse_map() {
        PropertyMap map;
        expect_symbol("{");
        if (accept_symbol("}"))
            return map;
        do {
            const Position pos = peek().pos;
            std::string key = expect_label_name("property key");
            expect_symbol(":");
            if (std::ranges::any_of(map, [&](const auto& entry) { return entry.first == key; }))
                syntax("duplicate property key", pos);
            map.emplace_back(std::move(key), parse_expression());
        } while (accept_symbol(","));
        expect_symbol("}");
        return map;
    }

    // --- İfadeler ---

    class DepthGuard {
    public:
        explicit DepthGuard(Parser& parser) : parser_(parser) {
            if (++parser_.depth_ > parser_.limits_.max_nesting)
                parser_.limit("nesting_depth", parser_.peek().pos);
        }
        ~DepthGuard() { --parser_.depth_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;

    private:
        Parser& parser_;
    };

    ExprPtr make_expr(Expr::Kind kind, Position pos) {
        auto expr = std::make_unique<Expr>();
        expr->kind = kind;
        expr->pos = pos;
        return expr;
    }

    // Yükseklik sınırı parantezsiz zincirleri de kapsar: `1+1+1+...` tek döngüyle sola derin ağaç kurar.
    ExprPtr with_args(ExprPtr expr, std::vector<ExprPtr> args) {
        int height = 0;
        for (const ExprPtr& arg : args)
            height = std::max(height, arg->height);
        expr->height = height + 1;
        expr->args = std::move(args);
        if (static_cast<std::size_t>(expr->height) > limits_.max_nesting)
            limit("expression_height", expr->pos);
        return expr;
    }

    ExprPtr make_unary(Expr::Kind kind, Position pos, ExprPtr operand) {
        std::vector<ExprPtr> args;
        args.push_back(std::move(operand));
        return with_args(make_expr(kind, pos), std::move(args));
    }

    ExprPtr make_binary(BinaryOp op, Position pos, ExprPtr left, ExprPtr right) {
        auto expr = make_expr(Expr::Kind::Binary, pos);
        expr->op = op;
        std::vector<ExprPtr> args;
        args.push_back(std::move(left));
        args.push_back(std::move(right));
        return with_args(std::move(expr), std::move(args));
    }

    ExprPtr make_literal(QueryValue value, Position pos) {
        auto expr = make_expr(Expr::Kind::Literal, pos);
        expr->literal = std::move(value);
        return expr;
    }

    // Hata sonrası da ağaç kurulabilsin: tüm erişimler null denetimsizdir.
    ExprPtr bad_expr() { return make_literal(QueryValue{}, peek().pos); }

    ExprPtr parse_expression() {
        const DepthGuard guard(*this);
        return parse_or();
    }

    ExprPtr parse_or() {
        ExprPtr left = parse_and();
        while (at_word("OR") || at_word("XOR")) {
            const Position pos = peek().pos;
            if (at_word("XOR")) {
                unsupported("XOR", pos);
                return left;
            }
            ++at_;
            left = make_binary(BinaryOp::Or, pos, std::move(left), parse_and());
        }
        return left;
    }

    ExprPtr parse_and() {
        ExprPtr left = parse_not();
        while (at_word("AND")) {
            const Position pos = peek().pos;
            ++at_;
            left = make_binary(BinaryOp::And, pos, std::move(left), parse_not());
        }
        return left;
    }

    ExprPtr parse_not() {
        const Position pos = peek().pos;
        if (!accept_word("NOT"))
            return parse_comparison();
        const DepthGuard guard(*this);
        return make_unary(Expr::Kind::Not, pos, parse_not());
    }

    std::optional<BinaryOp> comparison_op() const {
        constexpr std::array<std::pair<std::string_view, BinaryOp>, 6> kOps = {{{"=", BinaryOp::Eq},
                                                                                {"<>", BinaryOp::Ne},
                                                                                {"<=", BinaryOp::Le},
                                                                                {">=", BinaryOp::Ge},
                                                                                {"<", BinaryOp::Lt},
                                                                                {">", BinaryOp::Gt}}};
        for (const auto& [symbol, op] : kOps)
            if (at_symbol(symbol))
                return op;
        return std::nullopt;
    }

    ExprPtr parse_comparison() {
        ExprPtr left = parse_predicate();
        const std::optional<BinaryOp> op = comparison_op();
        if (!op)
            return left;
        const Position pos = peek().pos;
        ++at_;
        ExprPtr result = make_binary(*op, pos, std::move(left), parse_predicate());
        // openCypher a < b < c'yi AND'e açar; sola bağlamak sessizce yanlış sonuç verirdi.
        if (comparison_op())
            unsupported("chained comparison", peek().pos);
        return result;
    }

    std::optional<BinaryOp> accept_string_or_list_predicate() {
        if (accept_word("IN"))
            return BinaryOp::In;
        if (accept_word("CONTAINS"))
            return BinaryOp::Contains;
        if (at_word("STARTS")) {
            ++at_;
            expect_word("WITH");
            return BinaryOp::StartsWith;
        }
        if (at_word("ENDS")) {
            ++at_;
            expect_word("WITH");
            return BinaryOp::EndsWith;
        }
        return std::nullopt;
    }

    ExprPtr parse_predicate() {
        ExprPtr left = parse_additive();
        while (!at_end()) {
            const Position pos = peek().pos;
            if (accept_word("IS")) {
                const bool negated = accept_word("NOT");
                expect_word("NULL");
                left = make_unary(negated ? Expr::Kind::IsNotNull : Expr::Kind::IsNull, pos, std::move(left));
                continue;
            }
            const std::optional<BinaryOp> op = accept_string_or_list_predicate();
            if (!op)
                break;
            left = make_binary(*op, pos, std::move(left), parse_additive());
        }
        return left;
    }

    ExprPtr parse_additive() {
        ExprPtr left = parse_multiplicative();
        while (at_symbol("+") || at_symbol("-")) {
            const Position pos = peek().pos;
            const BinaryOp op = at_symbol("+") ? BinaryOp::Add : BinaryOp::Sub;
            ++at_;
            left = make_binary(op, pos, std::move(left), parse_multiplicative());
        }
        return left;
    }

    std::optional<BinaryOp> accept_multiplicative() {
        if (accept_symbol("*"))
            return BinaryOp::Mul;
        if (accept_symbol("/"))
            return BinaryOp::Div;
        if (accept_symbol("%"))
            return BinaryOp::Mod;
        return std::nullopt;
    }

    ExprPtr parse_multiplicative() {
        ExprPtr left = parse_unary();
        while (true) {
            const Position pos = peek().pos;
            const std::optional<BinaryOp> op = accept_multiplicative();
            if (!op)
                return left;
            left = make_binary(*op, pos, std::move(left), parse_unary());
        }
    }

    ExprPtr parse_unary() {
        const Position pos = peek().pos;
        if (!at_symbol("+") && !at_symbol("-"))
            return parse_postfix();
        const DepthGuard guard(*this);
        if (accept_symbol("+"))
            return parse_unary();
        ++at_;
        // En küçük int64 yalnız eksi işaretiyle yazılabilir: rakamlar tek başına taşar.
        if (peek().kind == TokenKind::Integer)
            return parse_integer("-" + peek().text, pos);
        return make_unary(Expr::Kind::Negate, pos, parse_unary());
    }

    ExprPtr parse_integer(const std::string& text, Position pos) {
        std::int64_t value = 0;
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
        (void)end;
        if (code != std::errc{}) {
            syntax("integer literal out of range", pos);
            return bad_expr();
        }
        ++at_;
        return make_literal(value, pos);
    }

    ExprPtr parse_float(const Token& token) {
        double value = 0;
        const auto [end, code] =
            std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
        (void)end;
        // Sıfıra yuvarlanacak kadar küçük sayı da aralık dışıdır: sessizce sıfır olmaz.
        if (code != std::errc{}) {
            syntax("float literal out of range", token.pos);
            return bad_expr();
        }
        ++at_;
        return make_literal(value, token.pos);
    }

    ExprPtr parse_postfix() {
        ExprPtr atom = parse_atom();
        if (at_symbol("[")) {
            unsupported("list indexing", peek().pos);
            return atom;
        }
        if (at_symbol(".") && atom->kind == Expr::Kind::Var) {
            ++at_;
            auto property = make_expr(Expr::Kind::Property, atom->pos);
            property->name = expect_label_name("property key");
            std::vector<ExprPtr> args;
            args.push_back(std::move(atom));
            atom = with_args(std::move(property), std::move(args));
        } else if (at_symbol(":") && atom->kind == Expr::Kind::Var) {
            auto has_labels = make_expr(Expr::Kind::HasLabels, atom->pos);
            has_labels->labels = parse_label_chain();
            std::vector<ExprPtr> args;
            args.push_back(std::move(atom));
            atom = with_args(std::move(has_labels), std::move(args));
        }
        if (at_symbol(".") || at_symbol("["))
            unsupported("chained property access", peek().pos);
        return atom;
    }

    ExprPtr parse_atom() {
        const Token& token = peek();
        switch (token.kind) {
        case TokenKind::Integer:
            return parse_integer(token.text, token.pos);
        case TokenKind::Float:
            return parse_float(token);
        case TokenKind::String: {
            ExprPtr literal = make_literal(token.text, token.pos);
            ++at_;
            return literal;
        }
        case TokenKind::Param:
            return parse_param(token);
        case TokenKind::Word:
        case TokenKind::QuotedName:
            return parse_word(token);
        case TokenKind::Symbol:
            return parse_symbol_atom(token);
        case TokenKind::End:
            break;
        }
        syntax_here("expected expression");
        return bad_expr();
    }

    ExprPtr parse_param(const Token& token) {
        auto param = make_expr(Expr::Kind::Param, token.pos);
        param->name = token.text;
        ++at_;
        return param;
    }

    ExprPtr parse_symbol_atom(const Token& token) {
        if (accept_symbol("(")) {
            ExprPtr inner = parse_expression();
            expect_symbol(")");
            return inner;
        }
        if (at_symbol("["))
            return parse_list(token.pos);
        if (at_symbol("{")) {
            unsupported("map literal", token.pos);
            return bad_expr();
        }
        syntax_here("expected expression");
        return bad_expr();
    }

    ExprPtr parse_list(Position pos) {
        expect_symbol("[");
        auto list = make_expr(Expr::Kind::List, pos);
        std::vector<ExprPtr> items;
        if (!at_symbol("]")) {
            do {
                items.push_back(parse_expression());
            } while (accept_symbol(","));
        }
        expect_symbol("]");
        return with_args(std::move(list), std::move(items));
    }

    ExprPtr parse_word(const Token& token) {
        const Position pos = token.pos;
        if (token.kind == TokenKind::Word) {
            if (at_word("TRUE") || at_word("FALSE")) {
                const bool value = at_word("TRUE");
                ++at_;
                return make_literal(value, pos);
            }
            if (accept_word("NULL"))
                return make_literal(QueryValue{}, pos);
            if (at_word("CASE")) {
                unsupported("CASE", pos);
                return bad_expr();
            }
            if (at_symbol("(", 1))
                return parse_call(token);
            // EXISTS { ... }, COUNT { ... }: alt sorgu yok (SPEC-graph "Yok").
            if (at_symbol("{", 1)) {
                unsupported("subquery", pos);
                return bad_expr();
            }
        }
        if (!at_variable()) {
            syntax_here("expected expression");
            return bad_expr();
        }
        auto var = make_expr(Expr::Kind::Var, pos);
        var->name = expect_variable();
        return var;
    }

    std::optional<AggregateFn> aggregate_named(std::string_view name) const {
        constexpr std::array<std::pair<std::string_view, AggregateFn>, 6> kAggregates = {
            {{"COUNT", AggregateFn::Count},
             {"SUM", AggregateFn::Sum},
             {"MIN", AggregateFn::Min},
             {"MAX", AggregateFn::Max},
             {"AVG", AggregateFn::Avg},
             {"COLLECT", AggregateFn::Collect}}};
        for (const auto& [word, fn] : kAggregates)
            if (same_word(name, word))
                return fn;
        return std::nullopt;
    }

    std::optional<Function> function_named(std::string_view name) const {
        constexpr std::array<std::pair<std::string_view, Function>, 3> kFunctions = {
            {{"LABELS", Function::Labels}, {"TYPE", Function::Type}, {"ID", Function::Id}}};
        for (const auto& [word, fn] : kFunctions)
            if (same_word(name, word))
                return fn;
        return std::nullopt;
    }

    ExprPtr parse_call(const Token& token) {
        const std::string name = token.text;
        const Position pos = token.pos;
        if (const std::optional<AggregateFn> aggregate = aggregate_named(name))
            return parse_aggregate(*aggregate, pos);
        const std::optional<Function> function = function_named(name);
        if (!function) {
            // Kullanıcı fonksiyonu ve desteklenmeyen yerleşikler: exists, shortestPath, size...
            unsupported(name, pos);
            return bad_expr();
        }
        at_ += 2;  // ad ve (
        auto call = make_expr(Expr::Kind::Call, pos);
        call->fn = *function;
        std::vector<ExprPtr> args;
        args.push_back(parse_expression());
        if (!at_symbol(")"))
            syntax_here("expected ')': this function takes one argument");
        expect_symbol(")");
        return with_args(std::move(call), std::move(args));
    }

    ExprPtr parse_aggregate(AggregateFn fn, Position pos) {
        at_ += 2;  // ad ve (
        auto call = make_expr(Expr::Kind::Aggregate, pos);
        call->aggregate = fn;
        if (fn == AggregateFn::Count && accept_symbol("*")) {
            call->aggregate = AggregateFn::CountStar;
            expect_symbol(")");
            return call;
        }
        call->distinct = accept_word("DISTINCT");
        std::vector<ExprPtr> args;
        args.push_back(parse_expression());
        expect_symbol(")");
        return with_args(std::move(call), std::move(args));
    }

    std::vector<Token> tokens_;
    std::string_view source_;
    std::size_t at_ = 0;
    std::size_t depth_ = 0;
    std::size_t pattern_elements_ = 0;
    std::optional<Error> error_;
    QueryLimits limits_;
};

}  // namespace

Result<Query> parse_query(const char* data, std::size_t size, const QueryLimits& limits) {
    if (size > limits.max_text_bytes)
        return std::unexpected(located_error(Error::Code::LimitExceeded, "graph.limit_exceeded", Position{},
                                             {{"limit", "query_bytes"}}));
    const std::string_view source = size == 0 ? std::string_view() : std::string_view(data, size);
    Result<std::vector<Token>> tokens = tokenize(source);
    if (!tokens)
        return std::unexpected(std::move(tokens).error());
    return Parser(std::move(*tokens), source, limits).run();
}

}  // namespace noteapp::graph::detail::cypher
